// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "addons.h"

#include "format.h"
#include "hostprocess.h"
#include "signature.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

namespace {

const QStringList kConditions = {
    QStringLiteral("mounted"), QStringLiteral("unmounted"), QStringLiteral("removable"), QStringLiteral("internal"),
    QStringLiteral("encrypted"), QStringLiteral("unlocked"), QStringLiteral("locked"), QStringLiteral("has-health"),
};
const QStringList kPlaceholders = {
    QStringLiteral("device"), QStringLiteral("disk"), QStringLiteral("mountpoint"), QStringLiteral("label"),
    QStringLiteral("uuid"), QStringLiteral("fstype"), QStringLiteral("size"), QStringLiteral("model"), QStringLiteral("home"),
};
// Placeholders whose values come from the drive, so whoever made the drive picked them.
const QStringList kDriveValues = {
    QStringLiteral("mountpoint"), QStringLiteral("label"), QStringLiteral("uuid"), QStringLiteral("fstype"), QStringLiteral("model"),
};

// Programs that change what an action can do. Every part of the command is checked, so
// "env sudo ..." or "konsole -e bash ..." count too.
const QStringList kAdmin = {
    QStringLiteral("pkexec"), QStringLiteral("sudo"), QStringLiteral("sudoedit"), QStringLiteral("doas"), QStringLiteral("run0"),
    QStringLiteral("su"), QStringLiteral("kdesu"), QStringLiteral("kdesudo"), QStringLiteral("gksu"), QStringLiteral("gksudo"),
    QStringLiteral("lxsu"), QStringLiteral("lxsudo"), QStringLiteral("lxqt-sudo"), QStringLiteral("beesu"),
    QStringLiteral("systemd-run"), QStringLiteral("machinectl"),
};
const QStringList kAnything = {
    // shells
    QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("zsh"), QStringLiteral("fish"), QStringLiteral("dash"),
    QStringLiteral("ksh"), QStringLiteral("mksh"), QStringLiteral("oksh"), QStringLiteral("csh"), QStringLiteral("tcsh"),
    QStringLiteral("yash"), QStringLiteral("nu"), QStringLiteral("elvish"), QStringLiteral("xonsh"), QStringLiteral("pwsh"),
    QStringLiteral("busybox"), QStringLiteral("toybox"),
    // programs that run a command line or a script handed to them
    QStringLiteral("env"), QStringLiteral("xargs"), QStringLiteral("watch"), QStringLiteral("script"), QStringLiteral("flock"),
    QStringLiteral("parallel"), QStringLiteral("expect"), QStringLiteral("awk"), QStringLiteral("gawk"), QStringLiteral("mawk"),
    QStringLiteral("nawk"), QStringLiteral("node"), QStringLiteral("nodejs"), QStringLiteral("deno"), QStringLiteral("bun"),
    QStringLiteral("irb"), QStringLiteral("Rscript"), QStringLiteral("julia"), QStringLiteral("java"),
};
const QStringList kNetwork = {
    QStringLiteral("curl"), QStringLiteral("wget"), QStringLiteral("wget2"), QStringLiteral("aria2c"), QStringLiteral("axel"),
    QStringLiteral("ssh"), QStringLiteral("scp"), QStringLiteral("sftp"), QStringLiteral("nc"), QStringLiteral("ncat"),
    QStringLiteral("netcat"), QStringLiteral("socat"), QStringLiteral("telnet"), QStringLiteral("ftp"), QStringLiteral("lftp"),
    QStringLiteral("rclone"),
};
const QStringList kDeletes = {
    QStringLiteral("rm"), QStringLiteral("rmdir"), QStringLiteral("shred"), QStringLiteral("srm"), QStringLiteral("dd"),
    QStringLiteral("wipefs"), QStringLiteral("blkdiscard"), QStringLiteral("truncate"), QStringLiteral("mkswap"),
    QStringLiteral("mke2fs"), QStringLiteral("sgdisk"), QStringLiteral("sfdisk"), QStringLiteral("fdisk"), QStringLiteral("gdisk"),
    QStringLiteral("cfdisk"), QStringLiteral("parted"), QStringLiteral("cryptsetup"),
};

// The key that signs the online add-on list. It's on the maintainer's PC only, so the
// list can't be changed by someone who gets into the GitHub account.
const QStringList kCatalogKeys = {
    QStringLiteral("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIDQf3ECvgg/mFU909xd37joeI3/yW1X1v1gYYwcIfhir DannyS124"),
};

QSettings settings()
{
    return QSettings(QStringLiteral("diskforge"), QStringLiteral("addons"));
}

QString hashText(const Addon &addon)
{
    return QString::fromLatin1(addon.fileHash.toHex());
}

QString firstMount(const Disk &disk, const Volume *volume)
{
    if (volume)
        return volume->mounts().value(0);
    for (const Volume &v : disk.volumes) {
        if (!v.mounts().isEmpty())
            return v.mounts().first();
    }
    return {};
}

bool conditionHolds(const QString &c, const Disk &disk, const Volume *v)
{
    const bool removable = disk.removable || disk.bus == QLatin1String("usb");
    if (c == QLatin1String("mounted"))
        return !firstMount(disk, v).isEmpty();
    if (c == QLatin1String("unmounted"))
        return firstMount(disk, v).isEmpty();
    if (c == QLatin1String("removable"))
        return removable;
    if (c == QLatin1String("internal"))
        return !removable && !disk.isLoop;
    if (c == QLatin1String("encrypted"))
        return v && v->encrypted;
    if (c == QLatin1String("unlocked"))
        return v && v->encrypted && !v->cleartextPath.isEmpty();
    if (c == QLatin1String("locked"))
        return v && v->encrypted && v->cleartextPath.isEmpty();
    if (c == QLatin1String("has-health"))
        return disk.health.state != Health::State::Unknown;
    if (c.startsWith(QLatin1String("filesystem:")))
        return v && c.mid(11).split(QLatin1Char('|')).contains(v->effectiveFsType());
    return false;
}

using host::inFlatpak;
using host::programExists;
// Runs a command on the host and waits, for quick checks.
int runOnHost(QStringList command)
{
    return host::run(std::move(command));
}

// Wraps the command so it runs in a terminal window the user can watch.
QStringList inTerminal(const QStringList &argv)
{
    const QString preferred = qEnvironmentVariable("TERMINAL");
    const QList<QPair<QString, QStringList>> terminals = {
        {QStringLiteral("konsole"), {QStringLiteral("--hold"), QStringLiteral("-e")}},
        {QStringLiteral("kitty"), {QStringLiteral("--hold")}},
        {QStringLiteral("alacritty"), {QStringLiteral("--hold"), QStringLiteral("-e")}},
        {QStringLiteral("foot"), {QStringLiteral("--hold")}},
        {QStringLiteral("gnome-terminal"), {QStringLiteral("--")}},
        {QStringLiteral("xterm"), {QStringLiteral("-hold"), QStringLiteral("-e")}},
    };
    for (const auto &[name, args] : terminals) {
        if (!preferred.isEmpty() && preferred.section(QLatin1Char('/'), -1) != name)
            continue;
        if (programExists(name))
            return QStringList{name} + args + argv;
    }
    if (!preferred.isEmpty())
        return QStringList{preferred, QStringLiteral("-e")} + argv;
    return {};
}

// How a placeholder's value is described when it can't be used.
QString what(const QString &placeholder)
{
    if (placeholder == QLatin1String("label"))
        return QObject::tr("name");
    if (placeholder == QLatin1String("model"))
        return QObject::tr("model name");
    if (placeholder == QLatin1String("uuid"))
        return QObject::tr("UUID");
    if (placeholder == QLatin1String("fstype"))
        return QObject::tr("file system type");
    if (placeholder == QLatin1String("mountpoint"))
        return QObject::tr("folder");
    return QLatin1Char('{') + placeholder + QLatin1Char('}');
}

// {name} or {ask:id} / {setting:id}
const QRegularExpression &placeholderPattern()
{
    static const QRegularExpression pattern(QStringLiteral("\\{([a-z]+)(?::([a-z0-9_-]+))?\\}"));
    return pattern;
}

const AddonField *findField(const QVector<AddonField> &fields, const QString &id)
{
    for (const AddonField &f : fields) {
        if (f.id == id)
            return &f;
    }
    return nullptr;
}

// A value for a field has to be one the field allows. Choices and checkbox values must be
// the add-on's current ones: a saved setting may be from an older version of it.
QString checkValue(const AddonField &field, const QString &value)
{
    if (hasHiddenCharacters(value))
        return QObject::tr("\"%1\" has hidden characters in it, so this wasn't run.").arg(field.label);
    switch (field.type) {
    case AddonField::Type::Choice:
        for (const auto &choice : field.choices) {
            if (choice.second == value)
                return {};
        }
        return QObject::tr("\"%1\" isn't one of its choices anymore. Pick it again in the add-on's settings.").arg(field.label);
    case AddonField::Type::Check:
        return value == field.on || value == field.off
            ? QString() : QObject::tr("\"%1\" isn't set right anymore. Set it again in the add-on's settings.").arg(field.label);
    case AddonField::Type::Number: {
        if (value.isEmpty())
            return {};
        bool ok = false;
        const qint64 n = value.toLongLong(&ok);
        return ok && n >= field.min && n <= field.max
            ? QString() : QObject::tr("\"%1\" has to be a whole number from %2 to %3.").arg(field.label).arg(field.min).arg(field.max);
    }
    default:
        return {};
    }
}

// The form's answers and the add-on's settings, for fill().
struct Inputs {
    const Addon *addon = nullptr;
    const AddonAction *action = nullptr;
    QMap<QString, QString> answers;
};

// Fills in placeholders, in one pass over the add-on's own text: values that went in are
// never looked at again. For command arguments (not the confirm text), a value that begins
// an argument can't begin with "-", or the program could read it as an option, unless the
// add-on file itself lists it (a choice or a checkbox value).
QStringList fill(const QStringList &args, const Disk &disk, const Volume *volume, bool arguments, QString *error,
                 const Inputs *inputs = nullptr)
{
    auto driveValue = [&](const QString &name) -> QString {
        if (name == QLatin1String("device"))
            return volume ? volume->device : disk.device;
        if (name == QLatin1String("disk"))
            return disk.device;
        if (name == QLatin1String("mountpoint"))
            return firstMount(disk, volume);
        if (name == QLatin1String("label"))
            return volume ? (!volume->label.isEmpty() ? volume->label : !volume->partName.isEmpty() ? volume->partName : shortDevice(volume->device))
                          : disk.model;
        if (name == QLatin1String("uuid"))
            return volume ? volume->uuid : QString();
        if (name == QLatin1String("fstype"))
            return volume ? volume->effectiveFsType() : QString();
        if (name == QLatin1String("size"))
            return QString::number(volume ? volume->size : disk.size);
        if (name == QLatin1String("model"))
            return disk.model;
        if (name == QLatin1String("home"))
            return QDir::homePath();
        return {};
    };
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QStringList();
    };
    QStringList out;
    for (const QString &arg : args) {
        QString result;
        qsizetype last = 0;
        bool wholeInput = false; // the argument is exactly one {ask:x} or {setting:x}
        for (auto it = placeholderPattern().globalMatch(arg); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            const QString name = m.captured(1), id = m.captured(2);
            QString v;
            if (name == QLatin1String("ask") || name == QLatin1String("setting")) {
                const bool ask = name == QLatin1String("ask");
                const AddonField *field = inputs ? findField(ask ? inputs->action->ask : inputs->addon->settings, id) : nullptr;
                if (!field)
                    return fail(QObject::tr("{%1:%2} isn't available here").arg(name, id));
                v = ask ? inputs->answers.value(id, Addons::fieldDefault(*field, disk, volume)) : Addons::setting(*inputs->addon, id);
                const QString problem = checkValue(*field, v);
                if (!problem.isEmpty())
                    return fail(problem);
                if (field->typed() && arguments && m.capturedStart() == 0 && v.startsWith(QLatin1Char('-')))
                    return fail(QObject::tr("\"%1\" starts with \"-\", so the program could take it for an option. It wasn't run.")
                                    .arg(field->label));
                wholeInput = m.capturedStart() == 0 && m.capturedEnd() == arg.size();
            } else {
                v = driveValue(name);
                if (v.isEmpty())
                    return fail(name == QLatin1String("mountpoint") ? QObject::tr("Mount it first") : QObject::tr("{%1} isn't available here").arg(name));
                if (kDriveValues.contains(name)) {
                    // Like UDisks does for folder names under /run/media: a name is one folder name.
                    if (name != QLatin1String("mountpoint"))
                        v.replace(QLatin1Char('/'), QLatin1Char('_'));
                    if (hasHiddenCharacters(v))
                        return fail(QObject::tr("The drive's %1 has hidden characters in it, so this wasn't run. Rename the drive first.").arg(what(name)));
                    if (v == QLatin1String(".") || v == QLatin1String(".."))
                        return fail(QObject::tr("The drive's %1 is \"%2\", which would point to another folder, so this wasn't run. "
                                                "Rename the drive first.").arg(what(name), v));
                }
                if (arguments && m.capturedStart() == 0 && v.startsWith(QLatin1Char('-')))
                    return fail(QObject::tr("The drive's %1 starts with \"-\", so the program could take it for an option. "
                                            "It wasn't run; rename the drive first.").arg(what(name)));
            }
            result += arg.mid(last, m.capturedStart() - last) + v;
            last = m.capturedEnd();
        }
        result += arg.mid(last);
        // An empty answer leaves the whole argument out, so a checkbox can add or drop a flag.
        if (arguments && wholeInput && result.isEmpty())
            continue;
        out << result;
    }
    return out;
}

