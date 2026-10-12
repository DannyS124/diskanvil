// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "virusscan.h"

#include "applog.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QVersionNumber>
#include <QtEndian>

namespace virus {

namespace {

// A ClamAV list folder's date and size: its newest daily list says when it was made, and
// every file's header how many signatures it holds. (.cud is ClamAV's unsigned kind, for lists
// people make themselves; the tests use one.)
QDateTime folderDate(const QString &folder, qint64 *signatures)
{
    QDateTime newest;
    qint64 total = 0;
    const QStringList kinds = {QStringLiteral("*.cvd"), QStringLiteral("*.cld"), QStringLiteral("*.cud")};
    for (const QFileInfo &f : QDir(folder).entryInfoList(kinds, QDir::Files)) {
        QFile file(f.filePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        qint64 count = 0;
        const QDateTime date = listDate(file.read(512), &count);
        total += count;
        if (f.baseName() == QLatin1String("daily") && date.isValid() && (!newest.isValid() || date > newest))
            newest = date;
    }
    if (signatures)
        *signatures = total;
    return newest;
}

[[maybe_unused]] QString programFiles(const char *variable)
{
    return QDir::fromNativeSeparators(qEnvironmentVariable(variable));
}

#ifndef Q_OS_WIN
// Where file systems are mounted: other drives, and the system's own (/proc, /run...). From
// /proc/self/mountinfo, which writes spaces and the like in paths as \040.
QSet<QString> mountPoints()
{
    QSet<QString> points;
    QFile f(QStringLiteral("/proc/self/mountinfo"));
    if (!f.open(QIODevice::ReadOnly))
        return points;
    for (const QByteArray &line : f.readAll().split('\n')) {
        const QList<QByteArray> fields = line.split(' ');
        if (fields.size() <= 4)
            continue;
        const QByteArray &raw = fields[4];
        QByteArray path;
        for (qsizetype i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 3 < raw.size()) {
                path += char(raw.mid(i + 1, 3).toInt(nullptr, 8));
                i += 3;
            } else {
                path += raw[i];
            }
        }
        points.insert(QFile::decodeName(path));
    }
    return points;
}
#endif

} // namespace

QString Engine::name() const
{
    switch (kind) {
    case Kind::ClamAV:
        return QStringLiteral("ClamAV");
    case Kind::Defender:
        return QStringLiteral("Microsoft Defender");
    case Kind::None:
        break;
    }
    return {};
}

QDateTime listDate(const QByteArray &header, qint64 *signatures)
{
    // "ClamAV-VDB:<date>:<version>:<signatures>:<level>:<md5>:<signature>:<builder>:<unix time>"
    const QList<QByteArray> fields = header.left(512).split(':');
    if (fields.size() < 9 || fields[0] != "ClamAV-VDB")
        return {};
    bool ok = false;
    const qint64 count = fields[3].trimmed().toLongLong(&ok);
    if (signatures)
        *signatures = ok ? count : 0;
    const qint64 seconds = fields[8].trimmed().toLongLong(&ok);
    return ok && seconds > 1000000000 ? QDateTime::fromSecsSinceEpoch(seconds) : QDateTime();
}

Engine findEngine(bool live)
{
    Engine e;
#ifdef Q_OS_WIN
    // ClamAV for Windows when it's installed, otherwise Microsoft Defender.
    for (const QString &root : {programFiles("ProgramFiles"), programFiles("ProgramFiles(x86)")}) {
        const QString dir = root + QStringLiteral("/ClamAV");
        if (!root.isEmpty() && QFileInfo::exists(dir + QStringLiteral("/clamscan.exe"))) {
            e.kind = Engine::Kind::ClamAV;
            e.scanner = dir + QStringLiteral("/clamscan.exe");
            e.updater = dir + QStringLiteral("/freshclam.exe");
            e.database = dir + QStringLiteral("/database");
            e.databaseWritable = true;
            e.listDate = folderDate(e.database, &e.signatures);
            return e;
        }
    }
    // Defender's newest platform version keeps its scanner under ProgramData; the one in
    // Program Files is the copy Windows shipped with.
    QString scanner;
    const QString platform = programFiles("ProgramData") + QStringLiteral("/Microsoft/Windows Defender/Platform");
    QStringList versions = QDir(platform).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    std::sort(versions.begin(), versions.end(), [](const QString &a, const QString &b) {
        return QVersionNumber::fromString(a) < QVersionNumber::fromString(b);
    });
    for (auto it = versions.crbegin(); it != versions.crend() && scanner.isEmpty(); ++it) {
        if (QFileInfo::exists(platform + QLatin1Char('/') + *it + QStringLiteral("/MpCmdRun.exe")))
            scanner = platform + QLatin1Char('/') + *it + QStringLiteral("/MpCmdRun.exe");
    }
    if (scanner.isEmpty() && QFileInfo::exists(programFiles("ProgramFiles") + QStringLiteral("/Windows Defender/MpCmdRun.exe")))
        scanner = programFiles("ProgramFiles") + QStringLiteral("/Windows Defender/MpCmdRun.exe");
    if (!scanner.isEmpty()) {
        e.kind = Engine::Kind::Defender;
        e.scanner = scanner;
        e.updater = scanner;
    }
    Q_UNUSED(live)
    return e;
#else
    e.scanner = QStandardPaths::findExecutable(QStringLiteral("clamscan"));
    if (e.scanner.isEmpty())
        return e;
    e.kind = Engine::Kind::ClamAV;
    e.updater = QStandardPaths::findExecutable(QStringLiteral("freshclam"));
    // The newest list there is: DiskAnvil's own (in memory on DiskAnvil Live), or the system's.
    const QString own = live ? QString::fromLatin1(kLiveList)
                             : QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/diskanvil/clamav");
    QVector<QPair<QString, bool>> places; // folder, whether DiskAnvil can write there
    places << qMakePair(own, true) << qMakePair(QStringLiteral("/var/lib/clamav"), false);
    for (const auto &[folder, writable] : std::as_const(places)) {
        qint64 count = 0;
        const QDateTime date = folderDate(folder, &count);
        if (date.isValid() && (!e.listDate.isValid() || date > e.listDate)) {
            e.listDate = date;
            e.signatures = count;
            e.database = folder;
            e.databaseWritable = writable;
        }
    }
    if (e.database.isEmpty()) {
        // Nothing yet: the first download goes to DiskAnvil's own folder.
        e.database = own;
        e.databaseWritable = true;
    }
    return e;
#endif
}

bool quickScanWants(const QString &fileName)
{
    static const QSet<QString> kinds = {
        // programs and what they load
        QStringLiteral("exe"), QStringLiteral("dll"), QStringLiteral("sys"), QStringLiteral("scr"), QStringLiteral("com"),
        QStringLiteral("pif"), QStringLiteral("cpl"), QStringLiteral("ocx"), QStringLiteral("drv"), QStringLiteral("efi"),
        QStringLiteral("msi"), QStringLiteral("msp"), QStringLiteral("mst"), QStringLiteral("so"), QStringLiteral("elf"),
        QStringLiteral("bin"), QStringLiteral("jar"), QStringLiteral("class"), QStringLiteral("apk"), QStringLiteral("appx"),
        QStringLiteral("msix"),
        // scripts and shortcuts
        QStringLiteral("ps1"), QStringLiteral("psm1"), QStringLiteral("vbs"), QStringLiteral("vbe"), QStringLiteral("js"),
        QStringLiteral("jse"), QStringLiteral("wsf"), QStringLiteral("wsh"), QStringLiteral("hta"), QStringLiteral("bat"),
        QStringLiteral("cmd"), QStringLiteral("lnk"), QStringLiteral("url"), QStringLiteral("scf"), QStringLiteral("reg"),
        QStringLiteral("inf"), QStringLiteral("sh"), QStringLiteral("py"), QStringLiteral("chm"),
        // documents that can carry code, and archives
        QStringLiteral("doc"), QStringLiteral("docm"), QStringLiteral("dotm"), QStringLiteral("xls"), QStringLiteral("xlsm"),
        QStringLiteral("xlam"), QStringLiteral("ppt"), QStringLiteral("pptm"), QStringLiteral("rtf"), QStringLiteral("pdf"),
        QStringLiteral("one"), QStringLiteral("zip"), QStringLiteral("rar"), QStringLiteral("7z"), QStringLiteral("cab"),
        QStringLiteral("iso"), QStringLiteral("img"), QStringLiteral("vhd"), QStringLiteral("vhdx"), QStringLiteral("gz"),
        QStringLiteral("tgz"), QStringLiteral("xz"),
    };
    const qsizetype dot = fileName.lastIndexOf(QLatin1Char('.'));
    return dot < 0 ? true : kinds.contains(fileName.mid(dot + 1).toLower());
}

namespace {

const QSet<QString> &programEnds()
{
    static const QSet<QString> ends = {QStringLiteral("exe"), QStringLiteral("scr"), QStringLiteral("com"), QStringLiteral("pif"),
                                       QStringLiteral("bat"), QStringLiteral("cmd"), QStringLiteral("js"),  QStringLiteral("jse"),
                                       QStringLiteral("vbs"), QStringLiteral("vbe"), QStringLiteral("wsf"), QStringLiteral("hta"),
                                       QStringLiteral("lnk"), QStringLiteral("ps1"), QStringLiteral("msi"), QStringLiteral("cpl"),
                                       QStringLiteral("jar")};
    return ends;
}

// What people open without a second thought.
const QSet<QString> &harmlessEnds()
{
    static const QSet<QString> ends = {QStringLiteral("pdf"),  QStringLiteral("doc"),  QStringLiteral("docx"), QStringLiteral("xls"),
                                       QStringLiteral("xlsx"), QStringLiteral("ppt"),  QStringLiteral("pptx"), QStringLiteral("txt"),
                                       QStringLiteral("rtf"),  QStringLiteral("odt"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"),
                                       QStringLiteral("png"),  QStringLiteral("gif"),  QStringLiteral("bmp"),  QStringLiteral("webp"),
                                       QStringLiteral("heic"), QStringLiteral("mp3"),  QStringLiteral("mp4"),  QStringLiteral("avi"),
                                       QStringLiteral("mov"),  QStringLiteral("mkv"),  QStringLiteral("wav"),  QStringLiteral("zip"),
                                       QStringLiteral("rar"),  QStringLiteral("7z"),   QStringLiteral("csv"),  QStringLiteral("html")};
    return ends;
}

QString endOf(const QString &name)
{
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    return dot < 0 ? QString() : name.mid(dot + 1).trimmed().toLower();
}

} // namespace

bool disguiseNeedsHead(const QString &fileName)
{
    return harmlessEnds().contains(endOf(fileName));
}

QString disguise(const QString &fileName, const QByteArray &head)
{
    // The right-to-left override turns the rest of the name around on screen.
    if (fileName.contains(QChar(0x202E)))
        return QStringLiteral("DiskAnvil.Disguised.RightToLeft");
    const QString last = endOf(fileName);
    if (programEnds().contains(last)) {
        const QString rest = fileName.left(fileName.lastIndexOf(QLatin1Char('.')));
        if (rest.contains(QLatin1Char('.')) && harmlessEnds().contains(endOf(rest)))
            return QStringLiteral("DiskAnvil.Disguised.DoubleExtension");
        return {};
    }
    // A Windows program (MZ, then "PE" where the header says) named like a picture or document.
    if (harmlessEnds().contains(last) && head.size() >= 64 && head.startsWith("MZ")) {
        const quint32 pe = qFromLittleEndian<quint32>(head.constData() + 0x3C);
        if (pe >= 64 && qsizetype(pe) + 4 <= head.size() && head.mid(pe, 4) == QByteArray("PE\0\0", 4))
            return QStringLiteral("DiskAnvil.Disguised.ProgramInside");
    }
    return {};
}

bool Scan::skipped(const QString &path)
{
    static const QRegularExpression big(QStringLiteral("/(pagefile|hiberfil|swapfile)\\.sys$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression copies(QStringLiteral("/(System Volume Information|\\.snapshots|@\\.?snapshots|timeshift-btrfs)(/|$)"),
                                           QRegularExpression::CaseInsensitiveOption);
    return big.match(path).hasMatch() || copies.match(path).hasMatch();
}

Scan::Scan(Engine engine, QStringList folders, Depth depth)
    : m_engine(std::move(engine))
    , m_folders(std::move(folders))
    , m_depth(depth)
{
    qRegisterMetaType<virus::Detection>();
}

void Scan::run()
{
    qCInfo(lcOps).noquote() << "Scan for Viruses:" << m_engine.name() << "looking through" << m_folders.join(QStringLiteral(", "))
                            << (m_depth == Depth::Quick ? "(quick)" : "(everything)");
    if (m_engine.kind == Engine::Kind::Defender) {
        runDefender();
        return;
    }
    if (m_engine.kind != Engine::Kind::ClamAV) {
        Q_EMIT finished(false, 0, 0, tr("There's no antivirus to scan with. Install ClamAV."));
        return;
    }
    // The list goes to a file as it's made: a Windows drive has a million files.
    QTemporaryFile list;
    if (!list.open()) {
        Q_EMIT finished(false, 0, 0, tr("Couldn't make the list of files to scan: %1").arg(list.errorString()));
        return;
    }
    Q_EMIT phase(tr("Making a list of the files"));
    const qint64 total = listFiles(&list);
    if (m_cancel) {
        Q_EMIT finished(false, 0, 0, tr("Stopped."));
        return;
    }
    if (!list.flush()) {
        Q_EMIT finished(false, 0, 0, tr("Couldn't make the list of files to scan: %1").arg(list.errorString()));
        return;
    }
    runClamAv(list.fileName(), total);
}

// It stays on the drive being scanned: other drives and the system's folders (/proc...) mounted
// inside it are left out, and so are links and Windows' junctions, which point at files that are
// listed where they really are (and can go round in circles). A Btrfs drive's subvolumes are
// part of it, and are walked.
qint64 Scan::listFiles(QIODevice *list)
{
#ifndef Q_OS_WIN
    const QSet<QString> mounted = mountPoints();
#endif
    qint64 count = 0;
    QSet<QString> listed;
    for (const QString &file : std::as_const(m_also)) {
        if (list && !file.contains(QLatin1Char('\n')) && QFileInfo(file).isFile() && !listed.contains(file)) {
            listed.insert(file);
            list->write(QDir::toNativeSeparators(file).toUtf8() + '\n');
            ++count;
        }
    }
    QElapsedTimer sinceReport;
    sinceReport.start();
    for (const QString &root : std::as_const(m_folders)) {
        QStringList folders{root};
        while (!folders.isEmpty() && !m_cancel) {
            const QString folder = folders.takeLast();
            const QFileInfoList entries = QDir(folder).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                                                                     QDir::Unsorted);
            for (const QFileInfo &fi : entries) {
                const QString path = fi.filePath();
                if (fi.isSymbolicLink() || fi.isJunction() || skipped(path))
                    continue;
                if (fi.isDir() && !fi.isShortcut()) {
#ifndef Q_OS_WIN
                    if (mounted.contains(path))
                        continue;
#endif
                    folders << path;
                } else if ((fi.isFile() || fi.isShortcut()) && !path.contains(QLatin1Char('\n'))) {
                    // Dressed up as something else: by its name, or a program inside a picture.
                    QByteArray head;
                    if (disguiseNeedsHead(fi.fileName()) && fi.size() >= 64) {
                        QFile f(path);
                        if (f.open(QIODevice::ReadOnly))
                            head = f.read(1024);
                    }
                    const QString dressed = disguise(fi.fileName(), head);
                    if (!dressed.isEmpty()) {
                        qCInfo(lcOps).noquote() << "Scan for Viruses: dressed up as something else:" << path;
                        Q_EMIT found(Detection{path, dressed});
                    }
                    if (!list || (m_depth == Depth::Quick && !quickScanWants(fi.fileName())) || listed.contains(path))
                        continue;
                    list->write(QDir::toNativeSeparators(path).toUtf8() + '\n');
                    ++count;
                }
            }
            if (sinceReport.elapsed() > 250) {
                sinceReport.restart();
                Q_EMIT phase(tr("Making a list of the files: %L1 so far").arg(count));
            }
        }
    }
    return count;
}

void Scan::runClamAv(const QString &list, qint64 total)
{
    QProcess clam;
    clam.setProcessChannelMode(QProcess::SeparateChannels);
    clam.start(m_engine.scanner, {QStringLiteral("--stdout"), QStringLiteral("--no-summary"),
                                  QStringLiteral("--database=") + QDir::toNativeSeparators(m_engine.database),
                                  QStringLiteral("--file-list=") + QDir::toNativeSeparators(list),
                                  QStringLiteral("--max-filesize=500M"), QStringLiteral("--max-scansize=1000M"),
                                  QStringLiteral("--alert-encrypted=no")});
    if (!clam.waitForStarted(10000)) {
        Q_EMIT finished(false, 0, 0, tr("Couldn't start ClamAV: %1").arg(clam.errorString()));
        return;
    }
    Q_EMIT phase(tr("Loading the virus list (this takes a while)"));
    quint64 done = 0;
    int hits = 0, unreadable = 0;
    QElapsedTimer sinceReport;
    sinceReport.start();
    QByteArray pending;
    while (clam.state() != QProcess::NotRunning || clam.bytesAvailable() > 0) {
        if (m_cancel) {
            clam.kill();
            clam.waitForFinished(5000);
            Q_EMIT finished(false, int(done), unreadable, tr("Stopped."));
            return;
        }
        if (clam.bytesAvailable() == 0)
            clam.waitForReadyRead(500);
        pending += clam.readAllStandardOutput();
        qsizetype newline;
        while ((newline = pending.indexOf('\n')) >= 0) {
            const QString line = QString::fromUtf8(pending.left(newline)).trimmed();
            pending.remove(0, newline + 1);
            Detection d;
            const ClamLine kind = parseClamLine(line, d);
            if (kind == ClamLine::Other)
                continue;
            if (done == 0)
                Q_EMIT phase(tr("Scanning"));
            ++done;
            if (kind == ClamLine::Unreadable) {
                ++unreadable;
                qCDebug(lcOps).noquote() << "Scan for Viruses: couldn't read" << line;
            } else if (kind == ClamLine::Found) {
                d.path = QDir::fromNativeSeparators(d.path);
                ++hits;
                qCInfo(lcOps).noquote() << "Scan for Viruses: found" << d.name << "in" << d.path;
                Q_EMIT found(d);
            }
        }
        if (sinceReport.elapsed() > 250) {
            sinceReport.restart();
            Q_EMIT progress(done, quint64(total));
        }
    }
    Q_EMIT progress(done, quint64(total));
    // 0: nothing found, 1: something found, 2: it went wrong (a damaged list, no memory...).
    const QString errors = QString::fromUtf8(clam.readAllStandardError()).trimmed();
    if (clam.exitStatus() != QProcess::NormalExit || (clam.exitCode() == 2 && done == 0 && total > 0)) {
        Q_EMIT finished(false, int(done), unreadable,
                        tr("ClamAV stopped with an error: %1").arg(errors.section(QLatin1Char('\n'), -3).simplified()));
        return;
    }
    qCInfo(lcOps).noquote() << "Scan for Viruses: done," << done << "files," << hits << "found," << unreadable << "unreadable";
    Q_EMIT finished(true, int(done), unreadable, QString());
}

void Scan::runDefender()
{
    Q_EMIT phase(tr("Looking for files dressed up as something else"));
    listFiles(nullptr);
    if (m_cancel) {
        Q_EMIT finished(false, 0, 0, tr("Stopped."));
        return;
    }
    Q_EMIT phase(tr("Microsoft Defender is looking through it (it doesn't say how far it's got)"));
    Q_EMIT progress(0, 0);
    int hits = 0;
    for (const QString &folder : std::as_const(m_folders)) {
        QProcess defender;
        defender.setProcessChannelMode(QProcess::MergedChannels);
        defender.start(m_engine.scanner, {QStringLiteral("-Scan"), QStringLiteral("-ScanType"), QStringLiteral("3"), QStringLiteral("-File"),
                                          QDir::toNativeSeparators(folder), QStringLiteral("-DisableRemediation")});
        if (!defender.waitForStarted(10000)) {
            Q_EMIT finished(false, 0, 0, tr("Couldn't start Microsoft Defender: %1").arg(defender.errorString()));
            return;
        }
        while (!defender.waitForFinished(500)) {
            if (m_cancel) {
                defender.kill();
                defender.waitForFinished(5000);
                Q_EMIT finished(false, 0, 0, tr("Stopped."));
                return;
            }
        }
        const QString output = QString::fromLocal8Bit(defender.readAll());
        // 0: nothing found, 2: something found. Anything else: Defender couldn't (another
        // antivirus is in charge, the service is off...).
        if (defender.exitStatus() != QProcess::NormalExit || (defender.exitCode() != 0 && defender.exitCode() != 2)) {
            const bool off = output.contains(QLatin1String("0x800106ba"), Qt::CaseInsensitive);
            Q_EMIT finished(false, 0, 0, off ? tr("Microsoft Defender is switched off on this PC (another antivirus is in charge). Install "
                                                  "ClamAV for Windows to scan with DiskAnvil, or scan with that antivirus.")
                                             : tr("Microsoft Defender couldn't scan: %1").arg(output.simplified().right(200)));
            return;
        }
        for (const Detection &d : parseDefender(output)) {
            ++hits;
            Q_EMIT found(d);
        }
    }
    qCInfo(lcOps).noquote() << "Scan for Viruses: Defender done," << hits << "found";
    Q_EMIT finished(true, 0, 0, QString());
}

Update::Update(Engine engine, QString into)
    : m_engine(std::move(engine))
    , m_into(std::move(into))
{
}

void Update::run()
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    QTemporaryFile config;
    if (m_engine.kind == Engine::Kind::Defender) {
        p.start(m_engine.updater, {QStringLiteral("-SignatureUpdate")});
    } else {
        if (m_engine.updater.isEmpty()) {
            Q_EMIT finished(false, tr("freshclam isn't installed, so the virus list can't be updated."));
            return;
        }
        QDir().mkpath(m_into);
        // Our own settings, so it works the same whatever the system's freshclam.conf says.
        if (!config.open()) {
            Q_EMIT finished(false, config.errorString());
            return;
        }
        config.write("DatabaseDirectory " + QDir::toNativeSeparators(m_into).toUtf8() + "\n"
                     "DatabaseMirror database.clamav.net\n"
                     "Foreground yes\n"
                     "NotifyClamd no\n"
                     "LogTime no\n");
        config.flush();
        p.start(m_engine.updater, {QStringLiteral("--config-file=") + QDir::toNativeSeparators(config.fileName()), QStringLiteral("--stdout")});
    }
    if (!p.waitForStarted(10000)) {
        Q_EMIT finished(false, tr("Couldn't start the update: %1").arg(p.errorString()));
        return;
    }
    QString all;
    while (p.state() != QProcess::NotRunning || p.bytesAvailable() > 0) {
        if (m_cancel) {
            p.kill();
            p.waitForFinished(5000);
            Q_EMIT finished(false, tr("Stopped."));
            return;
        }
        p.waitForReadyRead(500);
        const QString text = QString::fromLocal8Bit(p.readAll());
        all += text;
        for (const QString &line : text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts))
            Q_EMIT progress(line.simplified());
    }
    // freshclam: 0 updated, 1 already up to date; anything else went wrong.
    const bool ok = p.exitStatus() == QProcess::NormalExit && (p.exitCode() == 0 || (m_engine.kind == Engine::Kind::ClamAV && p.exitCode() == 1));
    qCInfo(lcOps).noquote() << "Scan for Viruses: list update" << (ok ? "done" : "failed") << p.exitCode();
    Q_EMIT finished(ok, ok ? QString() : all.trimmed().section(QLatin1Char('\n'), -3).simplified());
}

