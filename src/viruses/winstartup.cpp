// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "winstartup.h"

#include "shelllink.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>

namespace startup {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("startup", text);
}

using Env = QHash<QString, QString>;

// %name% filled in from `env` (lower-case names); unknown ones are left as they are.
QString expand(const QString &text, const Env &env)
{
    static const QRegularExpression variable(QStringLiteral("%([^%\\s]+)%"));
    QString out;
    qsizetype last = 0;
    for (auto it = variable.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        out += text.mid(last, m.capturedStart() - last);
        const QString value = env.value(m.captured(1).toLower());
        out += value.isEmpty() ? m.captured(0) : value;
        last = m.capturedEnd();
    }
    return out + text.mid(last);
}

bool hasExtension(const QString &path)
{
    const QString name = path.section(QLatin1Char('\\'), -1);
    return name.contains(QLatin1Char('.'));
}

// Where Windows looks for a program named without a folder.
QString findBare(const QString &name, const Env &env, const std::function<bool(const QString &)> &exists)
{
    const QString file = hasExtension(name) ? name : name + QStringLiteral(".exe");
    const QString root = env.value(QStringLiteral("systemroot"), QStringLiteral("C:\\Windows"));
    const QStringList folders = {root + QStringLiteral("\\System32"), root, root + QStringLiteral("\\SysWOW64"),
                                 root + QStringLiteral("\\System32\\WindowsPowerShell\\v1.0"), root + QStringLiteral("\\System32\\wbem")};
    for (const QString &folder : folders) {
        if (exists(folder + QLatin1Char('\\') + file))
            return folder + QLatin1Char('\\') + file;
    }
    return folders.first() + QLatin1Char('\\') + file;
}

QString lowerName(const QString &windowsPath)
{
    return windowsPath.section(QLatin1Char('\\'), -1).toLower();
}

// The kinds of folders a program can be in, from its Windows path.
bool inTemporaryFolder(const QString &lower)
{
    static const QRegularExpression temp(QStringLiteral("\\\\appdata\\\\local\\\\temp\\\\|\\\\windows\\\\temp\\\\|^[a-z]:\\\\temp\\\\|\\\\inetcache\\\\"
                                                        "|\\\\temporary internet files\\\\"));
    return temp.match(lower).hasMatch();
}

bool inSharedFolder(const QString &lower)
{
    return lower.contains(QLatin1String("\\users\\public\\")) || lower.contains(QLatin1String("\\$recycle.bin\\"))
        || lower.contains(QLatin1String("\\perflogs\\"));
}

bool inWritableFolder(const QString &lower)
{
    return lower.contains(QLatin1String("\\appdata\\")) || lower.contains(QLatin1String("\\programdata\\")) || inTemporaryFolder(lower)
        || inSharedFolder(lower) || lower.contains(QLatin1String("\\downloads\\"));
}

// Where Windows and installed programs are.
bool inProgramFolder(const QString &lower, const Env &env)
{
    for (const char *variable : {"systemroot", "programfiles", "programfiles(x86)", "programw6432"}) {
        const QString folder = env.value(QLatin1String(variable)).toLower();
        if (!folder.isEmpty() && lower.startsWith(folder + QLatin1Char('\\')))
            return true;
    }
    return lower.contains(QLatin1String("\\programdata\\microsoft\\windows defender\\"));
}

// "xkqzvbnw.exe", "3f9a0c12e4.exe", a GUID: names malware makes up.
bool looksRandom(const QString &fileName)
{
    const QString stem = fileName.section(QLatin1Char('.'), 0, 0).toLower();
    static const QRegularExpression guid(QStringLiteral("^\\{?[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\}?$"));
    static const QRegularExpression hex(QStringLiteral("^(?=.*[0-9])(?=.*[a-f])[0-9a-f]{8,}$"));
    static const QRegularExpression letters(QStringLiteral("^[a-z]{7,}$"));
    if (guid.match(stem).hasMatch() || hex.match(stem).hasMatch())
        return true;
    if (!letters.match(stem).hasMatch())
        return false;
    int vowels = 0;
    for (QChar c : stem)
        vowels += QStringLiteral("aeiouy").contains(c);
    return vowels == 0;
}

// Script hosts asked to run something hidden, encoded, or from the internet.
bool runsHiddenScript(const QString &program, const QString &command)
{
    static const QSet<QString> hosts = {QStringLiteral("powershell.exe"), QStringLiteral("pwsh.exe"),   QStringLiteral("wscript.exe"),
                                        QStringLiteral("cscript.exe"),    QStringLiteral("mshta.exe"),  QStringLiteral("cmd.exe"),
                                        QStringLiteral("rundll32.exe"),   QStringLiteral("regsvr32.exe"), QStringLiteral("certutil.exe"),
                                        QStringLiteral("bitsadmin.exe"),  QStringLiteral("msiexec.exe"), QStringLiteral("wmic.exe")};
    if (!hosts.contains(lowerName(program)) && !command.toLower().contains(QLatin1String("javascript:")))
        return false;
    static const QRegularExpression markers(QStringLiteral("\\s-e(nc|ncodedcommand)?\\s|frombase64string|downloadstring|downloadfile|"
                                                           "invoke-webrequest|invoke-expression|\\biex\\b|-w(indowstyle)?\\s+hidden|"
                                                           "https?://|javascript:|vbscript:|scrobj\\.dll|/i:http|-urlcache|/transfer"),
                                            QRegularExpression::CaseInsensitiveOption);
    return markers.match(command + QLatin1Char(' ')).hasMatch();
}

QString folderWords(const QString &lower)
{
    if (lower.contains(QLatin1String("\\$recycle.bin\\")))
        return tr("the Recycle Bin");
    if (lower.contains(QLatin1String("\\users\\public\\")))
        return tr("the Public folder every user shares");
    return tr("a folder for temporary files");
}

