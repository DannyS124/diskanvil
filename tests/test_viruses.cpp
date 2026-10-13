// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Scan for Viruses: what ClamAV's and Microsoft Defender's output says, and real ClamAV scans
// with a tiny list of our own (signatures by MD5), so the tests don't need the real 250 MB one
// or any real malware.

#include "testkit.h"

#include "../src/drives/udisks.h"
#include "../src/backup/savefiles.h"
#include "../src/viruses/crashdump.h"
#include "../src/viruses/evtx.h"
#include "../src/viruses/virusscan.h"
#include "../src/viruses/whycrash.h"
#include "../src/viruses/wof.h"

#include <QRandomGenerator>
#include <QTimeZone>

#include <cstring>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QDBusMessage>
#include <QDBusConnection>
#include <QTemporaryDir>
#include <QtEndian>

#include <sys/stat.h>
#include <unistd.h>

namespace {

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(data);
}

QVector<virus::Detection> scan(const virus::Engine &engine, const QStringList &folders, virus::Depth depth, bool *completed = nullptr,
                               int *scanned = nullptr, QString *problem = nullptr, int *unreadable = nullptr, int *compressed = nullptr)
{
    virus::Scan s(engine, folders, depth);
    QVector<virus::Detection> found;
    QObject::connect(&s, &virus::Scan::found, [&found](const virus::Detection &d) { found << d; });
    QObject::connect(&s, &virus::Scan::compressedByWindows, [compressed](int n) {
        if (compressed)
            *compressed = n;
    });
    QObject::connect(&s, &virus::Scan::finished, [&](bool ok, int n, int couldntRead, const QString &why) {
        if (completed)
            *completed = ok;
        if (scanned)
            *scanned = n;
        if (problem)
            *problem = why;
        if (unreadable)
            *unreadable = couldntRead;
    });
    s.run();
    return found;
}

void parsing()
{
    qint64 signatures = 0;
    const QDateTime date = virus::listDate("ClamAV-VDB:09 Oct 2026 07-21 +0000:27786:2071233:90:0123456789abcdef:sig:raynman:1791530460"
                                           + QByteArray(400, ' '), &signatures);
    report(date == QDateTime::fromSecsSinceEpoch(1791530460) && signatures == 2071233, QStringLiteral("viruses: a list's date and size from its header"),
           date.toString(Qt::ISODate));
    report(!virus::listDate("not a list at all").isValid(), QStringLiteral("viruses: something that isn't a list has no date"));

    using virus::ClamLine;
    virus::Detection d;
    const ClamLine hit = virus::parseClamLine(QStringLiteral("/mnt/win/Users/a/Downloads/setup.exe: Win.Trojan.Agent-1234 FOUND"), d);
    report(hit == ClamLine::Found && d.path == QLatin1String("/mnt/win/Users/a/Downloads/setup.exe") && d.name == QLatin1String("Win.Trojan.Agent-1234"),
           QStringLiteral("viruses: a ClamAV hit"), d.path + QLatin1String(" | ") + d.name);
    report(virus::parseClamLine(QStringLiteral("/mnt/a: b/notes: draft.txt: OK"), d) == ClamLine::Clean
               && d.path == QLatin1String("/mnt/a: b/notes: draft.txt") && d.name.isEmpty(),
           QStringLiteral("viruses: a clean file whose name has \": \" in it"), d.path);
    report(virus::parseClamLine(QStringLiteral("/mnt/a: b/shadow: Access denied"), d) == ClamLine::Unreadable && d.path == QLatin1String("/mnt/a: b/shadow"),
           QStringLiteral("viruses: a file ClamAV couldn't read"), d.path);
    report(virus::parseClamLine(QStringLiteral("LibClamAV Warning: something about the list"), d) == ClamLine::Other
               && virus::parseClamLine(QStringLiteral("WARNING: /x/y: Can't access file"), d) == ClamLine::Other,
           QStringLiteral("viruses: ClamAV's own remarks aren't files"));

    const QString defender = QStringLiteral(
        "Scan starting...\r\nScan finished.\r\nScanning C:\\Users\\a\\Downloads found 2 threats.\r\n"
        "<===========================LIST OF DETECTED THREATS==========================>\r\n"
        "----------------------------- Threat information ------------------------------\r\n"
        "Threat                  : Virus:DOS/EICAR_Test_File\r\nResources               : 1 total\r\n"
        "    file                : C:\\Users\\a\\Downloads\\eicar.com\r\n"
        "-------------------------------------------------------------------------------\r\n"
        "----------------------------- Threat information ------------------------------\r\n"
        "Threat                  : Trojan:Win32/Wacatac.B!ml\r\nResources               : 2 total\r\n"
        "    file                : C:\\Users\\a\\Downloads\\setup.exe\r\n    file                : C:\\Users\\a\\Downloads\\setup (1).exe\r\n");
    const QVector<virus::Detection> hits = virus::parseDefender(defender);
    QStringList seen;
    for (const virus::Detection &h : hits)
        seen << h.path + QLatin1Char('=') + h.name;
    report(hits.size() == 3 && hits[0].name == QLatin1String("Virus:DOS/EICAR_Test_File") && hits[2].path == QLatin1String("C:/Users/a/Downloads/setup (1).exe"),
           QStringLiteral("viruses: Microsoft Defender's report"), seen.join(QStringLiteral(", ")));

    report(virus::quickScanWants(QStringLiteral("SETUP.EXE")) && virus::quickScanWants(QStringLiteral("invoice.pdf.js"))
               && !virus::quickScanWants(QStringLiteral("holiday.jpg")) && virus::quickScanWants(QStringLiteral("no extension")),
           QStringLiteral("viruses: a quick scan looks at programs, scripts and documents"));
    QByteArray program(1024, '\0');
    program.replace(0, 2, "MZ");
    qToLittleEndian<quint32>(0x80, program.data() + 0x3C);
    program.replace(0x80, 4, QByteArray("PE\0\0", 4));
    report(virus::disguise(QStringLiteral("invoice.pdf.exe")) == QLatin1String("DiskAnvil.Disguised.DoubleExtension")
               && virus::disguise(QStringLiteral("Report.DOCX      .scr")) == QLatin1String("DiskAnvil.Disguised.DoubleExtension")
               && virus::disguise(QStringLiteral("photos.jpg.lnk")) == QLatin1String("DiskAnvil.Disguised.DoubleExtension")
               && virus::disguise(QStringLiteral("cute") + QChar(0x202E) + QStringLiteral("gpj.exe")) == QLatin1String("DiskAnvil.Disguised.RightToLeft")
               && virus::disguise(QStringLiteral("holiday.jpg"), program) == QLatin1String("DiskAnvil.Disguised.ProgramInside")
               && virus::disguise(QStringLiteral("setup.exe")).isEmpty() && virus::disguise(QStringLiteral("my.app.v2.exe")).isEmpty()
               && virus::disguise(QStringLiteral("holiday.jpg"), "\xff\xd8\xff\xe0 a real jpeg").isEmpty()
               && virus::disguise(QStringLiteral("tool.exe"), program).isEmpty(),
           QStringLiteral("viruses: files dressed up as something else, and ones that aren't"));
    report(virus::Scan::skipped(QStringLiteral("/mnt/win/pagefile.sys")) && virus::Scan::skipped(QStringLiteral("/mnt/win/System Volume Information/x"))
               && virus::Scan::skipped(QStringLiteral("/mnt/win/System Volume Information")) && virus::Scan::skipped(QStringLiteral("/mnt/linux/.snapshots"))
               && virus::Scan::skipped(QStringLiteral("/mnt/linux/@snapshots/12/snapshot/etc"))
               && !virus::Scan::skipped(QStringLiteral("/mnt/win/Windows/System32/kernel32.dll")) && !virus::Scan::skipped(QStringLiteral("/mnt/linux/@home")),
           QStringLiteral("viruses: the page file, shadow copies and snapshots are left out"));
}

