// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "winstart.h"

#include "lostfs.h"
#include "reghive.h"
#include "vshadow.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QXmlStreamReader>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace winstart {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("winstart", text);
}

QString tr(const char *text, const char *comment, int n)
{
    return QCoreApplication::translate("winstart", text, comment, n);
}

// What COMPONENTS keeps pointing at pending.xml (what DISM's revert clears).
const QStringList kPointers = {QStringLiteral("PendingXmlIdentifier"), QStringLiteral("NextQueueEntryIndex"),
                               QStringLiteral("AdvancedInstallersNeedResolving")};

QString config(const QString &root)
{
    return find(root, QStringLiteral("Windows/System32/config"));
}

// Sets `file` aside next to itself: <name>.before-diskanvil-<when>.
bool setAside(const QString &file, const QString &when, QStringList *done, QString *error)
{
    const QString to = file + QStringLiteral(".before-diskanvil-") + when;
    if (!QFile::rename(file, to)) {
        *error = tr("Couldn't rename %1 (the drive may be read-only).").arg(QDir::toNativeSeparators(file));
        return false;
    }
    if (done)
        *done << tr("%1 kept as %2").arg(QFileInfo(file).fileName(), QFileInfo(to).fileName());
    return true;
}

// A hive's logs, next to it.
QStringList logsOf(const QString &file)
{
    QStringList out;
    const QFileInfo info(file);
    const QString base = info.fileName().toLower();
    for (const QFileInfo &other : QDir(info.absolutePath()).entryInfoList(QDir::Files | QDir::Hidden | QDir::System)) {
        const QString name = other.fileName().toLower();
        if (name == base + QStringLiteral(".log1") || name == base + QStringLiteral(".log2") || name == base + QStringLiteral(".log"))
            out << other.filePath();
    }
    return out;
}

// Every key, to find what's broken: a hive can open fine and be damaged further in.
int walk(const reg::Hive &hive, reg::Key key, int depth, const std::atomic<bool> *cancel)
{
    if (depth > 512 || (cancel && *cancel))
        return 0;
    int n = 1;
    hive.values(key);
    for (reg::Key sub : hive.subkeys(key))
        n += walk(hive, sub, depth + 1, cancel);
    return n;
}

HiveCheck check(reg::Hive &hive, bool opened, const QString &openError, const QString &name, const std::atomic<bool> *cancel)
{
    HiveCheck c;
    c.name = name;
    if (!opened) {
        c.state = HiveState::Damaged;
        c.problem = tr("It can't be read as a registry file: %1.").arg(openError);
        return c;
    }
    c.keys = walk(hive, hive.root(), 0, cancel);
    if (hive.problems() > 0) {
        c.state = HiveState::Damaged;
        c.problem = tr("It's damaged: %n broken record(s) were found in it.", nullptr, hive.problems());
    } else if (hive.maybeBehind()) {
        c.state = HiveState::Behind;
        c.problem = tr("Windows didn't close it properly, and its logs can't bring it up to date: what changed last may be lost. "
                       "Windows usually copes with this.");
    } else if (hive.recoveredFrom() > 0) {
        c.state = HiveState::UpToDateFromLogs;
        c.problem = tr("Windows didn't close it properly, but its logs have everything: it's fine.");
    } else {
        c.state = HiveState::Fine;
    }
    if (c.keys < 2 && c.state == HiveState::Fine) {
        c.state = HiveState::Damaged;
        c.problem = tr("It's empty.");
    }
    return c;
}

} // namespace

QString find(const QString &root, const QString &path)
{
    QString at = root;
    while (at.size() > 1 && at.endsWith(QLatin1Char('/')))
        at.chop(1);
    for (const QString &part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        QString next = at + QLatin1Char('/') + part;
        if (!QFileInfo::exists(next)) {
            next.clear();
            for (const QString &name : QDir(at).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
                if (name.compare(part, Qt::CaseInsensitive) == 0) {
                    next = at + QLatin1Char('/') + name;
                    break;
                }
            }
            if (next.isEmpty())
                return {};
        }
        at = next;
    }
    return at;
}