// What looks odd about where it starts from and how.
void assess(Entry &e, const Env &env)
{
    const QString lower = e.program.toLower();
    const QString command = e.command.toLower();
    if (runsHiddenScript(e.program, e.command))
        e.flag(2, tr("It runs a hidden or encoded script, or one from the internet: a common way for malware to start itself."));
    // The program, or a script it's given, in a temporary or shared folder.
    for (const QString &where : {lower, command}) {
        if (inTemporaryFolder(where) || inSharedFolder(where)) {
            e.flag(2, tr("It starts something from %1. Programs that start with Windows don't live there.").arg(folderWords(where)));
            break;
        }
    }
    if (lower.contains(QLatin1String("\\downloads\\")))
        e.flag(1, tr("It starts a program from a Downloads folder, where installed programs don't live."));
    if (inWritableFolder(lower) && looksRandom(lowerName(lower)))
        e.flag(2, tr("Its name looks like random letters, which malware uses, in a folder any program can write to."));
    // Only a path on Windows' own drive can be looked for; another drive isn't here. Windows'
    // own tasks often outlive the program they started; that's nothing to look at.
    const bool windowsOwn = e.kind == Kind::Task && e.location.contains(QLatin1String(": \\Microsoft\\"))
                         && lower.startsWith(env.value(QStringLiteral("systemroot")).toLower() + QLatin1Char('\\'));
    if (e.file.isEmpty() && !windowsOwn && !e.program.isEmpty()
        && e.program.startsWith(env.value(QStringLiteral("systemdrive"), QStringLiteral("C:")), Qt::CaseInsensitive)) {
        e.flag(1, tr("The program it starts isn't there: left behind by something uninstalled, or by malware that was removed only "
                     "partly."));
    }
    if ((e.kind == Kind::Service || e.kind == Kind::Driver) && !e.program.isEmpty() && !inProgramFolder(lower, env)) {
        e.flag(1, tr("It starts with Windows from outside the folders where Windows and installed programs are."));
        if (e.name.startsWith(QLatin1String("Windows"), Qt::CaseInsensitive) || e.name.startsWith(QLatin1String("Microsoft"), Qt::CaseInsensitive))
            e.flag(2, tr("It's named like a part of Windows, but isn't in Windows' folder."));
    }
}

// Windows paths found here, any case: NTFS keeps the case, but Windows doesn't mind it.
class Resolver
{
public:
    explicit Resolver(QString root)
        : m_root(std::move(root))
    {
        while (m_root.size() > 1 && m_root.endsWith(QLatin1Char('/')))
            m_root.chop(1);
    }
    QString systemDrive = QStringLiteral("c:");

    QString local(const QString &windowsPath)
    {
        QString path = QString(windowsPath).replace(QLatin1Char('/'), QLatin1Char('\\'));
        if (path.size() < 2 || path.left(2).toLower() != systemDrive)
            return {}; // another drive: not known here
        const QString key = path.toLower();
        if (const auto it = m_found.constFind(key); it != m_found.constEnd())
            return *it;
        QString at = m_root;
        for (const QString &part : path.mid(2).split(QLatin1Char('\\'), Qt::SkipEmptyParts)) {
            if (part == QLatin1String("."))
                continue;
            QString next = at + QLatin1Char('/') + part;
            if (!QFileInfo::exists(next)) {
                next.clear();
                for (const QString &name : listing(at)) {
                    if (name.compare(part, Qt::CaseInsensitive) == 0) {
                        next = at + QLatin1Char('/') + name;
                        break;
                    }
                }
                if (next.isEmpty()) {
                    at.clear();
                    break;
                }
            }
            at = next;
        }
        m_found.insert(key, at);
        return at;
    }

private:
    const QStringList &listing(const QString &folder)
    {
        auto it = m_listings.find(folder);
        if (it == m_listings.end())
            it = m_listings.insert(folder, QDir(folder).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot));
        return *it;
    }

    QString m_root;
    QHash<QString, QStringList> m_listings;
    QHash<QString, QString> m_found;
};

class Collector
{
public:
    Collector(const Machine &m, Report &report)
        : m(m)
        , report(report)
    {
    }

    void run()
    {
        machineEnvironment();
        runKeys(*m.software, QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\"), m.softwareFile, QString(), m_env,
                {{QStringLiteral("Microsoft\\Windows\\CurrentVersion\\Run"), Kind::Run, tr("Starts when anyone signs in")},
                 {QStringLiteral("Microsoft\\Windows\\CurrentVersion\\RunOnce"), Kind::RunOnce, tr("Starts once, when anyone next signs in")},
                 {QStringLiteral("Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run"), Kind::Run,
                  tr("Starts when anyone signs in (set as a policy)")},
                 {QStringLiteral("WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run"), Kind::Run, tr("Starts when anyone signs in")},
                 {QStringLiteral("WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnce"), Kind::RunOnce,
                  tr("Starts once, when anyone next signs in")}});
        winlogon();
        debuggers();
        appInit();
        sessionManager();
        lsa();
        services();
        tasks();
        startupFolder(m_env.value(QStringLiteral("programdata")) + QStringLiteral("\\Microsoft\\Windows\\Start Menu\\Programs\\StartUp"),
                      QString(), m_env);
        for (const Machine::User &user : m.users)
            userEntries(user);
    }

private:
    struct Place {
        QString key;
        Kind kind;
        QString where;
    };

    void add(Entry e, const Env &env)
    {
        e.program = programOf(e.command, env, [this](const QString &path) { return !m.local(path).isEmpty(); });
        e.file = e.program.isEmpty() ? QString() : m.local(e.program);
        if (QFileInfo(e.file).isDir())
            e.file.clear();
        assess(e, env);
        e.live = m.live;
        report.entries.push_back(std::move(e));
    }

    QString softwareText(const QString &key, const QString &name, const QString &fallback = {}) const
    {
        const QString text = m.software->text(key, name);
        return text.isEmpty() ? fallback : text;
    }