// Programs that run as root, or that need a terminal to ask for a password.
const QStringList kNeedsTerminal = {
    QStringLiteral("sudo"), QStringLiteral("sudoedit"), QStringLiteral("su"), QStringLiteral("doas"),
};

const QStringList kPaletteRoles = {
    QStringLiteral("window"), QStringLiteral("base"), QStringLiteral("alternate"), QStringLiteral("button"), QStringLiteral("text"),
    QStringLiteral("highlight"), QStringLiteral("highlighted_text"), QStringLiteral("link"), QStringLiteral("mid"),
};
const QStringList kColorRoles = {
    QStringLiteral("partition"), QStringLiteral("free"), QStringLiteral("selection"), QStringLiteral("good"), QStringLiteral("warning"),
    QStringLiteral("danger"), QStringLiteral("muted"), QStringLiteral("map_good"), QStringLiteral("map_slow"), QStringLiteral("map_retry"),
    QStringLiteral("map_bad"), QStringLiteral("map_unread"), QStringLiteral("usage_small_files"), QStringLiteral("usage_file"),
    QStringLiteral("encrypted"),
};

// "#1a2b3c" -> 0x1a2b3c. Only full, opaque colors: a theme can't make anything see-through.
bool parseColor(const QJsonValue &value, quint32 *out)
{
    static const QRegularExpression hex(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    const QString s = value.toString();
    if (!hex.match(s).hasMatch())
        return false;
    *out = s.mid(1).toUInt(nullptr, 16);
    return true;
}

AddonTheme parseTheme(const QJsonObject &o, QString *error)
{
    AddonTheme theme;
    auto colors = [&](const char *key, const QStringList &known, QMap<QString, quint32> *into) {
        const QJsonObject group = o.value(QLatin1String(key)).toObject();
        static const QRegularExpression fsName(QStringLiteral("^[a-z0-9_]{1,24}$"));
        for (auto it = group.constBegin(); it != group.constEnd(); ++it) {
            if (known.isEmpty() ? !fsName.match(it.key()).hasMatch() : !known.contains(it.key()))
                continue; // a newer DiskForge's name, or nonsense
            quint32 c = 0;
            if (!parseColor(it.value(), &c)) {
                *error = QObject::tr("The theme's \"%1\" isn't a color like #1a2b3c").arg(it.key());
                return false;
            }
            into->insert(it.key(), c);
        }
        return true;
    };
    if (!colors("palette", kPaletteRoles, &theme.palette) || !colors("colors", kColorRoles, &theme.colors)
        || !colors("filesystems", {}, &theme.filesystems))
        return {};
    const QJsonArray usage = o.value(QStringLiteral("usage")).toArray();
    for (const QJsonValue &v : usage) {
        quint32 c = 0;
        if (!parseColor(v, &c) || theme.usage.size() >= 32) {
            *error = QObject::tr("The theme's \"usage\" has to be a list of up to 32 colors like #1a2b3c");
            return {};
        }
        theme.usage << c;
    }
    return theme;
}

// The fields of a form ("ask") or of an add-on's settings.
QVector<AddonField> parseFields(const QJsonArray &list, bool settings, QString *error)
{
    static const QRegularExpression idPattern(QStringLiteral("^[a-z][a-z0-9_-]*$"));
    static const QMap<QString, AddonField::Type> types = {
        {QStringLiteral("text"), AddonField::Type::Text},     {QStringLiteral("number"), AddonField::Type::Number},
        {QStringLiteral("choice"), AddonField::Type::Choice}, {QStringLiteral("check"), AddonField::Type::Check},
        {QStringLiteral("folder"), AddonField::Type::Folder}, {QStringLiteral("file"), AddonField::Type::File},
    };
    QVector<AddonField> fields;
    auto fail = [error](const QString &message) {
        *error = message;
        return QVector<AddonField>();
    };
    if (list.size() > 20)
        return fail(QObject::tr("A form can have at most 20 fields"));
    for (const QJsonValue &value : list) {
        const QJsonObject j = value.toObject();
        AddonField f;
        f.id = j.value(QStringLiteral("id")).toString();
        if (!idPattern.match(f.id).hasMatch() || findField(fields, f.id))
            return fail(QObject::tr("Each field needs its own \"id\": lowercase letters, digits, - and _ (\"%1\")").arg(f.id));
        const QString type = j.value(QStringLiteral("type")).toString(QStringLiteral("text"));
        if (!types.contains(type))
            return fail(QObject::tr("\"%1\" has an unknown type \"%2\"").arg(f.id, type));
        f.type = types.value(type);
        f.label = j.value(QStringLiteral("label")).toString(f.id);
        f.defaultValue = j.value(QStringLiteral("default")).toVariant().toString();
        f.on = j.value(QStringLiteral("on")).toString();
        f.off = j.value(QStringLiteral("off")).toString();
        for (const QJsonValue &c : j.value(QStringLiteral("choices")).toArray()) {
            const QJsonObject choice = c.toObject();
            if (c.isString())
                f.choices.push_back({c.toString(), c.toString()});
            else
                f.choices.push_back({choice.value(QStringLiteral("label")).toString(), choice.value(QStringLiteral("value")).toString()});
        }
        if (f.type == AddonField::Type::Number) { // only numbers have limits; others keep the defaults
            f.min = j.value(QStringLiteral("min")).toInteger(0);
            f.max = j.value(QStringLiteral("max")).toInteger(1000000);
        }

        QStringList texts = {f.label, f.defaultValue, f.on, f.off};
        for (const auto &choice : std::as_const(f.choices))
            texts << choice.first << choice.second;
        for (const QString &t : std::as_const(texts)) {
            if (hasHiddenCharacters(t))
                return fail(QObject::tr("\"%1\" has hidden characters in it").arg(f.id));
        }
        if (f.type == AddonField::Type::Choice && f.choices.isEmpty())
            return fail(QObject::tr("\"%1\" needs \"choices\"").arg(f.id));
        if (f.type == AddonField::Type::Number && (f.min < 0 || f.max < f.min))
            // "-5" at the start of an argument would look like an option.
            return fail(QObject::tr("\"%1\" needs a \"min\" of 0 or more, and a \"max\" at least as big").arg(f.id));
        // Defaults can use the drive's placeholders (settings only {home}: they're not for one drive).
        for (auto it = placeholderPattern().globalMatch(f.defaultValue); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            const QString name = m.captured(1);
            if (!m.captured(2).isEmpty() || !kPlaceholders.contains(name) || (settings && name != QLatin1String("home")))
                return fail(QObject::tr("The default of \"%1\" can't use {%2}").arg(f.id, m.captured(0).mid(1).chopped(1)));
        }
        fields << f;
    }
    return fields;
}

// A field's default before placeholders are filled in.
QString plainDefault(const AddonField &f)
{
    switch (f.type) {
    case AddonField::Type::Check:
        return f.defaultValue == QLatin1String("true") || (!f.on.isEmpty() && f.defaultValue == f.on) ? f.on : f.off;
    case AddonField::Type::Choice:
        for (const auto &choice : f.choices) {
            if (choice.second == f.defaultValue)
                return choice.second;
        }
        return f.choices.value(0).second;
    case AddonField::Type::Number:
        return f.defaultValue.isEmpty() ? QString::number(f.min) : f.defaultValue;
    default:
        return f.defaultValue;
    }
}

} // namespace