void scanning(const QString &dir)
{
    const QString clamscan = QStandardPaths::findExecutable(QStringLiteral("clamscan"));
    if (clamscan.isEmpty()) {
        out << "SKIP  ClamAV isn't installed: no real scans" << Qt::endl;
        return;
    }
    // Two "viruses" by their MD5 (a ClamAV .hdb list: md5:size:name), and files to find them in.
    const QByteArray one = "this stands in for something nasty, number one\n";
    const QByteArray two = QByteArray(70000, 'x') + "number two, a bigger one";
    const QString db = dir + QStringLiteral("/db");
    QDir().mkpath(db);
    auto line = [](const QByteArray &data, const char *name) {
        return QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex() + ':' + QByteArray::number(data.size()) + ':' + name + '\n';
    };
    writeFile(db + QStringLiteral("/diskanvil-test.hdb"), line(one, "DiskAnvil.Test.One") + line(two, "DiskAnvil.Test.Two"));
    const QString drive = dir + QStringLiteral("/drive");
    writeFile(drive + QStringLiteral("/Users/a/Downloads/setup.exe"), one);
    writeFile(drive + QStringLiteral("/Users/a/Documents/notes: draft.txt"), two);       // not a program: a quick scan skips it
    writeFile(drive + QStringLiteral("/Windows/System32/fine.dll"), "a clean library\n");
    writeFile(drive + QStringLiteral("/pagefile.sys"), one);                               // left out
    writeFile(drive + QStringLiteral("/Program Files/Some app/updater.exe"), two);

    virus::Engine engine;
    engine.kind = virus::Engine::Kind::ClamAV;
    engine.scanner = clamscan;
    engine.database = db;

    bool completed = false;
    int scanned = 0;
    QString problem;
    QVector<virus::Detection> found = scan(engine, {drive}, virus::Depth::Everything, &completed, &scanned, &problem);
    QStringList names;
    for (const virus::Detection &d : std::as_const(found))
        names << d.path.mid(drive.size()) + QLatin1Char('=') + d.name;
    names.sort();
    report(completed && scanned == 4 && found.size() == 3 && names[0].startsWith(QLatin1String("/Program Files/Some app/updater.exe=DiskAnvil.Test.Two"))
               && names[2].startsWith(QLatin1String("/Users/a/Downloads/setup.exe=DiskAnvil.Test.One")),
           QStringLiteral("viruses: ClamAV finds both, in every folder, by name"), names.join(QStringLiteral(", ")) + QLatin1Char(' ') + problem);

    found = scan(engine, {drive}, virus::Depth::Quick, &completed, &scanned);
    report(completed && scanned == 3 && found.size() == 2, QStringLiteral("viruses: a quick scan only looks at programs and the like"),
           QStringLiteral("%1 scanned, %2 found").arg(scanned).arg(found.size()));

    // A list that's damaged: said so, not a crash or "nothing found".
    const QString bad = dir + QStringLiteral("/bad-db");
    writeFile(bad + QStringLiteral("/broken.hdb"), "this:is:not:a:list\n\x01\x02");
    engine.database = bad;
    found = scan(engine, {drive}, virus::Depth::Everything, &completed, &scanned, &problem);
    report(!completed && !problem.isEmpty(), QStringLiteral("viruses: a damaged list is reported, not taken as all clear"), problem.left(120));
    engine.database = db;

    // Several folders at once (a Btrfs drive is mounted in several places). Links aren't
    // followed (what they point at is scanned where it really is), a FIFO is never handed over
    // (ClamAV would wait on it forever), and a file that can't be read is counted, not an error.
    QDir().mkpath(drive + QStringLiteral("/Users/a/Desktop"));
    QFile::link(drive + QStringLiteral("/Users/a/Downloads/setup.exe"), drive + QStringLiteral("/Users/a/Desktop/setup.exe"));
    QFile::link(drive + QStringLiteral("/Users"), drive + QStringLiteral("/Users/a/Desktop/loop"));
    ::mkfifo(QFile::encodeName(drive + QStringLiteral("/Users/a/pipe.exe")).constData(), 0600);
    writeFile(drive + QStringLiteral("/Users/a/secret.exe"), "can't be read");
    QFile::setPermissions(drive + QStringLiteral("/Users/a/secret.exe"), {});
    const bool root = ::geteuid() == 0;
    int unreadable = -1;
    found = scan(engine, {drive + QStringLiteral("/Users"), drive + QStringLiteral("/Program Files")}, virus::Depth::Quick, &completed, &scanned,
                 &problem, &unreadable);
    report(completed && scanned == 3 && found.size() == 2 && unreadable == (root ? 0 : 1),
           QStringLiteral("viruses: two folders, no links or pipes, and what can't be read is counted"),
           QStringLiteral("%1 scanned, %2 found, %3 unreadable %4").arg(scanned).arg(found.size()).arg(unreadable).arg(problem));
    QFile::setPermissions(drive + QStringLiteral("/Users/a/secret.exe"), QFile::ReadOwner | QFile::WriteOwner);

    // Dressed-up files are found whatever the antivirus says about them.
    const QString dressed = dir + QStringLiteral("/dressed");
    QByteArray program(1024, '\0');
    program.replace(0, 2, "MZ");
    qToLittleEndian<quint32>(0x80, program.data() + 0x3C);
    program.replace(0x80, 4, QByteArray("PE\0\0", 4));
    writeFile(dressed + QStringLiteral("/invoice.pdf.exe"), "not really anything");
    writeFile(dressed + QStringLiteral("/holiday.jpg"), program);
    writeFile(dressed + QStringLiteral("/cute") + QChar(0x202E) + QStringLiteral("gpj.exe"), "not really anything either");
    writeFile(dressed + QStringLiteral("/report.pdf"), "%PDF-1.7 a real one");
    found = scan(engine, {dressed}, virus::Depth::Quick, &completed, &scanned, &problem);
    QStringList kinds;
    for (const virus::Detection &d : std::as_const(found))
        kinds << d.name;
    kinds.sort();
    report(completed && kinds == QStringList{QStringLiteral("DiskAnvil.Disguised.DoubleExtension"), QStringLiteral("DiskAnvil.Disguised.ProgramInside"),
                                             QStringLiteral("DiskAnvil.Disguised.RightToLeft")},
           QStringLiteral("viruses: a scan finds the dressed-up ones too"), kinds.join(QStringLiteral(", ")));

    // Stop: quickly, and it says it stopped.
    for (int i = 0; i < 3000; ++i)
        writeFile(drive + QStringLiteral("/many/file%1.exe").arg(i), QByteArray::number(i));
    virus::Scan s(engine, {drive}, virus::Depth::Everything);
    bool finished = false;
    QString why;
    QObject::connect(&s, &virus::Scan::phase, [&s](const QString &) { s.cancel(); });
    QObject::connect(&s, &virus::Scan::finished, [&](bool ok, int, int, const QString &message) {
        finished = !ok;
        why = message;
    });
    QElapsedTimer t;
    t.start();
    s.run();
    report(finished && why == QLatin1String("Stopped.") && t.elapsed() < 10000, QStringLiteral("viruses: Stop stops it"),
           QStringLiteral("%1 ms: %2").arg(t.elapsed()).arg(why));
}