    void machineEnvironment()
    {
        const QString nt = QStringLiteral("Microsoft\\Windows NT\\CurrentVersion");
        const QString root = softwareText(nt, QStringLiteral("SystemRoot"), QStringLiteral("C:\\Windows"));
        m_env.insert(QStringLiteral("systemdrive"), root.left(2));
        m_env.insert(QStringLiteral("systemroot"), root);
        m_env.insert(QStringLiteral("windir"), root);
        const QString drive = root.left(2);
        const QString cv = QStringLiteral("Microsoft\\Windows\\CurrentVersion");
        const QString programFiles = softwareText(cv, QStringLiteral("ProgramFilesDir"), drive + QStringLiteral("\\Program Files"));
        m_env.insert(QStringLiteral("programfiles"), programFiles);
        m_env.insert(QStringLiteral("programw6432"), softwareText(cv, QStringLiteral("ProgramW6432Dir"), programFiles));
        m_env.insert(QStringLiteral("programfiles(x86)"),
                     softwareText(cv, QStringLiteral("ProgramFilesDir (x86)"), drive + QStringLiteral("\\Program Files (x86)")));
        m_env.insert(QStringLiteral("commonprogramfiles"), softwareText(cv, QStringLiteral("CommonFilesDir"), programFiles + QStringLiteral("\\Common Files")));
        const QString profiles = nt + QStringLiteral("\\ProfileList");
        const QString data = expand(softwareText(profiles, QStringLiteral("ProgramData"), drive + QStringLiteral("\\ProgramData")), m_env);
        m_env.insert(QStringLiteral("programdata"), data);
        m_env.insert(QStringLiteral("allusersprofile"), data);
        m_env.insert(QStringLiteral("public"), expand(softwareText(profiles, QStringLiteral("Public"), drive + QStringLiteral("\\Users\\Public")), m_env));
        for (const reg::Value &v : m.system->values(m.controlSet + QStringLiteral("\\Control\\Session Manager\\Environment"))) {
            const QString name = v.name.toLower();
            if (!m_env.contains(name))
                m_env.insert(name, expand(v.text(), m_env));
        }
    }

    void runKeys(const Keys &keys, const QString &prefix, const QString &hiveFile, const QString &user, const Env &env, const QVector<Place> &places)
    {
        for (const Place &place : places) {
            for (const reg::Value &v : keys.values(place.key)) {
                QString command = v.text().trimmed();
                if (command.isEmpty() || v.type == reg::Dword || v.type == reg::Binary)
                    continue;
                if (place.kind == Kind::RunOnce && (command.startsWith(QLatin1Char('!')) || command.startsWith(QLatin1Char('*'))))
                    command.remove(0, 1); // RunOnce's own marks
                Entry e;
                e.kind = place.kind;
                e.where = user.isEmpty() ? place.where : QString(place.where).arg(user);
                e.location = prefix + place.key;
                e.name = v.name.isEmpty() ? tr("(default)") : v.name;
                e.user = user;
                e.command = command;
                e.hiveFile = hiveFile;
                e.key = place.key;
                e.value = v.name;
                add(std::move(e), env);
            }
        }
    }

    void winlogon()
    {
        const QString key = QStringLiteral("Microsoft\\Windows NT\\CurrentVersion\\Winlogon");
        const QString root = m_env.value(QStringLiteral("systemroot")).toLower();
        struct Normal {
            const char *name;
            QStringList usual;
            QString words;
        };
        const QVector<Normal> values = {
            {"Shell", {QStringLiteral("explorer.exe"), root + QStringLiteral("\\explorer.exe")},
             tr("Windows starts this as the desktop. Normally that's only explorer.exe.")},
            {"Userinit", {root + QStringLiteral("\\system32\\userinit.exe"), QStringLiteral("userinit.exe")},
             tr("Windows starts this when anyone signs in. Normally that's only userinit.exe.")},
        };
        for (const Normal &n : values) {
            const QString text = m.software->text(key, QLatin1String(n.name));
            if (text.isEmpty())
                continue;
            QStringList odd;
            for (const QString &part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                if (!n.usual.contains(part.trimmed().toLower()))
                    odd << part.trimmed();
            }
            Entry e;
            e.kind = Kind::Winlogon;
            e.where = tr("Starts as Windows signs anyone in (Winlogon)");
            e.location = QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\") + key;
            e.name = QLatin1String(n.name);
            e.command = odd.isEmpty() ? text : odd.first();
            e.hiveFile = m.softwareFile;
            e.key = key;
            e.value = QLatin1String(n.name);
            if (!odd.isEmpty())
                e.flag(2, n.words);
            add(std::move(e), m_env);
        }
    }

    void debuggers()
    {
        static const QSet<QString> signIn = {QStringLiteral("sethc.exe"),    QStringLiteral("utilman.exe"),       QStringLiteral("osk.exe"),
                                             QStringLiteral("narrator.exe"), QStringLiteral("magnify.exe"),       QStringLiteral("displayswitch.exe"),
                                             QStringLiteral("atbroker.exe")};
        for (const QString &base : {QStringLiteral("Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options"),
                                    QStringLiteral("WOW6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options")}) {
            for (const QString &program : m.software->subkeys(base)) {
                const QString key = base + QLatin1Char('\\') + program;
                const QString debugger = m.software->text(key, QStringLiteral("Debugger"));
                if (debugger.trimmed().isEmpty())
                    continue;
                Entry e;
                e.kind = Kind::Debugger;
                e.where = tr("Starts instead of %1").arg(program);
                e.location = QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\") + key;
                e.name = program;
                e.command = debugger;
                e.hiveFile = m.softwareFile;
                e.key = key;
                e.value = QStringLiteral("Debugger");
                e.flag(2, signIn.contains(program.toLower())
                              ? tr("Whenever %1 starts, this starts instead. %1 can be started at the sign-in screen, so this is a "
                                   "well-known back door into Windows without a password.")
                                    .arg(program)
                              : tr("Whenever %1 starts, this starts instead. Malware uses it to stop antivirus programs and other "
                                   "tools from starting.")
                                    .arg(program));
                add(std::move(e), m_env);
            }
        }
    }

