// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Rescue Copy against a device with bad sectors (dm-dust), an unreadable stretch
// (dm-error) and a slow stretch (dm-delay, so it can be stopped partway). Needs root.

#include "testkit.h"

#include "../src/backup/rescuecopy.h"

#include <QElapsedTimer>
#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <functional>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr qint64 MiB = 1024 * 1024;
constexpr qint64 kSize = 48 * MiB;

struct Run {
    bool completed = false;
    QString message;
    QStringList phases; // every phase it reported, in order, once each
    qint64 ms = 0;
    int zones = 0;          // stretches left for last as a dying head's
    int failedBeforeTrim = -1; // read errors met before it started narrowing down bad areas
};

Run rescue(const QString &device, const QString &image, const QString &map, double stopAt = 0,
           const std::function<void(RescueCopy &)> &setup = {}, qint64 size = kSize)
{
    Run r;
    const int source = ::open(QFile::encodeName(device).constData(), O_RDONLY | O_DIRECT | O_CLOEXEC);
    const int target = ::open(QFile::encodeName(image).constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    RescueCopy copy(source, quint64(size), target, map);
    QObject::connect(&copy, &RescueCopy::progress, [&](quint64 rescued, quint64, quint64 total, const QString &phase) {
        if (r.phases.isEmpty() || r.phases.last() != phase)
            r.phases << phase;
        if (r.failedBeforeTrim < 0 && phase == QLatin1String("Narrowing down bad areas"))
            r.failedBeforeTrim = copy.failedReads();
        if (stopAt > 0 && rescued >= quint64(total * stopAt))
            copy.cancel();
    });
    QObject::connect(&copy, &RescueCopy::finished, [&](bool completed, const QString &m) {
        r.completed = completed;
        r.message = m;
    });
    if (setup)
        setup(copy);
    QElapsedTimer t;
    t.start();
    copy.run();
    r.ms = t.elapsed();
    r.zones = int(copy.headZones().size());
    return r;
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

void dyingHeadTests();

void rescueTests()
{
    for (const char *module : {"dm-dust", "dm-delay"})
        sh(QStringLiteral("modprobe"), {QLatin1String(module)});
    QTemporaryDir dir;
    const QString backing = dir.filePath(QStringLiteral("source.img"));
    QByteArray original(kSize, Qt::Uninitialized);
    QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(original.data()), int(kSize / 4));
    {
        QFile f(backing);
        if (!f.open(QIODevice::WriteOnly))
            return report(false, QStringLiteral("create test image"));
        f.write(original);
    }
    const QString loop = sh(QStringLiteral("losetup"), {QStringLiteral("-f"), QStringLiteral("--show"), backing});
    const QString dust = QStringLiteral("diskforge-rescue-dust");
    const QString name = QStringLiteral("diskforge-rescue-test");
    const QString device = QStringLiteral("/dev/mapper/") + name;
    // Sectors 1000 and 50003 are bad; 20 MiB to 20.25 MiB can't be read at all; the
    // last 18 MiB read slowly.
    sh(QStringLiteral("dmsetup"), {QStringLiteral("create"), dust, QStringLiteral("--table"), QStringLiteral("0 98304 dust %1 0 512").arg(loop)});
    for (const char *block : {"1000", "50003"})
        sh(QStringLiteral("dmsetup"), {QStringLiteral("message"), dust, QStringLiteral("0"), QStringLiteral("addbadblock"), QLatin1String(block)});
    sh(QStringLiteral("dmsetup"), {QStringLiteral("message"), dust, QStringLiteral("0"), QStringLiteral("enable")});
    const QString d = QStringLiteral("/dev/mapper/") + dust;
    int code = -1;
    sh(QStringLiteral("dmsetup"), {QStringLiteral("create"), name, QStringLiteral("--table"),
                                   QStringLiteral("0 40960 linear %1 0\n40960 512 error\n41472 19968 linear %1 41472\n61440 36864 delay %1 61440 40").arg(d)},
       &code);
    report(code == 0, QStringLiteral("create a failing test drive"));

    QByteArray expected = original;
    QVector<quint64> badSectors = {1000, 50003};
    for (quint64 s = 40960; s < 41472; ++s)
        badSectors << s;
    for (quint64 s : badSectors)
        std::memset(expected.data() + s * 512, 0, 512);

    // In one go.
    const QString image = dir.filePath(QStringLiteral("rescued.img"));
    const QString map = image + QStringLiteral(".map");
    Run r = rescue(device, image, map);
    report(r.zones == 0, QStringLiteral("scattered bad spots aren't taken for a dying head"));
    report(r.completed, QStringLiteral("rescue runs to the end"), r.message);
    RescueMap loaded;
    QString error;
    const bool parsed = loaded.load(map, quint64(kSize), &error);
    report(parsed && loaded.done(), QStringLiteral("its map file reads back and says finished"), error);
    report(loaded.total(RescueMap::Bad) == quint64(badSectors.size()) * 512, QStringLiteral("exactly the bad sectors are marked bad"),
           QStringLiteral("%1 bytes").arg(loaded.total(RescueMap::Bad)));
    bool marked = true;
    for (quint64 s : badSectors)
        marked = marked && loaded.statusAt(s * 512) == RescueMap::Bad;
    report(marked && loaded.statusAt(999 * 512) == RescueMap::Finished && loaded.statusAt(1001 * 512) == RescueMap::Finished,
           QStringLiteral("and their neighbours are rescued"));
    report(readFile(image) == expected, QStringLiteral("the copy matches, with zeros where it couldn't read"));
    if (QFile::exists(QStringLiteral("/usr/bin/ddrescuelog"))) {
        const QString tally = sh(QStringLiteral("ddrescuelog"), {QStringLiteral("-t"), map});
        report(tally.contains(QLatin1String("bad areas:")), QStringLiteral("ddrescue accepts the map file"), tally.section(QLatin1Char('\n'), 0, 0));
    }

    // Stopped partway, then continued.
    const QString image2 = dir.filePath(QStringLiteral("resumed.img"));
    const QString map2 = image2 + QStringLiteral(".map");
    r = rescue(device, image2, map2, 0.4);
    report(!r.completed && QFile::exists(map2), QStringLiteral("stopping keeps the progress"), r.message);
    RescueMap partial;
    partial.load(map2, quint64(kSize), &error);
    const bool partialOk = !partial.done() && partial.statusAt(2 * MiB) == RescueMap::Finished;
    report(partialOk, QStringLiteral("the saved map shows a partial rescue"),
           QStringLiteral("%1 rescued").arg(partial.total(RescueMap::Finished) / MiB));
    // Stamp a marker into a part that's already done: continuing must not copy it again.
    {
        QFile f(image2);
        if (f.open(QIODevice::ReadWrite) && f.seek(2 * MiB))
            f.write(QByteArray(4096, char(0xEE)));
    }
    r = rescue(device, image2, map2);
    QByteArray resumed = readFile(image2);
    report(r.completed && resumed.mid(2 * MiB, 4096) == QByteArray(4096, char(0xEE)), QStringLiteral("continuing skips what was already copied"), r.message);
    resumed.replace(2 * MiB, 4096, expected.mid(2 * MiB, 4096));
    report(resumed == expected, QStringLiteral("and the result is the same as in one go"));

    // Going easy on the drive. A speed limit on a drive that reads fine: 48 MiB at 16 MB/s
    // can't take less than about 3 s (without it, a fraction of a second).
    const QString image3 = dir.filePath(QStringLiteral("limited.img"));
    r = rescue(loop, image3, image3 + QStringLiteral(".map"), 0, [](RescueCopy &c) { c.setSpeedLimit(16 * 1000 * 1000); });
    const double rate = double(kSize) / (double(r.ms) / 1000);
    report(r.completed && rate <= 16e6 * 1.1, QStringLiteral("the speed limit holds"),
           QStringLiteral("%1 MB/s over %2 s").arg(rate / 1e6, 0, 'f', 1).arg(r.ms / 1000.0, 0, 'f', 1));
    // Resting after errors in a row: the 512 unreadable sectors are read one by one at the
    // end, so 200 in a row happen twice there.
    const QString image4 = dir.filePath(QStringLiteral("rested.img"));
    r = rescue(device, image4, image4 + QStringLiteral(".map"), 0, [](RescueCopy &c) { c.setErrorRest(200, 1); });
    const int rests = int(std::count_if(r.phases.cbegin(), r.phases.cend(), [](const QString &p) { return p.startsWith(QLatin1String("Resting for")); }));
    report(r.completed && rests >= 2 && readFile(image4) == expected, QStringLiteral("it rests after that many read errors in a row, and the copy is the same"),
           QStringLiteral("%1 rests").arg(rests));
    // Paused (too hot): it waits until it isn't.
    const QString image5 = dir.filePath(QStringLiteral("paused.img"));
    std::thread later;
    r = rescue(loop, image5, image5 + QStringLiteral(".map"), 0, [&later](RescueCopy &c) {
        c.setPaused(true);
        later = std::thread([&c] {
            QThread::msleep(1500);
            c.setPaused(false);
        });
    });
    later.join();
    report(r.completed && r.ms >= 1400 && r.phases.contains(QStringLiteral("Resting while the drive cools down")),
           QStringLiteral("paused for the heat, it waits, then carries on"), QStringLiteral("%1 s").arg(r.ms / 1000.0, 0, 'f', 1));

    // A map for a different-size drive is refused.
    RescueMap wrong;
    report(!wrong.load(map, quint64(kSize) * 2, &error), QStringLiteral("a map for another drive is refused"), error);

    sh(QStringLiteral("dmsetup"), {QStringLiteral("remove"), name});
    sh(QStringLiteral("dmsetup"), {QStringLiteral("remove"), dust});
    sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
    dyingHeadTests();
}

// A dying head: bad stripes of 512 KiB every 4 MiB, the way a drive's bands take turns between
// its heads. Once they've come round a few times the rest are jumped over and left for last:
// far fewer failed reads before the good data is all off, and the same copy in the end.
void dyingHeadTests()
{
    // 256 MiB three times over: not in /tmp, which is memory.
    QDir().mkpath(QDir::homePath() + QStringLiteral("/.cache"));
    QTemporaryDir dir(QDir::homePath() + QStringLiteral("/.cache/diskanvil-heads-XXXXXX"));
    constexpr qint64 size = 256 * MiB;
    const QString backing = dir.filePath(QStringLiteral("heads.img"));
    QByteArray original(size, Qt::Uninitialized);
    QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(original.data()), int(size / 4));
    {
        QFile f(backing);
        if (!f.open(QIODevice::WriteOnly))
            return report(false, QStringLiteral("dying head: create test image"));
        f.write(original);
    }
    const QString loop = sh(QStringLiteral("losetup"), {QStringLiteral("-f"), QStringLiteral("--show"), backing});
    const QString name = QStringLiteral("diskanvil-rescue-heads");
    QStringList table;
    QByteArray expected = original;
    constexpr qint64 sectorsPerMiB = 2048;
    qint64 at = 0;
    for (qint64 band = 5 * sectorsPerMiB; band + 3 * sectorsPerMiB < size / 512; band += 12 * sectorsPerMiB) {
        table << QStringLiteral("%1 %2 linear %3 %1").arg(at).arg(band - at).arg(loop);
        table << QStringLiteral("%1 %2 error").arg(band).arg(3 * sectorsPerMiB);
        std::memset(expected.data() + band * 512, 0, size_t(3 * sectorsPerMiB * 512));
        at = band + 3 * sectorsPerMiB;
    }
    table << QStringLiteral("%1 %2 linear %3 %1").arg(at).arg(size / 512 - at).arg(loop);
    int code = -1;
    sh(QStringLiteral("dmsetup"), {QStringLiteral("create"), name, QStringLiteral("--table"), table.join(QLatin1Char('\n'))}, &code);
    report(code == 0, QStringLiteral("dying head: a drive with a bad stripe every 12 MiB"));
    const QString device = QStringLiteral("/dev/mapper/") + name;

    const QString off = dir.filePath(QStringLiteral("off.img")), on = dir.filePath(QStringLiteral("on.img"));
    const Run plain = rescue(device, off, off + QStringLiteral(".map"), 0, [](RescueCopy &c) { c.setSkipDyingHead(false); }, size);
    const Run heads = rescue(device, on, on + QStringLiteral(".map"), 0, {}, size);
    report(plain.completed && readFile(off) == expected && plain.zones == 0, QStringLiteral("dying head: without it, the copy is right"));
    report(heads.completed && readFile(on) == expected && heads.zones >= 12 && heads.phases.contains(QStringLiteral("Copying the dying head's stripes, last")),
           QStringLiteral("dying head: the pattern is seen, its stripes copied last, and the copy is the same"),
           QStringLiteral("%1 stripes; %2").arg(heads.zones).arg(heads.phases.join(QStringLiteral(" | "))));
    report(heads.failedBeforeTrim >= 0 && heads.failedBeforeTrim * 2 < plain.failedBeforeTrim,
           QStringLiteral("dying head: far fewer failed reads before the good data is off"),
           QStringLiteral("%1 with it, %2 without").arg(heads.failedBeforeTrim).arg(plain.failedBeforeTrim));
    sh(QStringLiteral("dmsetup"), {QStringLiteral("remove"), name});
    sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
}