QString Addons::userDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/diskforge/addons");
}

QString Addons::systemDir()
{
    return QStringLiteral("/usr/share/diskforge/addons");
}

QStringList Addons::searchDirs()
{
    return {userDir(), systemDir()};
}

Addon Addons::parse(const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        Addon a;
        a.file = file;
        a.error = f.errorString();
        return a;
    }
    return parseData(f.read(kMaxDownload * 4), file);
}

Addon Addons::parseData(const QByteArray &json, const QString &file)
{
    Addon a;
    a.file = file;
    a.fileHash = QCryptographicHash::hash(json, QCryptographicHash::Sha256);
    QJsonParseError parseError;
    const QJsonObject o = QJsonDocument::fromJson(json, &parseError).object();
    if (parseError.error != QJsonParseError::NoError) {
        a.error = QObject::tr("Not valid JSON: %1").arg(parseError.errorString());
        return a;
    }
    a.id = o.value(QStringLiteral("id")).toString();
    a.name = o.value(QStringLiteral("name")).toString(a.id);
    a.version = o.value(QStringLiteral("version")).toString();
    a.author = o.value(QStringLiteral("author")).toString();
    a.description = o.value(QStringLiteral("description")).toString();
    if (!QRegularExpression(QStringLiteral("^[a-z0-9][a-z0-9-]*$")).match(a.id).hasMatch()) {
        a.error = QObject::tr("\"id\" must be lowercase letters, digits and dashes");
        return a;
    }
    // Names end up in menus and questions; invisible characters (or a tab, which fakes a
    // shortcut column in a menu) could make them look like something else.
    if (hasHiddenCharacters(a.name) || hasHiddenCharacters(a.author)) {
        a.error = QObject::tr("The name and author can't have hidden characters");
        return a;
    }

    QString problem;
    a.settings = parseFields(o.value(QStringLiteral("settings")).toArray(), true, &problem);
    if (problem.isEmpty() && o.contains(QStringLiteral("theme")))
        a.theme = parseTheme(o.value(QStringLiteral("theme")).toObject(), &problem);
    if (!problem.isEmpty()) {
        a.error = problem;
        return a;
    }

    QVector<AddonAction> actions;
    for (const QJsonValue &value : o.value(QStringLiteral("actions")).toArray()) {
        const QJsonObject j = value.toObject();
        AddonAction act;
        act.index = int(actions.size());
        act.label = j.value(QStringLiteral("label")).toString();
        act.icon = j.value(QStringLiteral("icon")).toString();
        act.appliesTo = j.value(QStringLiteral("applies_to")).toString(QStringLiteral("volume"));
        act.terminal = j.value(QStringLiteral("terminal")).toBool();
        act.confirm = j.value(QStringLiteral("confirm")).toString();
        act.systemDisks = j.value(QStringLiteral("system_disks")).toBool();
        act.lookOnly = j.value(QStringLiteral("look_only")).toBool();
        for (const QJsonValue &w : j.value(QStringLiteral("when")).toArray())
            act.when << w.toString();
        for (const QJsonValue &c : j.value(QStringLiteral("command")).toArray())
            act.command << c.toString();
        const QString output = j.value(QStringLiteral("output")).toString();
        if (output == QLatin1String("terminal") || output == QLatin1String("window") || output == QLatin1String("none")) {
            act.terminal = output == QLatin1String("terminal");
            act.window = output == QLatin1String("window");
        } else if (!output.isEmpty()) {
            a.error = QObject::tr("\"output\" must be terminal, window or none");
            return a;
        }
        act.ask = parseFields(j.value(QStringLiteral("ask")).toArray(), false, &problem);
        if (!problem.isEmpty()) {
            a.error = problem;
            return a;
        }

        if (act.label.isEmpty() || act.command.isEmpty() || act.command.first().isEmpty()) {
            a.error = QObject::tr("Every action needs a \"label\" and a \"command\"");
            return a;
        }
        if (hasHiddenCharacters(act.label)) {
            a.error = QObject::tr("The label \"%1\" has hidden characters").arg(cleanName(act.label));
            return a;
        }
        // The run question shows the command; nothing in it may be invisible or flip the text.
        for (const QString &part : std::as_const(act.command) + QStringList{act.confirm, act.icon}) {
            if (hasHiddenCharacters(part)) {
                a.error = QObject::tr("\"%1\" has hidden characters in its command").arg(act.label);
                return a;
            }
        }
        for (const AddonAction &other : actions) {
            if (other.label == act.label) {
                a.error = QObject::tr("Two actions are called \"%1\"").arg(act.label);
                return a;
            }
        }
        if (!QStringList{QStringLiteral("volume"), QStringLiteral("disk"), QStringLiteral("free"), QStringLiteral("any")}.contains(act.appliesTo)) {
            a.error = QObject::tr("\"applies_to\" must be volume, disk, free or any");
            return a;
        }
        for (const QString &w : act.when) {
            if (!kConditions.contains(w) && !w.startsWith(QLatin1String("filesystem:"))) {
                a.error = QObject::tr("Unknown condition \"%1\"").arg(w);
                return a;
            }
        }
        for (const QString &arg : std::as_const(act.command)) {
            for (auto it = placeholderPattern().globalMatch(arg); it.hasNext();) {
                const QRegularExpressionMatch m = it.next();
                const QString name = m.captured(1), id = m.captured(2);
                const bool input = name == QLatin1String("ask") || name == QLatin1String("setting");
                if (input ? !findField(name == QLatin1String("ask") ? act.ask : a.settings, id) : (!id.isEmpty() || !kPlaceholders.contains(name))) {
                    a.error = QObject::tr("Unknown placeholder %1").arg(m.captured(0));
                    return a;
                }
            }
        }
        // The program itself can't come from the drive (like {mountpoint}/run.sh) or a form.
        if (QString(act.command.first()).remove(QStringLiteral("{home}")).contains(QLatin1Char('{'))) {
            a.error = QObject::tr("The program to run can't come from the drive or a form: only {home} can be used in the first "
                                  "part of \"command\"");
            return a;
        }
        AddonAction unboxed = act;
        unboxed.lookOnly = false;
        const AddonRisks risky = risks(unboxed, a.settings);
        if (act.lookOnly && !risky.admin.isEmpty()) {
            a.error = QObject::tr("\"look_only\" actions can't use %1: the sandbox doesn't allow admin power").arg(risky.admin);
            return a;
        }
        if (act.window) {
            for (const QString &part : std::as_const(act.command)) {
                if (kNeedsTerminal.contains(part.section(QLatin1Char('/'), -1))) {
                    a.error = QObject::tr("%1 asks for a password in a terminal: use \"output\": \"terminal\"").arg(part.section(QLatin1Char('/'), -1));
                    return a;
                }
            }
        }
        actions << act;
    }
    if (actions.isEmpty() && a.theme.isEmpty()) {
        a.error = QObject::tr("No actions");
        return a;
    }
    a.actions = actions;
    return a;
}