    void appInit()
    {
        for (const QString &key : {QStringLiteral("Microsoft\\Windows NT\\CurrentVersion\\Windows"),
                                   QStringLiteral("WOW6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Windows")}) {
            const QString dlls = m.software->text(key, QStringLiteral("AppInit_DLLs")).trimmed();
            if (dlls.isEmpty())
                continue;
            const std::optional<reg::Value> load = m.software->value(key, QStringLiteral("LoadAppInit_DLLs"));
            Entry e;
            e.kind = Kind::AppInit;
            e.where = tr("Loaded into every program that shows a window (AppInit)");
            e.location = QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\") + key;
            e.name = QStringLiteral("AppInit_DLLs");
            e.command = dlls.section(QRegularExpression(QStringLiteral("[ ,;]")), 0, 0);
            e.hiveFile = m.softwareFile;
            e.key = key;
            e.value = QStringLiteral("AppInit_DLLs");
            if (load && load->number() == 1)
                e.flag(2, tr("Windows loads this into almost every program. Few programs need that any more; malware still uses it."));
            add(std::move(e), m_env);
        }
    }

    void sessionManager()
    {
        const QString key = m.controlSet + QStringLiteral("\\Control\\Session Manager");
        static const QStringList usual = {QStringLiteral("autocheck autochk *"), QStringLiteral("autocheck autochk /q /v *")};
        for (const QString &name : {QStringLiteral("BootExecute"), QStringLiteral("SetupExecute"), QStringLiteral("Execute")}) {
            const std::optional<reg::Value> v = m.system->value(key, name);
            if (!v)
                continue;
            for (const QString &item : v->strings()) {
                if (item.trimmed().isEmpty() || usual.contains(item.trimmed().toLower()))
                    continue;
                Entry e;
                e.kind = Kind::BootExecute;
                e.where = tr("Starts before Windows has fully started");
                e.location = QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\") + key;
                e.name = name;
                e.command = item.trimmed();
                e.item = item;
                e.hiveFile = m.systemFile;
                e.key = key;
                e.value = name;
                e.flag(1, tr("Besides Windows' own disk check, some antivirus and disk tools start here; so does malware that wants "
                             "to be first."));
                add(std::move(e), m_env);
            }
        }
    }

    void lsa()
    {
        const QString key = m.controlSet + QStringLiteral("\\Control\\Lsa");
        struct Known {
            const char *name;
            QStringList usual;
        };
        const QVector<Known> lists = {
            {"Authentication Packages", {QStringLiteral("msv1_0")}},
            {"Notification Packages", {QStringLiteral("scecli"), QStringLiteral("rassfm")}},
            {"Security Packages",
             {QString(), QStringLiteral("\"\""), QStringLiteral("kerberos"), QStringLiteral("msv1_0"), QStringLiteral("schannel"),
              QStringLiteral("wdigest"), QStringLiteral("tspkg"), QStringLiteral("pku2u"), QStringLiteral("cloudap"),
              QStringLiteral("negoextender"), QStringLiteral("livessp")}},
        };
        for (const Known &list : lists) {
            const std::optional<reg::Value> v = m.system->value(key, QLatin1String(list.name));
            if (!v)
                continue;
            for (const QString &item : v->strings()) {
                if (list.usual.contains(item.trimmed().toLower()))
                    continue;
                Entry e;
                e.kind = Kind::Lsa;
                e.item = item;
                e.where = tr("Loaded by Windows' sign-in service (LSA)");
                e.location = QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\") + key;
                e.name = QLatin1String(list.name);
                e.command = item.trimmed().endsWith(QLatin1String(".dll"), Qt::CaseInsensitive) ? item.trimmed() : item.trimmed() + QStringLiteral(".dll");
                e.hiveFile = m.systemFile;
                e.key = key;
                e.value = QLatin1String(list.name);
                e.flag(1, tr("Windows hands this library people's passwords as they sign in or change them. Some company software "
                             "does this; so does malware that steals passwords."));
                add(std::move(e), m_env);
            }
        }
    }

    void services()
    {
        const QString base = m.controlSet + QStringLiteral("\\Services");
        for (const QString &name : m.system->subkeys(base)) {
            const QString key = base + QLatin1Char('\\') + name;
            const std::optional<reg::Value> start = m.system->value(key, QStringLiteral("Start"));
            if (!start || start->number() > 2)
                continue; // only the ones that start with Windows
            const quint64 type = m.system->value(key, QStringLiteral("Type")).value_or(reg::Value()).number();
            const bool driver = (type & 0xB) != 0;
            QString command = m.system->text(key, QStringLiteral("ImagePath"));
            const QString dll = m.system->text(key + QStringLiteral("\\Parameters"), QStringLiteral("ServiceDll"));
            if (!dll.isEmpty() && (command.isEmpty() || command.contains(QLatin1String("svchost"), Qt::CaseInsensitive)))
                command = dll; // svchost runs the service's library: that's what matters
            if (command.isEmpty() && driver)
                command = QStringLiteral("System32\\drivers\\") + name + QStringLiteral(".sys");
            if (command.isEmpty())
                continue;
            Entry e;
            e.kind = driver ? Kind::Driver : Kind::Service;
            e.where = driver ? tr("A driver that starts with Windows") : tr("A service that starts with Windows");
            e.location = QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\") + key;
            const QString display = m.system->text(key, QStringLiteral("DisplayName"));
            e.name = display.isEmpty() || display.startsWith(QLatin1Char('@')) ? name : display;
            e.command = command;
            e.hiveFile = m.systemFile;
            e.key = key;
            e.value = QStringLiteral("Start");
            add(std::move(e), m_env);
        }
    }