void finding(const QString &dir)
{
    // DiskAnvil's own list folder is picked when it's the newest there is.
    const QByteArray oldHome = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", (dir + QStringLiteral("/data")).toUtf8());
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    writeFile(dir + QStringLiteral("/data/diskanvil/clamav/daily.cld"),
              "ClamAV-VDB:today:27790:12345:90:md5:sig:builder:" + QByteArray::number(now) + QByteArray(450, ' '));
    const virus::Engine e = virus::findEngine();
    if (e.kind == virus::Engine::Kind::ClamAV) {
        report(e.database == dir + QStringLiteral("/data/diskanvil/clamav") && e.databaseWritable && e.signatures == 12345
                   && e.listDate == QDateTime::fromSecsSinceEpoch(now),
               QStringLiteral("viruses: DiskAnvil's own, newest list is the one used"), e.database);
    } else {
        out << "SKIP  ClamAV isn't installed: which list is used" << Qt::endl;
    }
    if (oldHome.isNull())
        qunsetenv("XDG_DATA_HOME");
    else
        qputenv("XDG_DATA_HOME", oldHome);
}

// Files Windows keeps compressed (WOF), on a volume Windows made (tests/data/wof), with each of
// Linux's NTFS drivers here: they're read, or told apart from files that can't be read for other
// reasons. ntfs3 reads them when it's built to (Arch's is; Debian's isn't); ntfs-3g shows them as
// links unless its plugin is there.
void compressedWith(const QString &root, const QString &driver, const QString &dir)
{
    const QString name = QStringLiteral("compressed files (%1): ").arg(driver);
    const QByteArray text = wofText(), mixed = wofMixed();
    report(QCryptographicHash::hash(mixed, QCryptographicHash::Sha256).toHex() == "aa6615068a5b05345287dff5e0c022b15451add260808557d79f54f0f9acd239",
           QStringLiteral("compressed files: the test's own files made again are the same as Windows' ones"));
    auto read = [](const QString &path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    const QString plain = root + QStringLiteral("/plain.txt");
    report(read(plain) == text && !wof::compressed(plain), name + QStringLiteral("a file Windows left as it is reads, and isn't called compressed"));
    // compact.exe's four ways, and NTFS's own compression (which both drivers read).
    const QVector<QPair<QString, QByteArray>> packed = {
        {QStringLiteral("/Windows/System32/drivers/anvil.sys"), text}, {QStringLiteral("/Program Files/Tool/tool.exe"), text},
        {QStringLiteral("/Mixed/mixed4k.bin"), mixed},                 {QStringLiteral("/Windows/System32/lzx.dll"), text},
        {QStringLiteral("/Mixed/mixedlzx.bin"), mixed},                {QStringLiteral("/Windows/System32/x8.dll"), text},
        {QStringLiteral("/Windows/System32/x16.dll"), text}};
    QStringList how;
    int notRead = 0;
    bool ok = true;
    for (const auto &[file, expect] : packed) {
        const QString path = root + file;
        if (read(path) == expect) {
            how << QStringLiteral("read");
            ok = ok && !wof::compressed(path);
        } else {
            ++notRead;
            how << (QFileInfo(path).isSymbolicLink() ? QStringLiteral("a link") : QStringLiteral("can't be read"));
            ok = ok && wof::compressed(path) && wof::changed(path).date().year() >= 2026;
        }
    }
    report(ok, name + QStringLiteral("the ones compact.exe compressed are read, or known as compressed"), how.join(QStringLiteral(", ")));
    report(read(root + QStringLiteral("/Docs/notes.txt")) == text && read(root + QStringLiteral("/Docs/mixed.bin")) == mixed,
           name + QStringLiteral("NTFS's own compression is read by the driver"));
    report(wof::inWindowsFolder(root, root + QStringLiteral("/Windows/System32/drivers/anvil.sys"))
               && wof::inWindowsFolder(root, root + QStringLiteral("/WINDOWS/system32/x.dll"))
               && !wof::inWindowsFolder(root, root + QStringLiteral("/Program Files/Tool/tool.exe"))
               && !wof::inWindowsFolder(root, root + QStringLiteral("/Users/a/Windows/x.exe")),
           name + QStringLiteral("only the Windows folder's count as Windows' own"));

    // DiskAnvil's own unpacking, from the drive: every one of them, whatever the driver can do.
    wof::Reader reader;
    QString why;
    QByteArray probe;
    const bool canOpen = reader.read(plain, &probe, &why);
    if (!canOpen) {
        out << "SKIP  " << name << "DiskAnvil can't open the drive itself here (" << why << ")" << Qt::endl;
    } else {
        QStringList wrong;
        for (const auto &[file, expect] : packed) {
            QByteArray got;
            if (!reader.read(root + file, &got, &why) || got != expect)
                wrong << file + QStringLiteral(": ") + (why.isEmpty() ? QStringLiteral("%1 bytes, not the same").arg(got.size()) : why);
        }
        for (const QString &file : {QStringLiteral("/Docs/notes.txt"), QStringLiteral("/Docs/mixed.bin"), QStringLiteral("/plain.txt")}) {
            QByteArray got;
            if (!reader.read(root + file, &got, &why) || got != (file.contains(QLatin1String("mixed")) ? mixed : text))
                wrong << file + QStringLiteral(": ") + why;
        }
        report(probe == text && wrong.isEmpty(), name + QStringLiteral("DiskAnvil unpacks each of them itself (XPRESS 4K, 8K, 16K, LZX, NTFS's LZNT1)"),
               wrong.join(QStringLiteral("; ")));
        const QString copy = dir + QStringLiteral("/unpacked-lzx.bin");
        QFile::remove(copy);
        report(reader.copy(root + QStringLiteral("/Mixed/mixedlzx.bin"), copy, &why) && read(copy) == mixed
                   && QFileInfo(copy).lastModified().date().year() >= 2026,
               name + QStringLiteral("and copies one out whole, with its date"), why);
        QByteArray none;
        report(!reader.read(root + QStringLiteral("/Gone"), &none, &why) && !reader.read(QStringLiteral("/etc/hostname"), &none, &why),
               name + QStringLiteral("a folder, or a file on another drive, isn't read as anything"), why);
    }

    // Save My Files: copied out whole, unpacked by DiskAnvil when Linux can't read them.
    savefiles::Folder folder;
    folder.user = QStringLiteral("test");
    folder.name = QStringLiteral("Mixed");
    folder.path = root + QStringLiteral("/Mixed");
    const QString to = dir + QStringLiteral("/saved-") + driver;
    QDir().mkpath(to);
    savefiles::Saver saver({folder}, {}, to);
    savefiles::Result saved;
    QObject::connect(&saver, &savefiles::Saver::finished, [&saved](const savefiles::Result &r) { saved = r; });
    saver.run();
    const QString copies = saved.folder + QStringLiteral("/test/Mixed/");
    if (canOpen || notRead == 0)
        report(saved.files == 2 && saved.failed.isEmpty() && read(copies + QStringLiteral("mixed4k.bin")) == mixed && read(copies + QStringLiteral("mixedlzx.bin")) == mixed,
               name + QStringLiteral("Save My Files copies them out whole"), saved.failed.join(QStringLiteral("; ")));
    else
        report(saved.files == 0 && saved.failed.size() == 2 && saved.failed.first().contains(QLatin1String("compressed")),
               name + QStringLiteral("Save My Files says which it couldn't unpack"), saved.failed.join(QStringLiteral("; ")));

    const QString clamscan = QStandardPaths::findExecutable(QStringLiteral("clamscan"));
    if (clamscan.isEmpty()) {
        out << "SKIP  ClamAV isn't installed: compressed files aren't scanned" << Qt::endl;
        return;
    }
    // A "virus" that's the mixed file: found in each of the three, whatever the driver can read.
    const QString db = dir + QStringLiteral("/wof-db");
    writeFile(db + QStringLiteral("/diskanvil-test.hdb"),
              QCryptographicHash::hash(mixed, QCryptographicHash::Md5).toHex() + ':' + QByteArray::number(mixed.size()) + ":DiskAnvil.Test.Mixed\n");
    virus::Engine engine;
    engine.kind = virus::Engine::Kind::ClamAV;
    engine.scanner = clamscan;
    engine.database = db;
    bool completed = false;
    int scanned = 0, unreadable = -1, left = -1;
    const QVector<virus::Detection> found = scan(engine, {root}, virus::Depth::Everything, &completed, &scanned, nullptr, &unreadable, &left);
    QStringList where;
    for (const virus::Detection &d : found)
        where << QDir(root).relativeFilePath(d.path);
    where.sort();
    const QStringList expect = {QStringLiteral("Docs/mixed.bin"), QStringLiteral("Mixed/mixed4k.bin"), QStringLiteral("Mixed/mixedlzx.bin")};
    if (canOpen) {
        report(completed && scanned == 10 && unreadable == 0 && left == 0 && where == expect,
               name + QStringLiteral("the scan unpacks what Linux can't read, and finds what's in it under its own name"),
               QStringLiteral("%1 scanned, %2 unreadable, %3 not unpacked, found in %4").arg(scanned).arg(unreadable).arg(left).arg(where.join(QStringLiteral(", "))));
    } else {
        report(completed && scanned == 10 - notRead && unreadable == 0 && left == notRead,
               name + QStringLiteral("the scan counts what it couldn't unpack apart from files it couldn't read"),
               QStringLiteral("%1 scanned, %2 unreadable, %3 not unpacked").arg(scanned).arg(unreadable).arg(left));
    }
}

void compressed(const QString &dir)
{
    const QString image = dir + QStringLiteral("/wof.img");
    int code = -1;
    sh(QStringLiteral("zstd"), {QStringLiteral("-dqf"), QStringLiteral(SOURCE_DIR "/tests/data/wof/wof.img.zst"), QStringLiteral("-o"), image}, &code);
    QString error = QStringLiteral("zstd isn't installed");
    const QString loop = code == 0 ? loopSetup(image, &error) : QString();
    if (loop.isEmpty()) {
        out << "SKIP  compressed files: no loop device (" << error << ")" << Qt::endl;
        return;
    }
    UDisks udisks;
    udisks.setInteractive(false);
    const Disk *disk = waitForDisk(udisks, image, [](const Disk &d) { return !d.volumes.isEmpty() && d.volumes.first().fsType == QLatin1String("ntfs"); });
    report(disk, QStringLiteral("compressed files: the volume Windows made is there"));
    // The driver UDisks picks, then UDisks' "ntfs" (ntfs-3g, where it's installed).
    QStringList seen;
    for (const QString &fstype : {QString(), QStringLiteral("ntfs")}) {
        if (!disk)
            break;
        const Volume volume = disk->volumes.first();
        QVariantMap options{{QStringLiteral("options"), QStringLiteral("ro")}, {QStringLiteral("auth.no_user_interaction"), true}};
        if (!fstype.isEmpty())
            options.insert(QStringLiteral("fstype"), fstype);
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.UDisks2"), volume.objectPath,
                                                           QStringLiteral("org.freedesktop.UDisks2.Filesystem"), QStringLiteral("Mount"));
        call << options;
        const QDBusMessage reply = QDBusConnection::systemBus().call(call, QDBus::Block, 60000);
        if (reply.type() != QDBusMessage::ReplyMessage) {
            if (fstype.isEmpty())
                report(false, QStringLiteral("compressed files: the volume mounts"), reply.errorMessage());
            else
                out << "SKIP  compressed files: no ntfs-3g (" << reply.errorMessage() << ")" << Qt::endl;
            continue;
        }
        const QString root = reply.arguments().value(0).toString();
        const QString driver = QStorageInfo(root).fileSystemType() == "fuseblk" ? QStringLiteral("ntfs-3g") : QString::fromUtf8(QStorageInfo(root).fileSystemType());
        if (!seen.contains(driver)) {
            seen << driver;
            compressedWith(root, driver, dir);
        }
        disk = waitForDisk(udisks, image, [](const Disk &d) { return !d.volumes.isEmpty() && !d.volumes.first().mountPoints.isEmpty(); });
        if (disk)
            run(udisks, QStringLiteral("compressed files: unmount"), [&] { udisks.unmount(disk->volumes.first()); });
        disk = waitForDisk(udisks, image, [](const Disk &d) { return !d.volumes.isEmpty() && d.volumes.first().mountPoints.isEmpty(); });
    }
    loopDelete(loop);
}

} // namespace