void Addons::load()
{
    m_addons.clear();
    QStringList seen;
    QSettings s = settings();
    // Until 0.5.0, trust was remembered per command. It's per add-on file now, so the old
    // entries go and each action asks once more.
    s.remove(QStringLiteral("trusted"));
    for (const QString &dir : searchDirs()) {
        const bool system = dir == systemDir();
        for (const QFileInfo &sub : QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const QString file = sub.filePath() + QStringLiteral("/addon.json");
            if (!QFileInfo::exists(file))
                continue;
            Addon a = parse(file);
            if (!a.id.isEmpty() && seen.contains(a.id))
                continue; // the user's copy wins over a system-wide one
            seen << a.id;
            a.enabled = s.value(QStringLiteral("enabled/") + a.id, true).toBool();
            a.systemWide = system;
            // DiskForge notes the checksum of every add-on it installs. One that doesn't
            // match was put there (or changed) some other way.
            a.outside = !system && s.value(QStringLiteral("installed/") + a.id).toString() != hashText(a);
            m_addons << a;
        }
    }
}

void Addons::setEnabled(const QString &id, bool enabled)
{
    QSettings s = settings();
    s.setValue(QStringLiteral("enabled/") + id, enabled);
    for (Addon &a : m_addons) {
        if (a.id == id)
            a.enabled = enabled;
    }
}