QString stamp()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd-HHmm"));
}

StuckUpdate findStuckUpdate(const QString &root)
{
    StuckUpdate s;
    s.pendingFile = find(root, QStringLiteral("Windows/WinSxS/pending.xml"));
    if (!s.pendingFile.isEmpty()) {
        const QFileInfo info(s.pendingFile);
        s.pendingWritten = info.lastModified();
        s.pendingSize = info.size();
        QFile f(s.pendingFile);
        if (f.open(QIODevice::ReadOnly)) {
            QXmlStreamReader xml(&f);
            // What's to be done is listed in its POQ ("pending operations queue") sections:
            // files to link, move and delete, registry keys and values to set.
            int depth = 0, poqAt = -1;
            while (!xml.atEnd() && s.actions < 1000000) {
                const auto t = xml.readNext();
                if (t == QXmlStreamReader::StartElement) {
                    ++depth;
                    if (poqAt < 0 && xml.name().compare(QLatin1String("POQ"), Qt::CaseInsensitive) == 0)
                        poqAt = depth;
                    else if (poqAt >= 0 && depth == poqAt + 1)
                        ++s.actions;
                } else if (t == QXmlStreamReader::EndElement) {
                    if (depth == poqAt)
                        poqAt = -1;
                    --depth;
                }
            }
        }
    }
    const QString cfg = config(root);
    if (!cfg.isEmpty()) {
        reg::Hive software;
        QString error;
        const QString softwareFile = find(cfg, QStringLiteral("SOFTWARE"));
        if (!softwareFile.isEmpty() && software.open(softwareFile, &error)) {
            const reg::Key cbs = software.key(QStringLiteral("Microsoft\\Windows\\CurrentVersion\\Component Based Servicing"));
            if (cbs) {
                if (const reg::Key pending = software.key(QStringLiteral("PackagesPending"), cbs))
                    for (reg::Key k : software.subkeys(pending))
                        s.packages << software.name(k);
                if (const reg::Key reboot = software.key(QStringLiteral("RebootPending"), cbs); reboot && s.packages.isEmpty())
                    for (reg::Key k : software.subkeys(reboot))
                        s.packages << software.name(k);
            }
        }
        s.componentsFile = find(cfg, QStringLiteral("COMPONENTS"));
        reg::Hive components;
        if (!s.componentsFile.isEmpty() && components.open(s.componentsFile, &error)) {
            for (const QString &p : kPointers)
                if (components.value(components.root(), p))
                    s.pointers << p;
        }
    }
    s.stuck = !s.pendingFile.isEmpty() || !s.pointers.isEmpty();
    if (s.stuck && !s.pendingFile.isEmpty() && !QFileInfo(QFileInfo(s.pendingFile).absolutePath()).isWritable())
        s.problem = tr("The drive is open read-only: open it again so it can be changed.");
    return s;
}