// Little-endian bytes, put where they go.
template<typename T>
void put(QByteArray &b, qint64 at, T v)
{
    if (b.size() < at + qint64(sizeof(T)))
        b.resize(at + sizeof(T), '\0');
    std::memcpy(b.data() + at, &v, sizeof(T));
}

quint64 fileTimeOf(const QDateTime &t)
{
    return quint64(t.toMSecsSinceEpoch() + 11644473600000LL) * 10000;
}

// A small blue-screen dump of our own: stop code 0xD1 in testnet.sys.
QByteArray makeDump(const QDateTime &when)
{
    QByteArray d(0x4000, '\0');
    std::memcpy(d.data(), "PAGEDU64", 8);
    put<quint32>(d, 0x0c, 22631);
    put<quint32>(d, 0x30, 0x8664);
    put<quint32>(d, 0x38, 0xd1);
    put<quint64>(d, 0x40, 0x10);
    put<quint64>(d, 0x48, 2);
    put<quint64>(d, 0x50, 0);
    put<quint64>(d, 0x58, 0xfffff80012341234ULL); // where: in testnet.sys
    put<quint32>(d, 0xf98, 4);
    put<quint64>(d, 0xfa8, fileTimeOf(when));
    constexpr qint64 t = 0x2000;
    put<quint32>(d, t + 0x08, 0x3ff0);
    std::memcpy(d.data() + 0x3ff0, "TRGD", 4);
    put<quint32>(d, t + 0x30, 0x2100); // the drivers
    put<quint32>(d, t + 0x34, 2);
    const auto driver = [&d](int i, const QString &path, quint64 base, quint32 size, qint64 nameAt) {
        const qint64 e = 0x2100 + 0x90 * i;
        put<quint32>(d, e, quint32(nameAt));
        put<quint64>(d, e + 0x38, base);
        put<quint32>(d, e + 0x48, size);
        put<quint32>(d, nameAt, quint32(path.size()));
        for (int c = 0; c < path.size(); ++c)
            put<quint16>(d, nameAt + 4 + 2 * c, path[c].unicode());
    };
    driver(0, QStringLiteral("\\SystemRoot\\system32\\ntoskrnl.exe"), 0xfffff80000000000ULL, 0x1000000, 0x2300);
    driver(1, QStringLiteral("\\SystemRoot\\System32\\drivers\\testnet.sys"), 0xfffff80012340000ULL, 0x20000, 0x2400);
    return d;
}

