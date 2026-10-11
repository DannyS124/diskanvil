// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// The block map against device-mapper test devices: a slow region (dm-delay), an
// unreadable one (dm-error) and plain good space. Needs root.

#include "testkit.h"

#include "../src/health/blockmapdata.h"
#include "../src/health/surfacescan.h"

#include <QFile>
#include <QTemporaryDir>

#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr quint64 MiB = 1024 * 1024;
constexpr quint64 kSectors = MiB / 512;

struct TestDevice {
    QString loop;
    QString name;
    QString path() const { return QStringLiteral("/dev/mapper/") + name; }
};

TestDevice create(const QTemporaryDir &dir, quint64 sizeMiB, const QString &name, const QString &table)
{
    TestDevice t;
    const QString image = dir.filePath(name + QStringLiteral(".img"));
    QFile f(image);
    if (!f.open(QIODevice::WriteOnly) || !f.resize(qint64(sizeMiB * MiB)))
        return t;
    f.close();
    t.loop = sh(QStringLiteral("losetup"), {QStringLiteral("-f"), QStringLiteral("--show"), image});
    t.name = name;
    int code = -1;
    sh(QStringLiteral("dmsetup"), {QStringLiteral("create"), name, QStringLiteral("--table"), QString(table).replace(QStringLiteral("LOOP"), t.loop)}, &code);
    if (code != 0)
        t.name.clear();
    return t;
}

void remove(const TestDevice &t)
{
    if (!t.name.isEmpty())
        sh(QStringLiteral("dmsetup"), {QStringLiteral("remove"), t.name});
    if (!t.loop.isEmpty())
        sh(QStringLiteral("losetup"), {QStringLiteral("-d"), t.loop});
}

// Scans the device into a map; stopAfterFirstBatch cancels when the first samples arrive.
bool scan(const TestDevice &t, quint64 size, BlockMapData *map, bool stopAfterFirstBatch = false)
{
    const int fd = ::open(QFile::encodeName(t.path()).constData(), O_RDONLY | O_DIRECT | O_CLOEXEC);
    if (fd < 0)
        return false;
    bool completed = false;
    SurfaceScan s(fd, size);
    QObject::connect(&s, &SurfaceScan::samples, [&](const QVector<ReadSample> &batch) {
        map->add(batch);
        if (stopAfterFirstBatch)
            s.cancel();
    });
    QObject::connect(&s, &SurfaceScan::finished, [&](bool c, const QVector<quint64> &, int) { completed = c; });
    s.run();
    return completed;
}

QString states(const BlockMapData &map)
{
    QString out;
    for (int c = 0; c < map.cells(); ++c)
        out += QLatin1Char("ugspb"[int(map.state(c))]);
    return out;
}

} // namespace

void blockMapTests()
{
    // Check a USB Stick: written, then read back fine, except one bad megabyte.
    constexpr quint64 kMiB = 1024 * 1024;
    BlockMapData stick(32 * kMiB, false);
    stick.mark(0, 32 * kMiB, BlockMapData::State::Pending);
    stick.mark(5 * kMiB, kMiB, BlockMapData::State::Bad);
    stick.markChecked(0, 16 * kMiB);
    report(stick.count(BlockMapData::State::Good) == 15 && stick.count(BlockMapData::State::Bad) == 1
               && stick.count(BlockMapData::State::Pending) == 16,
           QStringLiteral("read back fine, written blocks turn checked; bad ones stay bad"),
           QStringLiteral("%1 good, %2 bad, %3 written").arg(stick.count(BlockMapData::State::Good)).arg(stick.count(BlockMapData::State::Bad))
               .arg(stick.count(BlockMapData::State::Pending)));

    sh(QStringLiteral("modprobe"), {QStringLiteral("dm-delay")});
    QTemporaryDir dir;

    // 32 MiB: good, then 1 MiB that takes 400 ms, then 1 MiB that can't be read, then good.
    const QString table = QStringLiteral("0 %1 linear LOOP 0\n%1 %2 delay LOOP %1 400\n%3 %2 error\n%4 %5 linear LOOP %4")
                              .arg(20 * kSectors).arg(kSectors).arg(21 * kSectors).arg(22 * kSectors).arg(10 * kSectors);
    const TestDevice mixed = create(dir, 32, QStringLiteral("diskforge-map-test"), table);
    report(!mixed.name.isEmpty(), QStringLiteral("create a test device with slow and unreadable parts"), mixed.loop);
    if (!mixed.name.isEmpty()) {
        BlockMapData map(32 * MiB, false);
        const bool done = scan(mixed, 32 * MiB, &map);
        const QString s = states(map);
        report(done && map.cells() == 32, QStringLiteral("the scan covers the whole device"), s);
        report(s.left(20) == QString(20, QLatin1Char('g')) && s.mid(22) == QString(10, QLatin1Char('g')), QStringLiteral("good space shows green"));
        report(s.at(20) == QLatin1Char('s'), QStringLiteral("the slow megabyte shows orange"), QStringLiteral("slowest %1 ms, median %2 ms").arg(map.slowestMs(20)).arg(map.medianMs()));
        report(s.at(21) == QLatin1Char('b'), QStringLiteral("the unreadable megabyte shows red"));
        report(map.slowAreas() == 1, QStringLiteral("one slow area is counted"), QString::number(map.slowAreas()));

        BlockMapData stopped(32 * MiB, false);
        const bool finished = scan(mixed, 32 * MiB, &stopped, true);
        report(!finished && stopped.state(31) == BlockMapData::State::Unread && stopped.state(0) == BlockMapData::State::Good,
               QStringLiteral("a stopped scan leaves the rest grey"), states(stopped));
    }
    remove(mixed);

    // Every read takes 80 ms, like a cheap USB stick. That's the stick, not a problem.
    const TestDevice slow = create(dir, 12, QStringLiteral("diskforge-slowstick-test"),
                                   QStringLiteral("0 %1 delay LOOP 0 80").arg(12 * kSectors));
    if (!slow.name.isEmpty()) {
        BlockMapData map(12 * MiB, false);
        scan(slow, 12 * MiB, &map);
        report(map.count(BlockMapData::State::Slow) == 0 && map.count(BlockMapData::State::Good) == 12,
               QStringLiteral("a drive that's slow everywhere isn't flagged"), QStringLiteral("%1, median %2 ms").arg(states(map)).arg(map.medianMs()));
    } else {
        report(false, QStringLiteral("create a uniformly slow test device"));
    }
    remove(slow);
}