    void tasks()
    {
        const QString folder = m.local(m_env.value(QStringLiteral("systemroot")) + QStringLiteral("\\System32\\Tasks"));
        if (folder.isEmpty())
            return;
        QDirIterator it(folder, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            QFile f(path);
            if (f.size() > 4 * 1024 * 1024 || !f.open(QIODevice::ReadOnly))
                continue;
            QXmlStreamReader xml(f.readAll());
            bool enabled = true;
            QString command, arguments;
            QVector<QPair<QString, QString>> actions;
            QStringList element;
            while (!xml.atEnd()) {
                const QXmlStreamReader::TokenType token = xml.readNext();
                if (token == QXmlStreamReader::StartElement) {
                    element << xml.name().toString();
                } else if (token == QXmlStreamReader::EndElement) {
                    if (element.value(element.size() - 1) == QLatin1String("Exec")) {
                        if (!command.isEmpty())
                            actions.push_back({command, arguments});
                        command.clear();
                        arguments.clear();
                    }
                    if (!element.isEmpty())
                        element.removeLast();
                } else if (token == QXmlStreamReader::Characters && !element.isEmpty()) {
                    const QString name = element.last();
                    const QString parent = element.value(element.size() - 2);
                    if (parent == QLatin1String("Exec") && name == QLatin1String("Command"))
                        command += xml.text().toString().trimmed();
                    else if (parent == QLatin1String("Exec") && name == QLatin1String("Arguments"))
                        arguments += xml.text().toString().trimmed();
                    else if (parent == QLatin1String("Settings") && name == QLatin1String("Enabled"))
                        enabled = xml.text().trimmed() != QLatin1String("false");
                }
            }
            if (!enabled)
                continue;
            const QString name = QDir(folder).relativeFilePath(path).replace(QLatin1Char('/'), QLatin1Char('\\'));
            for (const auto &[program, args] : std::as_const(actions)) {
                Entry e;
                e.kind = Kind::Task;
                e.where = tr("A scheduled task");
                e.location = tr("Task Scheduler: \\%1").arg(name);
                e.name = name.section(QLatin1Char('\\'), -1);
                e.command = (program.contains(QLatin1Char(' ')) && !program.startsWith(QLatin1Char('"')) ? QLatin1Char('"') + program + QLatin1Char('"') : program)
                          + (args.isEmpty() ? QString() : QLatin1Char(' ') + args);
                e.entryFile = path;
                add(std::move(e), m_env);
            }
        }
    }

    void startupFolder(const QString &windowsFolder, const QString &user, const Env &env)
    {
        const QString folder = m.local(windowsFolder);
        if (folder.isEmpty())
            return;
        for (const QFileInfo &file : QDir(folder).entryInfoList(QDir::Files | QDir::Hidden | QDir::System)) {
            if (file.fileName().compare(QLatin1String("desktop.ini"), Qt::CaseInsensitive) == 0)
                continue;
            Entry e;
            e.kind = Kind::StartupFolder;
            e.where = user.isEmpty() ? tr("Starts when anyone signs in (Startup folder)") : tr("Starts when %1 signs in (Startup folder)").arg(user);
            e.location = windowsFolder;
            e.name = file.fileName();
            e.user = user;
            e.entryFile = file.filePath();
            const QString self = windowsFolder + QLatin1Char('\\') + file.fileName();
            if (file.suffix().compare(QLatin1String("lnk"), Qt::CaseInsensitive) == 0) {
                QFile f(file.filePath());
                shelllink::Link link;
                if (f.size() < 1024 * 1024 && f.open(QIODevice::ReadOnly) && shelllink::parse(f.readAll(), &link) && !link.target.isEmpty()) {
                    e.command = QLatin1Char('"') + link.target + QLatin1Char('"') + (link.arguments.isEmpty() ? QString() : QLatin1Char(' ') + link.arguments);
                } else {
                    e.command = self;
                    e.flag(1, tr("A shortcut that can't be read."));
                }
            } else {
                e.command = QLatin1Char('"') + self + QLatin1Char('"');
            }
            add(std::move(e), env);
        }
    }