namespace {

QString words(const char *text)
{
    return QCoreApplication::translate("virus", text);
}

bool has(const QString &name, std::initializer_list<const char *> parts)
{
    for (const char *p : parts) {
        if (name.contains(QLatin1String(p), Qt::CaseInsensitive))
            return true;
    }
    return false;
}

} // namespace

QString shownName(const QString &name)
{
    QString shown = name;
    if (shown.endsWith(QLatin1String(".UNOFFICIAL")))
        shown.chop(11);
    return shown;
}

int severity(const QString &name)
{
    if (name == QLatin1String("DiskAnvil.Disguised.ProgramInside"))
        return 1;
    if (name.startsWith(QLatin1String("DiskAnvil.Disguised.")))
        return 2;
    if (has(name, {"eicar", "PUA.", "PUA:", "Adware", "Unwanted", "PUADlManager", "Test."}))
        return 0;
    if (has(name, {"Heuristics", "Heur", "Suspicious", "!ml", "Generic.Susp"}))
        return 1;
    return 2;
}

QString describe(const QString &name)
{
    if (name == QLatin1String("DiskAnvil.Disguised.DoubleExtension"))
        return words("A program named like a document or picture: Windows hides the end of the name, so it looks harmless.");
    if (name == QLatin1String("DiskAnvil.Disguised.RightToLeft"))
        return words("Its name is turned around by a hidden character, so a program looks like something else.");
    if (name == QLatin1String("DiskAnvil.Disguised.ProgramInside"))
        return words("A program kept in a file named like a picture or document: malware hides its parts that way.");
    if (has(name, {"eicar"}))
        return words("A harmless test file: antivirus programs use it to show they work.");
    if (has(name, {"PUA", "Unwanted", "Adware"}))
        return words("An unwanted program: adware, a toolbar or something that changes the browser. Not dangerous as such, but "
                     "rarely wanted.");
    if (has(name, {"Heuristics", "Heur", "Suspicious", "!ml"}))
        return words("It looks suspicious from how it's built: a guess, not a known virus. It may well be fine.");
    if (has(name, {"Ransom"}))
        return words("Ransomware: it locks people's files away and asks for money to give them back.");
    if (has(name, {"Backdoor", "RAT", "Remote"}))
        return words("A backdoor: it lets someone else control the PC from far away.");
    if (has(name, {"Coinminer", "Miner"}))
        return words("A coin miner: it uses the PC to make money for someone else, and makes it slow.");
    if (has(name, {"Phish"}))
        return words("A fake web page or e-mail made to steal passwords.");
    if (has(name, {"Macro", "Doc.", "Xls.", "Ppt."}))
        return words("A document with a harmful macro: it runs malware when the document is opened.");
    if (has(name, {"Worm"}))
        return words("A worm: it spreads to other computers by itself.");
    if (has(name, {"Exploit", "CVE"}))
        return words("An exploit: it attacks a weakness in another program to get in.");
    if (has(name, {"Downloader", "Dropper", "Loader"}))
        return words("A downloader: it fetches more malware from the internet.");
    if (has(name, {"Spy", "Stealer", "Keylog", "PWS", "Password"}))
        return words("Spyware: it steals passwords, card numbers or what's typed.");
    if (has(name, {"Rootkit", "Boot"}))
        return words("A rootkit: it hides itself and other malware deep in the system.");
    if (has(name, {"Trojan"}))
        return words("A Trojan: a harmful program that pretends to be something else.");
    if (has(name, {"Virus", "Infector"}))
        return words("A virus: it adds itself to other programs.");
    return words("Known malware.");
}