bool undoStuckUpdate(const QString &root, QStringList *done, QString *error)
{
    const StuckUpdate s = findStuckUpdate(root);
    if (!s.stuck) {
        *error = tr("Nothing is waiting to be done at the next start: no update is stuck.");
        return false;
    }
    if (!s.problem.isEmpty()) {
        *error = s.problem;
        return false;
    }
    const QString when = stamp();
    // COMPONENTS first: when it can't be changed, nothing is.
    if (!s.pointers.isEmpty()) {
        reg::Hive hive;
        if (!hive.open(s.componentsFile, error))
            return false;
        if (!hive.canWrite() && !hive.changeWhole()) {
            *error = tr("Windows' COMPONENTS file wasn't closed properly and can't be brought up to date, so it isn't changed. Use "
                        "Microsoft's own way instead, from Windows' recovery: Troubleshoot, Advanced options, Command Prompt, then: "
                        "dism /image:C:\\ /cleanup-image /revertpendingactions");
            return false;
        }
        for (const QString &p : s.pointers) {
            if (!hive.removeValue(hive.root(), p, error))
                return false;
        }
        const QString backup = s.componentsFile + QStringLiteral(".before-diskanvil-") + when;
        if (!QFile::copy(s.componentsFile, backup)) {
            *error = tr("Couldn't make a copy of COMPONENTS first, so nothing was changed.");
            return false;
        }
        *done << tr("COMPONENTS copied to %1").arg(QFileInfo(backup).fileName());
        if (hive.recoveredFrom() > 0) {
            // Written out whole, up to date, with its old logs set aside.
            const QString fresh = s.componentsFile + QStringLiteral(".new-diskanvil");
            QFile::remove(fresh);
            if (!hive.saveWhole(fresh, error))
                return false;
            for (const QString &log : logsOf(s.componentsFile))
                if (!setAside(log, when, done, error))
                    return false;
            if (!QFile::remove(s.componentsFile) || !QFile::rename(fresh, s.componentsFile)) {
                *error = tr("Couldn't put the changed COMPONENTS in place: its copy is %1.").arg(QFileInfo(backup).fileName());
                return false;
            }
        } else if (!hive.save(s.componentsFile, error)) {
            return false;
        }
        *done << tr("Taken out of COMPONENTS: %1").arg(s.pointers.join(QStringLiteral(", ")));
    }
    if (!s.pendingFile.isEmpty() && !setAside(s.pendingFile, when, done, error))
        return false;
    return true;
}

QStringList dismArguments(const QString &windowsDrive)
{
    QString image = QString(windowsDrive).replace(QLatin1Char('/'), QLatin1Char('\\'));
    if (!image.endsWith(QLatin1Char('\\')))
        image += QLatin1Char('\\');
    return {QStringLiteral("/Image:") + image, QStringLiteral("/Cleanup-Image"), QStringLiteral("/RevertPendingActions")};
}

HiveCheck checkHive(const QString &file, const QString &name, const std::atomic<bool> *cancel)
{
    if (file.isEmpty() || !QFileInfo::exists(file)) {
        HiveCheck c;
        c.name = name;
        c.problem = tr("It's missing.");
        return c;
    }
    reg::Hive hive;
    QString error;
    const bool opened = QFileInfo(file).size() > 0 && hive.open(file, &error);
    if (QFileInfo(file).size() == 0)
        error = tr("the file is empty");
    HiveCheck c = check(hive, opened, error, name, cancel);
    c.file = file;
    return c;
}

HiveCheck checkHiveBytes(const QByteArray &bytes, const QString &name)
{
    reg::Hive hive;
    QString error;
    const bool opened = hive.load(bytes, {}, &error);
    return check(hive, opened, error, name, nullptr);
}

QVector<HiveCheck> checkHives(const QString &root, const std::atomic<bool> *cancel)
{
    QVector<HiveCheck> out;
    const QString cfg = config(root);
    for (const QString &name : kHives)
        out << checkHive(cfg.isEmpty() ? QString() : find(cfg, name), name, cancel);
    return out;
}

QVector<EarlierCopy> regBackCopies(const QString &root)
{
    QVector<EarlierCopy> out;
    const QString folder = find(root, QStringLiteral("Windows/System32/config/RegBack"));
    if (folder.isEmpty())
        return out;
    for (const QString &name : kHives) {
        const QString file = find(folder, name);
        const QFileInfo info(file);
        if (file.isEmpty() || info.size() < 4096)
            continue; // Windows has left these empty since 2018, unless told to keep them
        EarlierCopy c;
        c.hive = name;
        c.from = tr("Windows' own backup (RegBack)");
        c.when = info.lastModified();
        c.file = file;
        c.read = [file](QString *why) {
            QFile f(file);
            if (!f.open(QIODevice::ReadOnly)) {
                *why = f.errorString();
                return QByteArray();
            }
            return f.readAll();
        };
        out << c;
    }
    return out;
}