// The map on its own: splitting, merging, finding work, and a save/load round trip.
void rescueMapTests()
{
    RescueMap m(100 * 512);
    m.set(10 * 512, 5 * 512, RescueMap::Finished);
    m.set(15 * 512, 5 * 512, RescueMap::Finished);
    report(m.blocks().size() == 3 && m.blocks()[1].pos == 10 * 512 && m.blocks()[1].size == 10 * 512,
           QStringLiteral("neighbouring pieces with the same status merge"), QString::number(m.blocks().size()));
    m.set(12 * 512, 512, RescueMap::Bad);
    report(m.blocks().size() == 5 && m.statusAt(12 * 512) == RescueMap::Bad && m.statusAt(13 * 512) == RescueMap::Finished,
           QStringLiteral("a bad sector splits a finished piece"));
    m.set(0, 100 * 512, RescueMap::Finished);
    report(m.blocks().size() == 1 && m.done(), QStringLiteral("covering everything leaves one piece"));

    RescueMap w(64 * 512);
    w.set(0, 8 * 512, RescueMap::Finished);
    w.set(20 * 512, 4 * 512, RescueMap::NonTrimmed);
    quint64 start = 0, end = 0;
    report(w.next(RescueMap::NonTried, 0, &start, &end) && start == 8 * 512 && end == 20 * 512, QStringLiteral("finds the next untried piece"));
    report(w.next(RescueMap::NonTried, 22 * 512, &start, &end) && start == 24 * 512 && end == 64 * 512, QStringLiteral("and the one after a failed piece"));
    report(w.previous(RescueMap::NonTried, 64 * 512, &start, &end) && start == 24 * 512 && end == 64 * 512
               && w.previous(RescueMap::NonTried, 24 * 512, &start, &end) && start == 8 * 512 && end == 20 * 512,
           QStringLiteral("searching backwards works too"));
    report(!w.next(RescueMap::NonScraped, 0, &start, &end), QStringLiteral("nothing found when there's nothing"));

    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("test.map"));
    QString error;
    w.currentPass = 2;
    report(w.save(path, &error), QStringLiteral("save a map"), error);
    RescueMap back;
    report(back.load(path, 64 * 512, &error) && back.blocks().size() == w.blocks().size() && back.currentPass == 2
               && back.statusAt(21 * 512) == RescueMap::NonTrimmed,
           QStringLiteral("and load it again"), error);
    QFile broken(path);
    if (broken.open(QIODevice::Append))
        broken.write("0x00001000  0x00000200  +\n");
    broken.close();
    report(!back.load(path, 64 * 512, &error), QStringLiteral("a map with overlaps is refused"), error);
}