ClamLine parseClamLine(const QString &line, Detection &d)
{
    auto file = [&](qsizetype ending, ClamLine kind) {
        d.path = line.left(ending);
        d.name.clear();
        return d.path.isEmpty() ? ClamLine::Other : kind;
    };
    if (line.endsWith(QLatin1String(" FOUND"))) {
        // A virus's name never has ": " in it, so the last one ends the path.
        const qsizetype colon = line.lastIndexOf(QLatin1String(": "), line.size() - 7);
        if (colon <= 0)
            return ClamLine::Other;
        file(colon, ClamLine::Found);
        d.name = line.mid(colon + 2, line.size() - 6 - colon - 2);
        return ClamLine::Found;
    }
    for (const char *end : {": OK", ": Empty file", ": Excluded", ": Symbolic link"}) {
        if (line.endsWith(QLatin1String(end)))
            return file(line.size() - qsizetype(qstrlen(end)), ClamLine::Clean);
    }
    for (const char *end : {": Access denied", ": No such file or directory", ": Can't open file or directory"}) {
        if (line.endsWith(QLatin1String(end)))
            return file(line.size() - qsizetype(qstrlen(end)), ClamLine::Unreadable);
    }
    // "<path>: <why> ERROR": the why can have ": " in it too; the first one is the best guess.
    if (line.endsWith(QLatin1String(" ERROR")) && !line.startsWith(QLatin1String("LibClamAV")) && !line.startsWith(QLatin1String("WARNING"))) {
        const qsizetype colon = line.indexOf(QLatin1String(": "));
        if (colon > 0)
            return file(colon, ClamLine::Unreadable);
    }
    return ClamLine::Other;
}

QVector<Detection> parseDefender(const QString &output)
{
    // ----------------------------- Threat information ------------------------------
    // Threat                  : Virus:DOS/EICAR_Test_File
    // Resources               : 1 total
    //     file                : C:\test\eicar.com
    QVector<Detection> out;
    QString threat;
    static const QRegularExpression field(QStringLiteral("^\\s*(\\w+)\\s*:\\s*(.+?)\\s*$"));
    for (const QString &line : output.split(QLatin1Char('\n'))) {
        const QRegularExpressionMatch m = field.match(line);
        if (!m.hasMatch())
            continue;
        const QString key = m.captured(1).toLower();
        if (key == QLatin1String("threat"))
            threat = m.captured(2);
        else if (key == QLatin1String("file") && !threat.isEmpty())
            out.push_back({QString(m.captured(2)).replace(QLatin1Char('\\'), QLatin1Char('/')), threat}); // always Windows paths
    }
    return out;
}

} // namespace virus