void Addons::accept(const QString &id, const QByteArray &fileHash)
{
    QSettings s = settings();
    for (Addon &a : m_addons) {
        if (a.id == id && a.fileHash == fileHash && !a.systemWide && a.error.isEmpty()) {
            s.setValue(QStringLiteral("installed/") + a.id, hashText(a));
            a.outside = false;
        }
    }
}

bool Addons::applies(const AddonAction &action, const Disk &disk, const Volume *volume, bool freeSpace)
{
    return whyNot(action, disk, volume, freeSpace).isEmpty();
}

QString Addons::whyNot(const AddonAction &action, const Disk &disk, const Volume *volume, bool freeSpace)
{
    // On the disk the system runs from, only look-only actions (in the sandbox) are offered.
    if (disk.isSystem && !(action.systemDisks && action.lookOnly))
        return QObject::tr("Not on the system disk: only look-only add-ons are offered there");
    const QString &to = action.appliesTo;
    if (to == QLatin1String("volume") && !volume)
        return QObject::tr("Pick a partition first");
    if (to == QLatin1String("disk") && (volume || freeSpace))
        return QObject::tr("Pick the whole drive first (its box on the left)");
    if (to == QLatin1String("free") && !freeSpace)
        return QObject::tr("Pick unallocated space first");
    for (const QString &c : action.when) {
        if (conditionHolds(c, disk, volume))
            continue;
        if (c == QLatin1String("mounted"))
            return QObject::tr("Mount it first");
        if (c == QLatin1String("unmounted"))
            return QObject::tr("Unmount it first");
        if (c == QLatin1String("removable"))
            return QObject::tr("Only for USB and other removable drives");
        if (c == QLatin1String("internal"))
            return QObject::tr("Only for internal drives");
        if (c == QLatin1String("encrypted"))
            return QObject::tr("Only for encrypted partitions");
        if (c == QLatin1String("unlocked"))
            return QObject::tr("Unlock it first");
        if (c == QLatin1String("locked"))
            return QObject::tr("Lock it first");
        if (c == QLatin1String("has-health"))
            return QObject::tr("Only for drives that report their health");
        if (c.startsWith(QLatin1String("filesystem:")))
            return QObject::tr("Only for %1").arg(c.mid(11).split(QLatin1Char('|')).join(QStringLiteral(", ")));
        return QObject::tr("Doesn't fit this selection");
    }
    return {};
}