quint64 ntfsRecord(const QString &file)
{
#ifdef Q_OS_WIN
    const HANDLE h = CreateFileW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(file).utf16()), 0,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    BY_HANDLE_FILE_INFORMATION info {};
    const bool ok = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    // The low 48 bits are the record; the rest is how many times it's been used.
    return ok ? ((quint64(info.nFileIndexHigh) << 32) | info.nFileIndexLow) & 0xFFFFFFFFFFFFull : 0;
#else
    struct stat st {};
    return ::stat(QFile::encodeName(file).constData(), &st) == 0 ? quint64(st.st_ino) : 0;
#endif
}

QVector<EarlierCopy> shadowCopies(const std::shared_ptr<lost::Source> &volume, const QHash<QString, quint64> &records, QString *problem)
{
    QVector<EarlierCopy> out;
    const lost::Extent whole {0, volume->size()};
    const QVector<vss::Copy> copies = vss::copies(*volume, whole, problem);
    for (int i = int(copies.size()) - 1; i >= 0; --i) {
        const vss::Copy copy = copies[i];
        const QDateTime when = QDateTime::fromMSecsSinceEpoch(copy.created);
        for (auto it = records.constBegin(); it != records.constEnd(); ++it) {
            if (it.value() == 0)
                continue;
            EarlierCopy c;
            c.hive = it.key();
            // To the second: Windows often makes two within a minute (one before an update, say).
            c.from = tr("System Restore's copy of %1").arg(when.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
            c.when = when;
            const QString name = it.key();
            const quint64 record = it.value();
            c.read = [volume, whole, i, name, record](QString *why) {
                const auto shadow = vss::open(volume, whole, i, why);
                if (!shadow)
                    return QByteArray();
                const auto file = lost::ntfsFile(shadow, record, name, why);
                if (!file)
                    return QByteArray();
                if (file->size() > 2ull * 1024 * 1024 * 1024) {
                    *why = tr("it's far too big for a registry file");
                    return QByteArray();
                }
                QByteArray bytes(qsizetype(file->size()), '\0');
                file->read(0, bytes.data(), quint64(bytes.size()));
                if (file->unreadable(0, quint64(bytes.size()))) {
                    *why = tr("parts of it couldn't be read");
                    return QByteArray();
                }
                return bytes;
            };
            out << c;
        }
    }
    return out;
}

bool putBack(const QString &root, const QString &hive, const QByteArray &bytes, QStringList *done, QString *error)
{
    const HiveCheck c = checkHiveBytes(bytes, hive);
    if (c.state == HiveState::Damaged) {
        *error = tr("That copy of %1 isn't whole either: %2").arg(hive, c.problem);
        return false;
    }
    const QString cfg = config(root);
    if (cfg.isEmpty()) {
        *error = tr("There's no Windows\\System32\\config folder on that drive.");
        return false;
    }
    if (!QFileInfo(cfg).isWritable()) {
        *error = tr("The drive is open read-only: open it again so it can be changed.");
        return false;
    }
    const QString when = stamp();
    const QString now = find(cfg, hive);
    const QString target = now.isEmpty() ? cfg + QLatin1Char('/') + hive : now;
    const QString fresh = target + QStringLiteral(".new-diskanvil");
    QFile::remove(fresh);
    QFile out(fresh);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.flush()) {
        *error = tr("Couldn't write it: %1").arg(out.errorString());
        out.remove();
        return false;
    }
    out.close();
    if (!now.isEmpty()) {
        if (!setAside(now, when, done, error))
            return false;
        // Its logs belong to the one set aside: Windows would put them back onto the earlier one.
        for (const QString &log : logsOf(now))
            if (!setAside(log, when, done, error))
                return false;
    }
    if (!QFile::rename(fresh, target)) {
        *error = tr("Couldn't put it in place as %1.").arg(QDir::toNativeSeparators(target));
        return false;
    }
    *done << tr("%1 put back from the earlier copy").arg(hive);
    return true;
}

} // namespace winstart