    void userEntries(const Machine::User &user)
    {
        Env env = m_env;
        const QString profile = user.profile;
        env.insert(QStringLiteral("userprofile"), profile);
        env.insert(QStringLiteral("homedrive"), profile.left(2));
        env.insert(QStringLiteral("homepath"), profile.mid(2));
        env.insert(QStringLiteral("username"), user.name);
        env.insert(QStringLiteral("appdata"), profile + QStringLiteral("\\AppData\\Roaming"));
        env.insert(QStringLiteral("localappdata"), profile + QStringLiteral("\\AppData\\Local"));
        env.insert(QStringLiteral("temp"), profile + QStringLiteral("\\AppData\\Local\\Temp"));
        env.insert(QStringLiteral("tmp"), profile + QStringLiteral("\\AppData\\Local\\Temp"));
        if (user.keys) {
            for (const reg::Value &v : user.keys->values(QStringLiteral("Environment")))
                env.insert(v.name.toLower(), expand(v.text(), env));
            const QString prefix = QStringLiteral("HKEY_CURRENT_USER\\");
            runKeys(*user.keys, prefix, user.hiveFile, user.name, env,
                    {{QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Run"), Kind::Run, tr("Starts when %1 signs in")},
                     {QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce"), Kind::RunOnce, tr("Starts once, when %1 next signs in")},
                     {QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run"), Kind::Run,
                      tr("Starts when %1 signs in (set as a policy)")}});
            // Old places, which today's programs don't use.
            const QString windows = QStringLiteral("Software\\Microsoft\\Windows NT\\CurrentVersion\\Windows");
            for (const QString &name : {QStringLiteral("Load"), QStringLiteral("Run")}) {
                const QString command = user.keys->text(windows, name).trimmed();
                if (command.isEmpty())
                    continue;
                Entry e;
                e.kind = Kind::Load;
                e.where = tr("Starts when %1 signs in (an old way)").arg(user.name);
                e.location = prefix + windows;
                e.name = name;
                e.user = user.name;
                e.command = command;
                e.hiveFile = user.hiveFile;
                e.key = windows;
                e.value = name;
                e.flag(1, tr("An old way of starting programs at sign-in that today's programs don't use; malware still does."));
                add(std::move(e), env);
            }
            const QString shell = user.keys->text(QStringLiteral("Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon"), QStringLiteral("Shell"));
            if (!shell.trimmed().isEmpty()) {
                Entry e;
                e.kind = Kind::Winlogon;
                e.where = tr("Starts as %1's desktop (Winlogon)").arg(user.name);
                e.location = prefix + QStringLiteral("Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon");
                e.name = QStringLiteral("Shell");
                e.user = user.name;
                e.command = shell.trimmed();
                e.hiveFile = user.hiveFile;
                e.key = QStringLiteral("Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon");
                e.value = QStringLiteral("Shell");
                e.flag(2, tr("This replaces Windows' desktop for %1. Windows doesn't set this itself.").arg(user.name));
                add(std::move(e), env);
            }
            const QString script = user.keys->text(QStringLiteral("Environment"), QStringLiteral("UserInitMprLogonScript"));
            if (!script.trimmed().isEmpty()) {
                Entry e;
                e.kind = Kind::Load;
                e.where = tr("Starts when %1 signs in (a logon script)").arg(user.name);
                e.location = prefix + QStringLiteral("Environment");
                e.name = QStringLiteral("UserInitMprLogonScript");
                e.user = user.name;
                e.command = script.trimmed();
                e.hiveFile = user.hiveFile;
                e.key = QStringLiteral("Environment");
                e.value = QStringLiteral("UserInitMprLogonScript");
                e.flag(1, tr("A logon script set for one user: rare outside company networks."));
                add(std::move(e), env);
            }
        }
        startupFolder(profile + QStringLiteral("\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup"), user.name, env);
    }

    const Machine &m;
    Report &report;
    Env m_env;
};

QString hiveNote(const QString &name, const reg::Hive &hive)
{
    if (hive.maybeBehind())
        return tr("Windows didn't close %1 properly and its logs couldn't bring it up to date, so the newest changes may be missing.").arg(name);
    if (hive.recoveredFrom() > 0)
        return tr("%1 was read with the changes Windows had only written to its log so far.").arg(name);
    return {};
}

std::shared_ptr<reg::Hive> openHive(const QString &path, const QString &name, QStringList &notes)
{
    auto hive = std::make_shared<reg::Hive>();
    QString error;
    if (path.isEmpty() || !hive->open(path, &error)) {
        notes << tr("%1 couldn't be read: %2").arg(name, path.isEmpty() ? tr("it isn't there") : error);
        return nullptr;
    }
    const QString note = hiveNote(name, *hive);
    if (!note.isEmpty())
        notes << note;
    return hive;
}

// No keys at all: for a hive that couldn't be read.
class NoKeys : public Keys
{
public:
    bool exists(const QString &) const override { return false; }
    QVector<reg::Value> values(const QString &) const override { return {}; }
    QStringList subkeys(const QString &) const override { return {}; }
};

} // namespace

void Entry::flag(int level, const QString &reason)
{
    if (!reasons.contains(reason))
        reasons << reason;
    severity = std::max(severity, level);
}

std::optional<reg::Value> Keys::value(const QString &path, const QString &name) const
{
    for (reg::Value &v : values(path)) {
        if (v.name.compare(name, Qt::CaseInsensitive) == 0)
            return std::move(v);
    }
    return std::nullopt;
}

QString Keys::text(const QString &path, const QString &name) const
{
    const std::optional<reg::Value> v = value(path, name);
    return v ? v->text() : QString();
}

reg::Key HiveKeys::find(const QString &path) const
{
    const QString name = path.toLower();
    auto it = m_found.constFind(name);
    if (it == m_found.constEnd())
        it = m_found.insert(name, m_hive->key(path));
    return *it;
}

bool HiveKeys::exists(const QString &path) const
{
    return find(path) != 0;
}

QVector<reg::Value> HiveKeys::values(const QString &path) const
{
    const reg::Key key = find(path);
    return key ? m_hive->values(key) : QVector<reg::Value>();
}

QStringList HiveKeys::subkeys(const QString &path) const
{
    QStringList out;
    const reg::Key key = find(path);
    if (key) {
        for (reg::Key sub : m_hive->subkeys(key))
            out << m_hive->name(sub);
    }
    return out;
}

QString programOf(const QString &command, const QHash<QString, QString> &env, const std::function<bool(const QString &)> &exists)
{
    QString cmd = expand(command.trimmed(), env);
    const QString root = env.value(QStringLiteral("systemroot"), QStringLiteral("C:\\Windows"));
    // How the kernel and service settings write paths.
    if (cmd.startsWith(QLatin1String("\\??\\")) || cmd.startsWith(QLatin1String("\\\\?\\")))
        cmd = cmd.mid(4);
    if (cmd.startsWith(QLatin1String("\\SystemRoot\\"), Qt::CaseInsensitive))
        cmd = root + cmd.mid(11);
    else if (cmd.startsWith(QLatin1String("System32\\"), Qt::CaseInsensitive) || cmd.startsWith(QLatin1String("SysWOW64\\"), Qt::CaseInsensitive))
        cmd = root + QLatin1Char('\\') + cmd;

    QString program, rest;
    if (cmd.startsWith(QLatin1Char('"'))) {
        const qsizetype end = cmd.indexOf(QLatin1Char('"'), 1);
        program = end < 0 ? cmd.mid(1) : cmd.mid(1, end - 1);
        rest = end < 0 ? QString() : cmd.mid(end + 1).trimmed();
    } else {
        // Not in quotes: "C:\Program Files\App\app.exe /tray" is the shortest start of it
        // that's a file, or that ends like a program.
        const QStringList words = cmd.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        static const QRegularExpression programEnd(QStringLiteral("\\.(exe|dll|sys|com|scr|bat|cmd|ps1|vbs|vbe|js|jse|wsf|hta|cpl|ocx|msi|lnk)$"),
                                                   QRegularExpression::CaseInsensitiveOption);
        qsizetype used = std::min<qsizetype>(1, words.size());
        if (used && words.first().contains(QLatin1Char('\\'))) {
            used = 0;
            for (qsizetype n = 1; n <= words.size() && !used; ++n) {
                const QString candidate = words.mid(0, n).join(QLatin1Char(' '));
                if (exists(candidate) || exists(candidate + QStringLiteral(".exe")))
                    used = n;
            }
            for (qsizetype n = 1; n <= words.size() && !used; ++n) {
                if (programEnd.match(words[n - 1]).hasMatch())
                    used = n;
            }
            if (!used)
                used = 1;
        }
        program = words.mid(0, used).join(QLatin1Char(' '));
        rest = words.mid(used).join(QLatin1Char(' '));
    }
    program = program.trimmed();
    while (program.endsWith(QLatin1Char(',')))
        program.chop(1);
    if (program.isEmpty())
        return {};
    if (!program.contains(QLatin1Char('\\')))
        program = findBare(program, env, exists);
    else if (!hasExtension(program) && exists(program + QStringLiteral(".exe")))
        program += QStringLiteral(".exe");

    // rundll32 and regsvr32 run a library: that's the program that matters.
    const QString name = lowerName(program);
    QString library;
    if (name == QLatin1String("rundll32.exe")) {
        // "rundll32.exe /d acproxy.dll,Entry": switches first, then the library.
        QString words = rest;
        while (words.startsWith(QLatin1Char('/')) || words.startsWith(QLatin1Char('-')))
            words = words.section(QLatin1Char(' '), 1).trimmed();
        library = words.section(QLatin1Char(','), 0, 0).trimmed();
    } else if (name == QLatin1String("regsvr32.exe")) {
        for (const QString &word : rest.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            if (!word.startsWith(QLatin1Char('/')) && !word.startsWith(QLatin1Char('-')))
                library = word;
        }
    }
    library.remove(QLatin1Char('"'));
    if (!library.isEmpty() && !library.contains(QLatin1Char(':')) && !library.contains(QLatin1Char('\\')))
        return findBare(hasExtension(library) ? library : library + QStringLiteral(".dll"), env, exists);
    if (library.size() > 2 && library[1] == QLatin1Char(':'))
        return library;
    return program;
}

namespace {

QString notClosed()
{
    return tr("Windows didn't close this registry file properly (Fast Startup, a crash, or it's running), so it isn't changed. Start "
              "Windows and shut it down fully first: hold Shift while clicking Shut down.");
}

// One entry, in a registry file that's open.
bool change(reg::Hive &hive, const Entry &e, QString *message)
{
    const reg::Key key = hive.key(e.key);
    if (!key) {
        *message = tr("it isn't there any more");
        return false;
    }
    QString error;
    bool ok = false;
    switch (e.kind) {
    case Kind::Service:
    case Kind::Driver:
        ok = hive.setDword(key, QStringLiteral("Start"), 4, &error);
        *message = tr("set to Disabled");
        break;
    case Kind::Winlogon:
        if (e.user.isEmpty()) {
            // Windows' own, which is shorter than what's there now.
            const QString root = hive.value(hive.key(QStringLiteral("Microsoft\\Windows NT\\CurrentVersion")), QStringLiteral("SystemRoot"))
                                     .value_or(reg::Value())
                                     .text();
            const QString usual = e.value == QLatin1String("Shell") ? QStringLiteral("explorer.exe")
                                                                     : (root.isEmpty() ? QStringLiteral("C:\\Windows") : root) + QStringLiteral("\\system32\\userinit.exe,");
            ok = hive.setString(key, e.value, usual, &error);
            *message = tr("set back to Windows' own: %1").arg(usual);
        } else {
            ok = hive.removeValue(key, e.value, &error);
            *message = tr("taken out of the registry");
        }
        break;
    case Kind::AppInit:
        ok = hive.setDword(key, QStringLiteral("LoadAppInit_DLLs"), 0, &error);
        *message = tr("switched off (LoadAppInit_DLLs set to 0)");
        break;
    case Kind::BootExecute:
    case Kind::Lsa: {
        QStringList items = hive.value(key, e.value).value_or(reg::Value()).strings();
        const qsizetype before = items.size();
        items.removeIf([&e](const QString &item) { return item.trimmed().compare(e.item.trimmed(), Qt::CaseInsensitive) == 0; });
        if (items.size() == before) {
            *message = tr("it isn't there any more");
            return false;
        }
        ok = hive.setStrings(key, e.value, items, &error);
        *message = tr("taken out of the list");
        break;
    }
    default:
        ok = hive.removeValue(key, e.value, &error);
        *message = tr("taken out of the registry");
        break;
    }
    if (!ok)
        *message = error;
    return ok;
}

// A task or a startup-folder file, moved into the backup folder (a task whose file is gone
// doesn't run).
bool moveAway(const Entry &e, const QString &backup, QString *message)
{
    QString into;
    if (e.kind == Kind::Task)
        into = backup + QStringLiteral("/Tasks/") + e.location.section(QStringLiteral(": \\"), 1).replace(QLatin1Char('\\'), QLatin1Char('/'));
    else
        into = backup + QStringLiteral("/Startup folder") + (e.user.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(e.user)) + QLatin1Char('/') + e.name;
    QDir().mkpath(QFileInfo(into).absolutePath());
    if (!QFile::rename(e.entryFile, into) && !(QFile::copy(e.entryFile, into) && QFile::remove(e.entryFile))) {
        *message = tr("couldn't be moved away");
        return false;
    }
    *message = e.kind == Kind::Task ? tr("switched off (its task file moved to the backup)") : tr("moved to the backup");
    return true;
}

} // namespace

bool canSwitchOff(const Entry &e)
{
    return (!e.hiveFile.isEmpty() && !e.key.isEmpty() && !e.value.isEmpty()) || !e.entryFile.isEmpty();
}

QVector<Change> switchOff(const QVector<Entry> &entries, const QString &backup)
{
    QVector<Change> changes;
    QDir().mkpath(backup);
    struct Open {
        std::shared_ptr<reg::Hive> hive;
        QString error;
        QString copy;
    };
    QHash<QString, Open> hives;
    QStringList done, copies;
    bool moved = false, exported = false;
    for (const Entry &e : entries) {
        Change c;
        c.name = e.name;
        if (e.live) {
#ifdef Q_OS_WIN
            c.done = switchOffRunning(e, backup, &c.message);
#endif
        } else if (!e.hiveFile.isEmpty()) {
            auto it = hives.find(e.hiveFile);
            if (it == hives.end()) {
                // Opened once, and copied before anything in it changes.
                Open o;
                o.hive = std::make_shared<reg::Hive>();
                const QString name = QFileInfo(e.hiveFile).fileName() + (e.user.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(e.user));
                o.copy = backup + QLatin1Char('/') + name;
                if (!o.hive->open(e.hiveFile, &o.error)) {
                    o.hive.reset();
                } else if (!o.hive->canWrite()) {
                    o.error = notClosed();
                    o.hive.reset();
                } else if (!QFile::copy(e.hiveFile, o.copy)) {
                    o.error = tr("a backup copy of it couldn't be made");
                    o.hive.reset();
                } else {
                    copies << QStringLiteral("  %1  (was %2)").arg(name, QDir::toNativeSeparators(e.hiveFile));
                }
                it = hives.insert(e.hiveFile, o);
            }
            if (!it->hive) {
                c.message = it->error;
            } else if ((c.done = change(*it->hive, e, &c.message))) {
                QString error;
                if (!it->hive->save(e.hiveFile, &error)) {
                    c.done = false;
                    c.message = error;
                }
            }
        } else if (!e.entryFile.isEmpty()) {
            c.done = moveAway(e, backup, &c.message);
        }
        if (c.done) {
            done << QStringLiteral("  %1: %2 (%3) - %4").arg(e.where, e.name, e.location, c.message);
            moved = moved || ((e.kind == Kind::Task || e.kind == Kind::StartupFolder) && !e.live);
            exported = exported || (e.live && e.kind != Kind::Task && e.kind != Kind::StartupFolder);
        }
        changes << c;
    }
    // How to put it back.
    QFile note(backup + QStringLiteral("/What DiskAnvil changed.txt"));
    if (note.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QStringList lines{QStringLiteral("DiskAnvil switched off these things that started with Windows, on %1:")
                              .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))),
                          QString()};
        lines << done << QString();
        if (!copies.isEmpty()) {
            lines << QStringLiteral("The registry files changed are here as they were before:") << copies
                  << QStringLiteral("To undo everything, copy them back where they were, while that Windows isn't running (from DiskAnvil Live).")
                  << QString();
        }
        if (moved)
            lines << QStringLiteral("Tasks and startup files that were moved are here too, in folders named after where they were. Moving one back "
                                    "where it was puts it back.");
        if (exported)
            lines << QStringLiteral("Double-click a .reg file here to put those settings back.");
        note.write(lines.join(QLatin1Char('\n')).toUtf8() + '\n');
    }
    return changes;
}