QVector<QPair<const Addon *, const AddonAction *>> Addons::actionsFor(const Disk &disk, const Volume *volume, bool freeSpace) const
{
    QVector<QPair<const Addon *, const AddonAction *>> out;
    for (const Addon &a : m_addons) {
        if (!a.enabled || !a.error.isEmpty())
            continue;
        for (const AddonAction &act : a.actions) {
            if (applies(act, disk, volume, freeSpace))
                out.push_back({&a, &act});
        }
    }
    return out;
}

QStringList Addons::expand(const QStringList &args, const Disk &disk, const Volume *v, QString *error)
{
    return fill(args, disk, v, true, error);
}

QString Addons::expandText(const QString &text, const Disk &disk, const Volume *v, QString *error)
{
    return fill({text}, disk, v, false, error).value(0);
}

AddonRisks Addons::risksOf(const QStringList &parts)
{
    AddonRisks r;
    static const QRegularExpression versioned(QStringLiteral("^(python|pypy|lua|luajit|perl|php|ruby|tclsh|wish)[0-9.]*$"));
    for (const QString &part : parts) {
        const QString name = part.section(QLatin1Char('/'), -1);
        if (kAdmin.contains(name)) {
            if (r.admin.isEmpty())
                r.admin = name;
        } else if (kAnything.contains(name) || versioned.match(name).hasMatch()) {
            if (r.anything.isEmpty())
                r.anything = name;
        } else if (kNetwork.contains(name)) {
            if (r.network.isEmpty())
                r.network = name;
        } else if (kDeletes.contains(name) || name.startsWith(QLatin1String("mkfs"))) {
            if (r.deletes.isEmpty())
                r.deletes = name;
        }
    }
    if (r.deletes.isEmpty() && parts.contains(QLatin1String("-delete")))
        r.deletes = QStringLiteral("find -delete");
    return r;
}

AddonRisks Addons::risks(const AddonAction &action, const QVector<AddonField> &settings)
{
    if (action.lookOnly)
        return {}; // the sandbox stops all of it
    // What a form or a setting can put in counts as much as the command itself.
    QStringList parts = action.command;
    for (const QVector<AddonField> *fields : {&action.ask, &settings}) {
        for (const AddonField &f : *fields) {
            parts << f.defaultValue << f.on << f.off;
            for (const auto &choice : f.choices)
                parts << choice.second;
        }
    }
    return risksOf(parts);
}

QStringList Addons::fillCommand(const Addon &addon, const AddonAction &action, const Disk &disk, const Volume *volume,
                                const QMap<QString, QString> &answers, QString *error)
{
    const Inputs inputs{&addon, &action, answers};
    return fill(action.command, disk, volume, true, error, &inputs);
}

QString Addons::fieldDefault(const AddonField &field, const Disk &disk, const Volume *volume)
{
    const QString value = plainDefault(field);
    if (!field.typed())
        return value;
    QString error;
    const QString filled = fill({value}, disk, volume, false, &error).value(0);
    return error.isEmpty() ? filled : QString();
}

QString Addons::setting(const Addon &addon, const QString &field)
{
    const AddonField *f = findField(addon.settings, field);
    if (!f)
        return {};
    const QVariant saved = settings().value(QStringLiteral("settings/%1/%2").arg(addon.id, field));
    if (saved.isValid())
        return saved.toString();
    return QString(plainDefault(*f)).replace(QStringLiteral("{home}"), QDir::homePath());
}

