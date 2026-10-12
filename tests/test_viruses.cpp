// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Scan for Viruses: what ClamAV's and Microsoft Defender's output says, and real ClamAV scans
// with a tiny list of our own (signatures by MD5), so the tests don't need the real 250 MB one
// or any real malware.

#include "testkit.h"

#include "../src/viruses/virusscan.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QStandardPaths>
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
                               int *scanned = nullptr, QString *problem = nullptr, int *unreadable = nullptr)
{
    virus::Scan s(engine, folders, depth);
    QVector<virus::Detection> found;
    QObject::connect(&s, &virus::Scan::found, [&found](const virus::Detection &d) { found << d; });
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

} // namespace

void virusTests()
{
    QTemporaryDir dir;
    parsing();
    scanning(dir.path());
    finding(dir.path());
}