Finder::Finder(QString root)
    : m_root(std::move(root))
{
    qRegisterMetaType<startup::Report>();
}

void Finder::run()
{
#ifdef Q_OS_WIN
    Q_EMIT finished(m_root.isEmpty() ? findRunning() : find(m_root));
#else
    Q_EMIT finished(find(m_root));
#endif
}

bool hasWindows(const QString &root)
{
    Resolver r(root);
    return !r.local(QStringLiteral("C:\\Windows\\System32\\config\\SOFTWARE")).isEmpty();
}

Report collect(const Machine &machine)
{
    Report report;
    report.windows = true;
    report.notes = machine.notes;
    Collector(machine, report).run();
    return report;
}

Report find(const QString &root)
{
    Report report;
    auto resolver = std::make_shared<Resolver>(root);
    const QString config = resolver->local(QStringLiteral("C:\\Windows\\System32\\config"));
    if (config.isEmpty())
        return report;
    Machine m;
    m.local = [resolver](const QString &path) { return resolver->local(path); };
    m.softwareFile = resolver->local(QStringLiteral("C:\\Windows\\System32\\config\\SOFTWARE"));
    m.systemFile = resolver->local(QStringLiteral("C:\\Windows\\System32\\config\\SYSTEM"));
    auto software = openHive(m.softwareFile, QStringLiteral("SOFTWARE"), m.notes);
    auto system = openHive(m.systemFile, QStringLiteral("SYSTEM"), m.notes);
    auto keysOf = [](const std::shared_ptr<reg::Hive> &hive) -> std::shared_ptr<Keys> {
        if (hive)
            return std::make_shared<HiveKeys>(hive);
        return std::make_shared<NoKeys>();
    };
    m.software = keysOf(software);
    m.system = keysOf(system);
    // Windows calls its own drive C: (nearly always); its own registry says.
    const QString systemRoot = m.software->text(QStringLiteral("Microsoft\\Windows NT\\CurrentVersion"), QStringLiteral("SystemRoot"));
    if (systemRoot.size() >= 2 && systemRoot[1] == QLatin1Char(':'))
        resolver->systemDrive = systemRoot.left(2).toLower();
    const std::optional<reg::Value> current = m.system->value(QStringLiteral("Select"), QStringLiteral("Current"));
    m.controlSet = QStringLiteral("ControlSet%1").arg(current ? current->number() : 1, 3, 10, QLatin1Char('0'));
    // Its users: everyone with a profile folder.
    const QString profiles = QStringLiteral("Microsoft\\Windows NT\\CurrentVersion\\ProfileList");
    for (const QString &sid : m.software->subkeys(profiles)) {
        if (!sid.startsWith(QLatin1String("S-1-5-21-")))
            continue; // the system's own accounts
        QString profile = m.software->text(profiles + QLatin1Char('\\') + sid, QStringLiteral("ProfileImagePath"));
        profile.replace(QLatin1String("%SystemDrive%"), systemRoot.left(2), Qt::CaseInsensitive);
        if (profile.isEmpty())
            continue;
        Machine::User user;
        user.name = profile.section(QLatin1Char('\\'), -1);
        user.profile = profile;
        user.hiveFile = resolver->local(profile + QStringLiteral("\\NTUSER.DAT"));
        if (user.hiveFile.isEmpty())
            continue; // a profile whose folder is gone
        if (auto hive = openHive(user.hiveFile, QStringLiteral("NTUSER.DAT (%1)").arg(user.name), m.notes))
            user.keys = std::make_shared<HiveKeys>(hive);
        m.users << user;
    }
    return collect(m);
}

} // namespace startup