void Addons::setSetting(const Addon &addon, const QString &field, const QString &value)
{
    QSettings s = settings();
    s.setValue(QStringLiteral("settings/%1/%2").arg(addon.id, field), value);
}

QString Addons::actionKey(const Addon &addon, const AddonAction &action)
{
    return addon.id + QLatin1Char('/') + QString::fromLatin1(QUrl::toPercentEncoding(action.label));
}

bool Addons::isPinned(const QString &key)
{
    return settings().value(QStringLiteral("pinned/") + key).toBool();
}

void Addons::setPinned(const QString &key, bool pinned)
{
    QSettings s = settings();
    if (pinned)
        s.setValue(QStringLiteral("pinned/") + key, true);
    else
        s.remove(QStringLiteral("pinned/") + key);
}

QString Addons::shortcut(const QString &key)
{
    return settings().value(QStringLiteral("shortcuts/") + key).toString();
}

void Addons::setShortcut(const QString &key, const QString &keys)
{
    QSettings s = settings();
    if (keys.isEmpty())
        s.remove(QStringLiteral("shortcuts/") + key);
    else
        s.setValue(QStringLiteral("shortcuts/") + key, keys);
}

bool Addons::isTrusted(const Addon &addon, const AddonAction &action)
{
    // Admin power, a shell, or an add-on that turned up from outside: always ask.
    if (addon.outside || risks(action, addon.settings).alwaysAsk())
        return false;
    return settings().value(QStringLiteral("allowed/%1-%2").arg(hashText(addon)).arg(action.index)).toBool();
}

bool Addons::isTrusted(const Addon &addon, const AddonAction &action, const QStringList &argv)
{
    // What the form put in counts: "Don't ask again" never covers admin power or a shell.
    if (!action.lookOnly && risksOf(argv).alwaysAsk())
        return false;
    return isTrusted(addon, action);
}

void Addons::trust(const Addon &addon, const AddonAction &action)
{
    QSettings s = settings();
    s.setValue(QStringLiteral("allowed/%1-%2").arg(hashText(addon)).arg(action.index), true);
}

QStringList Addons::sandboxed(const QStringList &argv, bool endWithParent)
{
    // Everything read-only, with its own empty /tmp and /run, so the sockets other programs
    // listen on (the session bus, Wayland, X11, ssh-agent...) aren't there. No network, a
    // /dev without the disks, its own process list, and no way to gain privileges. Mounted
    // drives under /run/media stay readable.
    return QStringList{
        QStringLiteral("bwrap"),
        QStringLiteral("--ro-bind"), QStringLiteral("/"), QStringLiteral("/"),
        QStringLiteral("--dev"), QStringLiteral("/dev"),
        QStringLiteral("--proc"), QStringLiteral("/proc"),
        QStringLiteral("--tmpfs"), QStringLiteral("/tmp"),
        QStringLiteral("--tmpfs"), QStringLiteral("/run"),
        QStringLiteral("--ro-bind-try"), QStringLiteral("/run/media"), QStringLiteral("/run/media"),
        QStringLiteral("--ro-bind-try"), QStringLiteral("/run/udev"), QStringLiteral("/run/udev"),
        QStringLiteral("--unshare-all"),
        QStringLiteral("--new-session"),
    } + (endWithParent ? QStringList{QStringLiteral("--die-with-parent")} : QStringList()) + QStringList{QStringLiteral("--")} + argv;
}

QStringList Addons::commandLine(const AddonAction &action, const QStringList &argv, QString *error)
{
    QStringList command = action.lookOnly ? sandboxed(argv) : argv;
    if (action.terminal) {
        command = inTerminal(command);
        if (command.isEmpty() && error)
            *error = QObject::tr("No terminal program found (install konsole, kitty or xterm)");
    }
    return command;
}

bool Addons::prepare(const AddonAction &action, const QStringList &argv, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (argv.isEmpty())
        return fail(QObject::tr("There's nothing to run"));
    if (inFlatpak() && QProcess::execute(QStringLiteral("flatpak-spawn"), {QStringLiteral("--host"), QStringLiteral("true")}) != 0)
        return fail(QObject::tr("Add-ons run programs outside the Flatpak, which it isn't allowed to do yet. To allow it, run:\n"
                                "flatpak override --user --talk-name=org.freedesktop.Flatpak " APP_ID));
    if (!programExists(argv.first()))
        return fail(QObject::tr("%1 isn't installed").arg(argv.first()));
    if (action.lookOnly) {
        if (!programExists(QStringLiteral("bwrap")))
            return fail(QObject::tr("Look-only add-ons run in a sandbox made with bubblewrap, which isn't installed.\n"
                                    "Install it with: sudo pacman -S bubblewrap"));
        // Some kernels don't allow sandboxes for normal users; then it doesn't run at all.
        if (runOnHost(sandboxed({QStringLiteral("true")})) != 0)
            return fail(QObject::tr("The sandbox for look-only add-ons doesn't work on this system, so it wasn't run."));
    }
    return true;
}

QStringList Addons::processCommand(const AddonAction &action, const QStringList &argv)
{
    QStringList command = action.lookOnly ? sandboxed(argv, true) : argv;
    // --watch-bus: the host side stops when DiskForge goes away.
    if (inFlatpak())
        command = QStringList{QStringLiteral("flatpak-spawn"), QStringLiteral("--host"), QStringLiteral("--watch-bus")} + command;
    return command;
}

bool Addons::run(const AddonAction &action, const QStringList &argv, QString *error)
{
    if (!prepare(action, argv, error))
        return false;
    QStringList full = commandLine(action, argv, error);
    if (full.isEmpty())
        return false;
    if (inFlatpak())
        full.prepend(QStringLiteral("--host"));
    const QString program = inFlatpak() ? QStringLiteral("flatpak-spawn") : full.takeFirst();
    if (!QProcess::startDetached(program, full)) {
        if (error)
            *error = QObject::tr("Couldn't start %1").arg(program);
        return false;
    }
    return true;
}