// An event log of our own, the way Windows keeps them: a template with substitutions, here two
// Kernel-Power 41 events (one a blue screen 0xD1, one the power button held).
QByteArray makeLog(const QDateTime &blueAt, const QDateTime &buttonAt)
{
    QByteArray chunk(65536, '\0');
    std::memcpy(chunk.data(), "ElfChnk\0", 8);
    int at = 512;
    const auto u8 = [&](quint8 v) { put<quint8>(chunk, at, v); at += 1; };
    const auto u16 = [&](quint16 v) { put<quint16>(chunk, at, v); at += 2; };
    const auto u32 = [&](quint32 v) { put<quint32>(chunk, at, v); at += 4; };
    const auto u64 = [&](quint64 v) { put<quint64>(chunk, at, v); at += 8; };
    const auto name = [&](const QString &n) { // inline: u32 next, u16 hash, u16 length, the letters, NUL
        u32(0);
        u16(0);
        u16(quint16(n.size()));
        for (QChar c : n)
            u16(c.unicode());
        u16(0);
    };
    const auto open = [&](const QString &n, bool attributes) {
        u8(attributes ? 0x41 : 0x01);
        u16(0xffff);
        u32(0);
        u32(quint32(at + 4));
        name(n);
        if (attributes)
            u32(0);
    };
    const auto attribute = [&](const QString &n, quint16 index, quint8 type) {
        u8(0x06);
        u32(quint32(at + 4));
        name(n);
        u8(0x0d);
        u16(index);
        u8(type);
    };
    const auto textAttribute = [&](const QString &n, const QString &value) {
        u8(0x06);
        u32(quint32(at + 4));
        name(n);
        u8(0x05);
        u8(0x01);
        u16(quint16(value.size()));
        for (QChar c : value)
            u16(c.unicode());
    };
    const auto substituted = [&](const QString &n, quint16 index, quint8 type) {
        open(n, false);
        u8(0x02);
        u8(0x0d);
        u16(index);
        u8(type);
        u8(0x04);
    };
    const auto record = [&](quint64 id, const QDateTime &when, quint32 code, quint64 button) {
        const int start = at;
        u32(0x2a2a);
        u32(0); // size, filled in below
        u64(id);
        u64(fileTimeOf(when));
        u8(0x0f), u8(1), u8(1), u8(0);
        u8(0x0c), u8(1), u32(1);
        u32(quint32(at + 4)); // the definition follows here
        u32(0);
        for (int i = 0; i < 16; ++i)
            u8(0);
        const int sizeAt = at;
        u32(0);
        const int body = at;
        u8(0x0f), u8(1), u8(1), u8(0);
        open(QStringLiteral("Event"), false);
        u8(0x02);
        open(QStringLiteral("System"), false);
        u8(0x02);
        open(QStringLiteral("Provider"), true);
        attribute(QStringLiteral("Name"), 0, 0x01);
        u8(0x03);
        substituted(QStringLiteral("EventID"), 1, 0x06);
        substituted(QStringLiteral("Level"), 2, 0x04);
        open(QStringLiteral("TimeCreated"), true);
        attribute(QStringLiteral("SystemTime"), 3, 0x11);
        u8(0x03);
        substituted(QStringLiteral("EventRecordID"), 4, 0x0a);
        u8(0x04);
        open(QStringLiteral("EventData"), false);
        u8(0x02);
        open(QStringLiteral("Data"), true);
        textAttribute(QStringLiteral("Name"), QStringLiteral("BugcheckCode"));
        u8(0x02), u8(0x0d), u16(5), u8(0x08), u8(0x04);
        open(QStringLiteral("Data"), true);
        textAttribute(QStringLiteral("Name"), QStringLiteral("PowerButtonTimestamp"));
        u8(0x02), u8(0x0d), u16(6), u8(0x0a), u8(0x04);
        u8(0x04);
        u8(0x04);
        u8(0x00);
        put<quint32>(chunk, sizeAt, quint32(at - body));
        // The values.
        const QString provider = QStringLiteral("Microsoft-Windows-Kernel-Power");
        const QVector<QPair<quint16, quint8>> sizes = {{quint16(provider.size() * 2), 0x01}, {2, 0x06}, {1, 0x04}, {8, 0x11}, {8, 0x0a}, {4, 0x08}, {8, 0x0a}};
        u32(quint32(sizes.size()));
        for (const auto &[size, type] : sizes)
            u16(size), u8(type), u8(0);
        for (QChar c : provider)
            u16(c.unicode());
        u16(41);
        u8(1);
        u64(fileTimeOf(when));
        u64(id);
        u32(code);
        u64(button);
        u32(0); // the copy of the size
        put<quint32>(chunk, start + 4, quint32(at - start));
        put<quint32>(chunk, at - 4, quint32(at - start));
    };
    record(100, blueAt.addSecs(120), 209, 0);
    record(101, buttonAt.addSecs(90), 0, fileTimeOf(buttonAt));
    put<quint32>(chunk, 48, quint32(at));
    QByteArray file(4096, '\0');
    std::memcpy(file.data(), "ElfFile\0", 8);
    return file + chunk;
}

void whyCrash()
{
    const QDateTime blueAt(QDate(2026, 9, 20), QTime(21, 14, 3), QTimeZone::UTC);
    const QDateTime buttonAt(QDate(2026, 10, 2), QTime(8, 2, 40), QTimeZone::UTC);
    const crashdump::Dump dump = crashdump::read(makeDump(blueAt));
    report(dump.ok && dump.code == 0xd1 && dump.time == blueAt && dump.build == 22631 && dump.drivers.size() == 2 && dump.complete,
           QStringLiteral("crash dump: the stop code, when and the drivers are read"), QString::number(dump.code, 16));
    report(crashdump::where(dump, dump.parameters[3]) == QLatin1String("testnet.sys+0x1234"), QStringLiteral("crash dump: an address is named by its driver"),
           crashdump::where(dump, dump.parameters[3]));
    const evtx::Log log = evtx::read(makeLog(blueAt, buttonAt));
    report(log.events.size() == 2 && log.events[0].provider == QLatin1String("Microsoft-Windows-Kernel-Power") && log.events[0].id == 41
               && log.events[0].value(QStringLiteral("BugcheckCode")) == QLatin1String("209") && log.events[0].time == blueAt.addSecs(120)
               && log.events[1].value(QStringLiteral("PowerButtonTimestamp")).toULongLong() == fileTimeOf(buttonAt),
           QStringLiteral("event log: a template's events read, with their values"),
           log.events.isEmpty() ? log.problem : log.events[0].provider + QLatin1Char(' ') + log.events[0].value(QStringLiteral("BugcheckCode")));
    whycrash::Records records;
    records.system = log;
    records.dumps.push_back({QStringLiteral("092026-1234-01.dmp"), dump});
    records.crashDumpEnabled = 0;
    records.describeDriver = [](const QString &path) { return path.endsWith(QLatin1String("testnet.sys")) ? QStringLiteral("Test Network Driver, Test Corp") : QString(); };
    const whycrash::Report r = whycrash::explain(records);
    const whycrash::Incident *blue = nullptr, *button = nullptr;
    for (const whycrash::Incident &i : r.incidents) {
        if (i.kind == whycrash::Kind::BlueScreen)
            blue = &i;
        if (i.kind == whycrash::Kind::PowerButton)
            button = &i;
    }
    report(r.incidents.size() == 2 && blue && button && r.incidents.first().kind == whycrash::Kind::PowerButton,
           QStringLiteral("why did it crash: a blue screen and a held power button, newest first"), QString::number(r.incidents.size()));
    report(blue && blue->title.contains(QLatin1String("DRIVER_IRQL_NOT_LESS_OR_EQUAL")) && blue->driver == QLatin1String("testnet.sys")
               && blue->blame.contains(QLatin1String("Test Network Driver")) && !blue->sources.filter(QLatin1String("092026-1234-01.dmp")).isEmpty(),
           QStringLiteral("why did it crash: the blue screen's driver named from its dump, with what it is and where that came from"),
           blue ? blue->blame : QString());
    report(r.summary.join(QLatin1Char(' ')).contains(QLatin1String("testnet.sys")) && r.notes.join(QLatin1Char(' ')).contains(QLatin1String("CrashDumpEnabled")),
           QStringLiteral("why did it crash: what they have in common, and a setting that hides crashes"), r.summary.join(QLatin1String(" | ")));
    whycrash::Records nothing;
    report(whycrash::explain(nothing).incidents.isEmpty() && !whycrash::explain(nothing).notes.isEmpty(), QStringLiteral("why did it crash: no log: said, not guessed"));
    const QString werText = QStringLiteral("Version=1\r\nEventType=BlueScreen\r\nEventTime=133500000000000000\r\nSig[0].Name=BCCode\r\nSig[0].Value=9f\r\nSig[1].Value=3\r\n");
    const QByteArray wer = QByteArrayLiteral("\xff\xfe") + QByteArray(reinterpret_cast<const char *>(werText.utf16()), werText.size() * 2);
    const whycrash::Records::Wer w = whycrash::parseWer(wer);
    report(w.type == QLatin1String("BlueScreen") && w.code == 0x9f && w.parameters[0] == 3 && w.when.isValid(), QStringLiteral("why did it crash: a Windows Error Reporting note read"));
    // Damaged files: never trusted, never a crash.
    QRandomGenerator random(77);
    bool survived = true;
    const QByteArray goodLog = makeLog(blueAt, buttonAt), goodDump = makeDump(blueAt);
    for (int i = 0; i < 400; ++i) {
        QByteArray l = goodLog, d = goodDump;
        for (int k = 0; k < 1 + i % 40; ++k) {
            l[4096 + 512 + int(random.bounded(2000))] = char(random.bounded(256));
            d[int(random.bounded(d.size()))] = char(random.bounded(256));
        }
        if (i % 7 == 0)
            l.truncate(4096 + 512 + int(random.bounded(65000)));
        const evtx::Log rl = evtx::read(l);
        const crashdump::Dump rd = crashdump::read(d);
        survived = survived && rl.events.size() <= 2;
        Q_UNUSED(rd)
    }
    report(survived, QStringLiteral("why did it crash: 400 damaged logs and dumps read without a crash"));
    // Real ones, when they're here (not in the repo: Windows' own files).
    const QString samples = qEnvironmentVariable("DISKANVIL_CRASH_SAMPLES");
    if (!samples.isEmpty() && QFileInfo::exists(samples + QStringLiteral("/system.evtx"))) {
        QFile f(samples + QStringLiteral("/system.evtx"));
        const evtx::Log real = f.open(QIODevice::ReadOnly) ? evtx::read(f.readAll()) : evtx::Log();
        report(real.events.size() > 1000 && real.broken == 0, QStringLiteral("event log: a real System log read whole"), QString::number(real.events.size()));
    }
}

void virusTests()
{
    whyCrash();
    QTemporaryDir dir;
    parsing();
    scanning(dir.path());
    finding(dir.path());
    compressed(dir.path());
}