bool Addons::install(const QByteArray &data, QString *error, bool record)
{
    const Addon a = parseData(data, QString());
    if (!a.error.isEmpty()) {
        if (error)
            *error = a.error;
        return false;
    }
    const QString dir = userDir() + QLatin1Char('/') + a.id;
    QSaveFile f(dir + QStringLiteral("/addon.json"));
    if (!QDir().mkpath(dir) || !f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
        if (error)
            *error = QObject::tr("Couldn't save it in %1").arg(dir);
        return false;
    }
    // Noted, so it isn't flagged as added from outside.
    QSettings s = settings();
    if (record)
        s.setValue(QStringLiteral("installed/") + a.id, hashText(a));
    else
        s.remove(QStringLiteral("installed/") + a.id);
    return true;
}

QString Addons::catalogUrl()
{
    return QStringLiteral("https://raw.githubusercontent.com/DannyS124/diskforge-addons/main/catalog.json");
}

QString Addons::catalogSignatureUrl()
{
    return catalogUrl() + QStringLiteral(".sig");
}

QStringList Addons::catalogKeys()
{
    return kCatalogKeys;
}

bool Addons::isPinnedUrl(const QString &url)
{
    static const QRegularExpression pinned(
        QStringLiteral("^https://raw\\.githubusercontent\\.com/DannyS124/diskforge-addons/[0-9a-f]{40}/[A-Za-z0-9._/-]+\\.json$"));
    // No "..": the path has to stay inside that commit of the repository.
    return pinned.match(url).hasMatch() && !url.contains(QLatin1String(".."));
}

QVector<CatalogEntry> Addons::parseSignedCatalog(const QByteArray &json, const QByteArray &sig, const QStringList &keys, QString *error)
{
    QString why;
    if (json.size() > kMaxDownload || sig.size() > kMaxDownload
        || !signature::verify(json, sig, QString::fromLatin1(kCatalogNamespace), keys, &why)) {
        *error = QObject::tr("The add-on list didn't pass the signature check, so it wasn't used (%1). If it was just updated, "
                             "try again in a few minutes.").arg(why.isEmpty() ? QObject::tr("too big") : why);
        return {};
    }
    return parseCatalog(json, error);
}

QVector<CatalogEntry> Addons::parseCatalog(const QByteArray &json, QString *error)
{
    QVector<CatalogEntry> entries;
    if (json.size() > kMaxDownload) {
        *error = QObject::tr("The add-on list is too big");
        return entries;
    }
    QJsonParseError parseError;
    const QJsonObject o = QJsonDocument::fromJson(json, &parseError).object();
    if (parseError.error != QJsonParseError::NoError || o.value(QStringLiteral("format")).toString() != QLatin1String("diskforge-addon-catalog")) {
        *error = QObject::tr("The add-on list isn't in a format DiskForge knows");
        return entries;
    }
    if (o.value(QStringLiteral("version")).toInt() > 1) {
        *error = QObject::tr("The add-on list needs a newer DiskForge");
        return entries;
    }
    static const QRegularExpression id(QStringLiteral("^[a-z0-9][a-z0-9-]*$"));
    static const QRegularExpression sha(QStringLiteral("^[0-9a-f]{64}$"));
    int skipped = 0;
    for (const QJsonValue &v : o.value(QStringLiteral("addons")).toArray()) {
        const QJsonObject j = v.toObject();
        CatalogEntry e;
        e.id = j.value(QStringLiteral("id")).toString();
        e.name = j.value(QStringLiteral("name")).toString(e.id);
        e.version = j.value(QStringLiteral("version")).toString();
        e.author = j.value(QStringLiteral("author")).toString();
        e.description = j.value(QStringLiteral("description")).toString();
        e.url = j.value(QStringLiteral("url")).toString();
        e.sha256 = j.value(QStringLiteral("sha256")).toString();
        // Anything not pinned to a commit of the catalog repository is left out.
        if (!id.match(e.id).hasMatch() || !sha.match(e.sha256).hasMatch() || !isPinnedUrl(e.url)) {
            ++skipped;
            continue;
        }
        entries << e;
    }
    if (skipped)
        *error = QObject::tr("%n entry(s) in the list were left out because they didn't check out", nullptr, skipped);
    return entries;
}

bool Addons::installVerified(const QByteArray &data, const CatalogEntry &entry, QString *error)
{
    if (data.size() > kMaxDownload) {
        *error = QObject::tr("The download is bigger than an add-on can be");
        return false;
    }
    if (QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()) != entry.sha256) {
        *error = QObject::tr("The download doesn't match the list's checksum, so it wasn't installed");
        return false;
    }
    const Addon a = parseData(data, QString());
    if (!a.error.isEmpty()) {
        *error = a.error;
        return false;
    }
    if (a.id != entry.id) {
        *error = QObject::tr("The add-on calls itself \"%1\", not \"%2\"").arg(a.id, entry.id);
        return false;
    }
    return install(data, error);
}

bool Addons::remove(const Addon &addon, QString *error)
{
    if (addon.systemWide || !addon.file.startsWith(userDir())) {
        if (error)
            *error = QObject::tr("This add-on was installed by a package; remove it with pacman");
        return false;
    }
    if (!QDir(QFileInfo(addon.file).path()).removeRecursively()) {
        if (error)
            *error = QObject::tr("Couldn't delete %1").arg(QFileInfo(addon.file).path());
        return false;
    }
    QSettings s = settings();
    for (const char *group : {"installed/", "enabled/", "settings/", "pinned/", "shortcuts/"})
        s.remove(QLatin1String(group) + addon.id);
    return true;
}
