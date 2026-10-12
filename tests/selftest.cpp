// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Runs every operation against an image file attached as a loop device.
// Operations need root (no polkit agent); --guard runs as the user.

#include "testkit.h"

#include "../src/common/format.h"
#include "../src/drives/recoverdialog.h"
#include "../src/drives/udisks.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

#include "../src/addons/addons.h"
#include "../src/addons/outputfilter.h"
#include "../src/health/benchmark.h"
#include "../src/health/surfacescan.h"

#include <QProcess>
#include <fcntl.h>
#include "../src/usb/imagewriter.h"

#include <QFileInfo>
#include <QRandomGenerator>
#include <QStandardPaths>

#include <functional>
#include <unistd.h>

namespace {

QString imagePath;


const Disk *testDisk(UDisks &udisks)
{
    for (const Disk &d : udisks.disks()) {
        if (d.isLoop && d.backingFile == imagePath)
            return &d;
    }
    return nullptr;
}

// UDisks2 updates objects a bit after the job finishes
bool waitFor(UDisks &udisks, const QString &step, const std::function<bool(const Disk &)> &check)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 15000) {
        udisks.refresh();
        const Disk *d = testDisk(udisks);
        if (d && check(*d)) {
            report(true, step);
            return true;
        }
        QCoreApplication::processEvents();
        QThread::msleep(200);
    }
    report(false, step, QStringLiteral("state never matched"));
    return false;
}

Span firstFree(const Disk &d)
{
    for (const Span &s : diskSpans(d)) {
        if (s.isFree())
            return s;
    }
    return {};
}

bool fsAvailable(const UDisks &udisks, const QString &id)
{
    for (const FsType &fs : udisks.filesystems()) {
        if (fs.id == id)
            return fs.available;
    }
    return false;
}

// returns false so the && chain stops at the first failure
bool step(UDisks &udisks, const QString &name, const std::function<void()> &op,
          const QString &expect, const std::function<bool(const Disk &)> &check)
{
    return run(udisks, name, op) && waitFor(udisks, expect, check);
}

bool resizing(UDisks &udisks);
bool extras(UDisks &udisks);

bool operations(UDisks &udisks)
{
    const QString winFs = fsAvailable(udisks, QStringLiteral("ntfs")) ? QStringLiteral("ntfs") : QStringLiteral("vfat");
    auto disk = [&]() -> const Disk & { return *testDisk(udisks); };
    constexpr quint64 MiB = 1024 * 1024;

    return waitFor(udisks, QStringLiteral("image shows up as an empty disk"),
                   [](const Disk &d) { return d.volumes.isEmpty() && d.tableType.isEmpty(); })
        && step(udisks, QStringLiteral("create GPT partition table"),
                [&] { udisks.createPartitionTable(disk(), QStringLiteral("gpt")); },
                QStringLiteral("disk is GPT"), [](const Disk &d) { return d.tableType == QLatin1String("gpt"); })
        && step(udisks, QStringLiteral("create 100 MB ext4 partition"),
                [&] { udisks.createPartition(disk(), firstFree(disk()).offset, 100 * MiB, QStringLiteral("ext4"), QStringLiteral("WKTEST")); },
                QStringLiteral("ext4 partition labelled WKTEST"), [](const Disk &d) {
                    return d.volumes.size() == 1 && d.volumes[0].fsType == QLatin1String("ext4") && d.volumes[0].label == QLatin1String("WKTEST");
                })
        && step(udisks, QStringLiteral("create FAT32 partition in the rest"),
                [&] { const Span f = firstFree(disk()); udisks.createPartition(disk(), f.offset, f.size, QStringLiteral("vfat"), QStringLiteral("DATA")); },
                QStringLiteral("second partition is FAT32"), [](const Disk &d) {
                    return d.volumes.size() == 2 && d.volumes[1].fsType == QLatin1String("vfat");
                })
        && step(udisks, QStringLiteral("mount FAT32 partition"), [&] { udisks.mount(disk().volumes[1]); },
                QStringLiteral("it is mounted"), [](const Disk &d) { return !d.volumes[1].mountPoints.isEmpty(); })
        && step(udisks, QStringLiteral("rename ext4 partition"), [&] { udisks.setLabel(disk().volumes[0], QStringLiteral("RENAMED")); },
                QStringLiteral("label is RENAMED"), [](const Disk &d) { return d.volumes[0].label == QLatin1String("RENAMED"); })
        && step(udisks, QStringLiteral("change the ext4 partition's type to Linux /home, flag it Don't mount automatically"),
                [&] {
                    const Volume &v = disk().volumes[0];
                    udisks.setPartitionTypeAndFlags(v, QStringLiteral("933ac7e1-2eb4-4f13-b844-0e14e2aef915"), true,
                                                    mergedPartitionFlags(QStringLiteral("gpt"), v.partFlags, quint64(1) << 63));
                },
                QStringLiteral("type and flags read back"), [](const Disk &d) {
                    const Volume &v = d.volumes[0];
                    return v.partType == QLatin1String("933ac7e1-2eb4-4f13-b844-0e14e2aef915") && v.partFlags == (quint64(1) << 63);
                })
        && step(udisks, QStringLiteral("format the mounted FAT32 partition as %1").arg(winFs),
                [&] { udisks.format(disk().volumes[1], winFs, QStringLiteral("WINDOWS")); },
                QStringLiteral("it is %1, unmounted, with the Microsoft partition type").arg(winFs), [&](const Disk &d) {
                    const Volume &v = d.volumes.value(1);
                    return v.fsType == winFs && v.mountPoints.isEmpty() && v.partType == QLatin1String("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7");
                })
        && step(udisks, QStringLiteral("delete ext4 partition"), [&] { udisks.deletePartition(disk().volumes[0]); },
                QStringLiteral("one partition left, free space before it"), [](const Disk &d) {
                    const QVector<Span> spans = diskSpans(d);
                    return d.volumes.size() == 1 && !spans.isEmpty() && spans.first().isFree();
                })
        && step(udisks, QStringLiteral("replace with an MBR partition table"),
                [&] { udisks.createPartitionTable(disk(), QStringLiteral("dos")); },
                QStringLiteral("disk is MBR and empty"), [](const Disk &d) { return d.tableType == QLatin1String("dos") && d.volumes.isEmpty(); })
        && step(udisks, QStringLiteral("create FAT32 partition on MBR"),
                [&] { const Span f = firstFree(disk()); udisks.createPartition(disk(), f.offset, f.size, QStringLiteral("vfat"), QStringLiteral("MBRFAT")); },
                QStringLiteral("MBR partition is FAT32 with type 0x0c"), [](const Disk &d) {
                    return d.volumes.size() == 1 && d.volumes[0].fsType == QLatin1String("vfat") && d.volumes[0].partType == QLatin1String("0x0c");
                })
        && step(udisks, QStringLiteral("mark the MBR partition bootable, type 0x0b"),
                [&] { udisks.setPartitionTypeAndFlags(disk().volumes[0], QStringLiteral("0x0b"), true, 0x80); },
                QStringLiteral("type and boot flag read back"), [](const Disk &d) {
                    return d.volumes.size() == 1 && d.volumes[0].partType == QLatin1String("0x0b") && d.volumes[0].partFlags == 0x80;
                })
        && resizing(udisks)
        && extras(udisks);
}

bool resizing(UDisks &udisks)
{
    auto disk = [&]() -> const Disk & { return *testDisk(udisks); };
    constexpr quint64 MiB = 1024 * 1024;
    auto vol = [&](int i) -> const Volume & { return disk().volumes[i]; };

    return step(udisks, QStringLiteral("new GPT table for the resize tests"),
                [&] { udisks.createPartitionTable(disk(), QStringLiteral("gpt")); },
                QStringLiteral("disk is GPT and empty"), [](const Disk &d) { return d.tableType == QLatin1String("gpt") && d.volumes.isEmpty(); })
        && step(udisks, QStringLiteral("create 64 MB ext4 partition"),
                [&] { udisks.createPartition(disk(), firstFree(disk()).offset, 64 * MiB, QStringLiteral("ext4"), QStringLiteral("GROW")); },
                QStringLiteral("it is 64 MB"), [](const Disk &d) { return d.volumes.size() == 1 && d.volumes[0].size == 64 * MiB; })
        && step(udisks, QStringLiteral("grow unmounted ext4 to 160 MB"), [&] { udisks.resize(vol(0), 160 * MiB); },
                QStringLiteral("partition is 160 MB"), [](const Disk &d) { return d.volumes[0].size == 160 * MiB; })
        && step(udisks, QStringLiteral("mount it"), [&] { udisks.mount(vol(0)); },
                QStringLiteral("file system grew with it (over 136 MB)"), [](const Disk &d) {
                    return !d.volumes[0].mountPoints.isEmpty() && d.volumes[0].fsTotal > 136 * MiB;
                })
        && step(udisks, QStringLiteral("grow mounted ext4 to 200 MB"), [&] { udisks.resize(vol(0), 200 * MiB); },
                QStringLiteral("partition is 200 MB, file system over 170 MB, still mounted"), [](const Disk &d) {
                    const Volume &v = d.volumes[0];
                    return v.size == 200 * MiB && !v.mountPoints.isEmpty() && v.fsTotal > 170 * MiB;
                })
        && step(udisks, QStringLiteral("shrink mounted ext4 to 96 MB (needs unmounting)"), [&] { udisks.resize(vol(0), 96 * MiB); },
                QStringLiteral("partition is 96 MB and unmounted"), [](const Disk &d) {
                    return d.volumes[0].size == 96 * MiB && d.volumes[0].mountPoints.isEmpty();
                })
        && step(udisks, QStringLiteral("create 400 MB Btrfs partition after it"),
                [&] { const Span f = firstFree(disk()); udisks.createPartition(disk(), f.offset, 400 * MiB, QStringLiteral("btrfs"), QStringLiteral("SHRINK")); },
                QStringLiteral("second partition is Btrfs"), [](const Disk &d) {
                    return d.volumes.size() == 2 && d.volumes[1].fsType == QLatin1String("btrfs");
                })
        && [&] {
               QString message;
               const bool ok = run(udisks, {}, [&] { udisks.resize(vol(1), 200 * MiB); }, &message);
               report(!ok && message.contains(QLatin1String("out of range")), QStringLiteral("Btrfs below 256 MB is refused"), message);
               return !ok;
           }()
        && step(udisks, QStringLiteral("shrink unmounted Btrfs to 300 MB (needs mounting)"), [&] { udisks.resize(vol(1), 300 * MiB); },
                QStringLiteral("partition is 300 MB, mounted, file system no bigger"), [](const Disk &d) {
                    const Volume &v = d.volumes[1];
                    return v.size == 300 * MiB && !v.mountPoints.isEmpty() && v.fsTotal > 0 && v.fsTotal <= 300 * MiB;
                })
        && [&] {
               QString message;
                  const bool ok = run(udisks, {}, [&] { udisks.resize(vol(1), 4096 * MiB); }, &message);
               report(!ok && message.contains(QLatin1String("out of range")), QStringLiteral("a size past the free space is refused"), message);
               return !ok;
           }();
}

// Waits for UDisks::deviceOpened and returns the fd (-1 on failure).
int openFd(UDisks &udisks, const Disk &disk, bool writable, bool benchmark)
{
    int fd = -2;
    QEventLoop loop;
    auto conn = QObject::connect(&udisks, &UDisks::deviceOpened, [&](const QString &, int f) { fd = f; loop.quit(); });
    udisks.openDevice(disk, writable, benchmark);
    if (fd == -2)
        loop.exec();
    QObject::disconnect(conn);
    return fd;
}


bool checkFails(UDisks &udisks, const QString &name, const std::function<void()> &op, const char *expect)
{
    QString message;
    const bool ok = run(udisks, {}, op, &message);
    report(!ok && message.contains(QLatin1String(expect)), name, message);
    return !ok && message.contains(QLatin1String(expect));
}

// Check/repair, mount at startup, LUKS, disk images, image writing, benchmark, wipe.
bool extras(UDisks &udisks)
{
    auto disk = [&]() -> const Disk & { return *testDisk(udisks); };
    auto vol = [&](int i) -> const Volume & { return disk().volumes[i]; };
    constexpr quint64 MiB = 1024 * 1024;

    QFile fstabFile(QStringLiteral("/etc/fstab"));
    if (!fstabFile.open(QIODevice::ReadOnly))
        return false;
    const QByteArray fstabBefore = fstabFile.readAll();
    fstabFile.close();

    bool ok = step(udisks, QStringLiteral("new GPT table for the extra tests"),
                   [&] { udisks.createPartitionTable(disk(), QStringLiteral("gpt")); },
                   QStringLiteral("disk is GPT and empty"), [](const Disk &d) { return d.tableType == QLatin1String("gpt") && d.volumes.isEmpty(); })
        && step(udisks, QStringLiteral("create 120 MB ext4 partition"),
                [&] { udisks.createPartition(disk(), firstFree(disk()).offset, 120 * MiB, QStringLiteral("ext4"), QStringLiteral("CHECKME")); },
                QStringLiteral("it exists"), [](const Disk &d) { return d.volumes.size() == 1 && d.volumes[0].fsType == QLatin1String("ext4"); })
        && run(udisks, QStringLiteral("check it"), [&] { udisks.check(vol(0)); })
        && run(udisks, QStringLiteral("repair it"), [&] { udisks.repair(vol(0)); })
        && step(udisks, QStringLiteral("make it mount at startup"), [&] { udisks.setMountAtStartup(vol(0), true); },
                QStringLiteral("fstab entry exists"), [](const Disk &d) { return !d.volumes[0].fstab.isEmpty(); })
        && step(udisks, QStringLiteral("mount it (uses the fstab entry)"), [&] { udisks.mount(vol(0)); },
                QStringLiteral("mounted at /mnt/CHECKME"), [](const Disk &d) { return d.volumes[0].mountPoints.contains(QStringLiteral("/mnt/CHECKME")); })
        && step(udisks, QStringLiteral("unmount it"), [&] { udisks.unmount(vol(0)); },
                QStringLiteral("unmounted"), [](const Disk &d) { return d.volumes[0].mountPoints.isEmpty(); })
        && step(udisks, QStringLiteral("stop it mounting at startup"), [&] { udisks.setMountAtStartup(vol(0), false); },
                QStringLiteral("fstab entry gone"), [](const Disk &d) { return d.volumes[0].fstab.isEmpty(); });

    if (!fstabFile.open(QIODevice::ReadOnly))
        return false;
    const QByteArray fstabAfter = fstabFile.readAll();
    fstabFile.close();
    report(fstabAfter == fstabBefore, QStringLiteral("/etc/fstab is back to how it was"));
    if (fstabAfter != fstabBefore) {
        QFile restore(QStringLiteral("/etc/fstab"));
        if (restore.open(QIODevice::WriteOnly | QIODevice::Truncate))
            restore.write(fstabBefore);
        return false;
    }
    QDir(QStringLiteral("/mnt/CHECKME")).removeRecursively();
    if (!ok)
        return false;

    ok = step(udisks, QStringLiteral("create 150 MB encrypted ext4 partition"),
              [&] { const Span f = firstFree(disk()); udisks.createPartition(disk(), f.offset, 150 * MiB, QStringLiteral("ext4"), QStringLiteral("SECRET"), QStringLiteral("first-passphrase")); },
              QStringLiteral("it is LUKS"), [](const Disk &d) { return d.volumes.size() == 2 && d.volumes[1].fsType == QLatin1String("crypto_LUKS"); })
        && step(udisks, QStringLiteral("lock it"), [&] { udisks.lock(vol(1)); },
                QStringLiteral("locked"), [](const Disk &d) { return d.volumes[1].cleartextPath.isEmpty(); })
        && checkFails(udisks, QStringLiteral("a wrong passphrase is refused"), [&] { udisks.unlock(vol(1), QStringLiteral("wrong")); }, "passphrase")
        && step(udisks, QStringLiteral("unlock it"), [&] { udisks.unlock(vol(1), QStringLiteral("first-passphrase")); },
                QStringLiteral("unlocked, ext4 inside"), [](const Disk &d) { return d.volumes[1].cleartextHasFilesystem && d.volumes[1].cleartextFsType == QLatin1String("ext4"); })
        && step(udisks, QStringLiteral("mount the unlocked side"), [&] { udisks.mount(vol(1)); },
                QStringLiteral("mounted"), [](const Disk &d) { return !d.volumes[1].cleartextMountPoints.isEmpty(); })
        && step(udisks, QStringLiteral("lock it while mounted (unmounts first)"), [&] { udisks.lock(vol(1)); },
                QStringLiteral("locked"), [](const Disk &d) { return d.volumes[1].cleartextPath.isEmpty(); })
        && run(udisks, QStringLiteral("change the passphrase"), [&] { udisks.changePassphrase(vol(1), QStringLiteral("first-passphrase"), QStringLiteral("second-passphrase")); })
        && step(udisks, QStringLiteral("unlock with the new passphrase"), [&] { udisks.unlock(vol(1), QStringLiteral("second-passphrase")); },
                QStringLiteral("unlocked"), [](const Disk &d) { return !d.volumes[1].cleartextPath.isEmpty(); })
        && step(udisks, QStringLiteral("lock it again"), [&] { udisks.lock(vol(1)); },
                QStringLiteral("locked"), [](const Disk &d) { return d.volumes[1].cleartextPath.isEmpty(); });
    if (!ok)
        return false;

    // A second, smaller image, attached the way the app does it.
    const QString second = QFileInfo(imagePath).dir().filePath(QStringLiteral("second.img"));
    {
        QFile f(second);
        if (!f.open(QIODevice::WriteOnly) || !f.resize(64ll * 1024 * 1024))
            return false;
    }
    if (!run(udisks, QStringLiteral("open a disk image (read-write)"), [&] { udisks.openImage(second, false); }))
        return false;
    if (!waitFor(udisks, QStringLiteral("it shows up as a disk"), [&](const Disk &) { return diskWithFile(udisks, second) != nullptr; }))
        return false;

    const QString iso = QFileInfo(imagePath).dir().filePath(QStringLiteral("test.iso"));
    QByteArray isoData(16 * 1024 * 1024, Qt::Uninitialized);
    QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(isoData.data()), isoData.size() / 4);
    {
        QFile f(iso);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(isoData);
    }

    int fd = openFd(udisks, *diskWithFile(udisks, second), true, false);
    report(fd >= 0, QStringLiteral("open the image disk for writing"));
    if (fd < 0)
        return false;
    {
        // Programs DiskForge starts (add-ons, terminals) must never get an open disk.
        const QString device = diskWithFile(udisks, second)->device;
        QProcess child;
        child.start(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("for f in /proc/$$/fd/*; do readlink \"$f\"; done")});
        child.waitForFinished();
        const QString inherited = QString::fromLocal8Bit(child.readAllStandardOutput());
        report((::fcntl(fd, F_GETFD) & FD_CLOEXEC) && !inherited.contains(device), QStringLiteral("a program started meanwhile doesn't get the open disk"),
               inherited.simplified());
    }
    bool written = false;
    QString writeMessage;
    {
        ImageWriter writer(iso, fd, true, checksums::parse(QString::fromLatin1(QCryptographicHash::hash(isoData, QCryptographicHash::Sha256).toHex())));
        QObject::connect(&writer, &ImageWriter::finished, [&](bool k, const QString &m) { written = k; writeMessage = m; });
        writer.run();
    }
    report(written, QStringLiteral("write a 16 MB image and verify it"), writeMessage);
    {
        QFile dev(diskWithFile(udisks, second)->device);
        if (!dev.open(QIODevice::ReadOnly))
            return false;
        report(dev.read(isoData.size()) == isoData, QStringLiteral("the disk really holds the image"));
    }

    fd = openFd(udisks, *diskWithFile(udisks, second), false, true);
    bool benchOk = false;
    BenchmarkResult result;
    if (fd >= 0) {
        Benchmark bench(fd, diskWithFile(udisks, second)->size);
        QObject::connect(&bench, &Benchmark::finished, [&](bool k, const BenchmarkResult &r, const QString &) { benchOk = k; result = r; });
        bench.run();
    }
    report(benchOk && result.readMBps > 0 && result.accessMs >= 0, QStringLiteral("benchmark it"),
           QStringLiteral("%1 MB/s, %2 ms").arg(result.readMBps, 0, 'f', 0).arg(result.accessMs, 0, 'f', 2));

    const bool wiped = run(udisks, QStringLiteral("securely wipe it"), [&] { udisks.wipe(*diskWithFile(udisks, second)); });
    {
        QFile dev(diskWithFile(udisks, second)->device);
        if (!dev.open(QIODevice::ReadOnly))
            return false;
        const QByteArray head = dev.read(isoData.size());
        report(wiped && head == QByteArray(isoData.size(), '\0'), QStringLiteral("it is all zeros now"));
    }

    return run(udisks, QStringLiteral("close the disk image"), [&] { udisks.detachImage(*diskWithFile(udisks, second)); })
        && waitFor(udisks, QStringLiteral("it is gone"), [&](const Disk &) { return diskWithFile(udisks, second) == nullptr; });
}

// Scan and repair against dm-dust, a kernel test target whose bad blocks fail on read
// and are cleared by a write, the way a drive's pending sectors behave.
void badSectorTests()
{
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("dust.img"));
    QByteArray original(64 * 1024 * 1024, Qt::Uninitialized);
    QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(original.data()), original.size() / 4);
    {
        QFile f(image);
        if (!f.open(QIODevice::WriteOnly))
            return report(false, QStringLiteral("create test image"));
        f.write(original);
    }
    const QString loop = sh(QStringLiteral("losetup"), {QStringLiteral("-f"), QStringLiteral("--show"), image});
    report(loop.startsWith(QLatin1String("/dev/loop")), QStringLiteral("attach test image"), loop);
    const QString name = QStringLiteral("diskforge-dust-test");
    const QString dev = QStringLiteral("/dev/mapper/") + name;
    sh(QStringLiteral("modprobe"), {QStringLiteral("dm-dust")});
    sh(QStringLiteral("dmsetup"), {QStringLiteral("create"), name, QStringLiteral("--table"),
                                   QStringLiteral("0 131072 dust %1 0 512").arg(loop)});
    // Sector 1000 starts a 4 KiB block; 50003 sits in the middle of one.
    for (const char *block : {"1000", "50003"})
        sh(QStringLiteral("dmsetup"), {QStringLiteral("message"), name, QStringLiteral("0"), QStringLiteral("addbadblock"), QLatin1String(block)});
    sh(QStringLiteral("dmsetup"), {QStringLiteral("message"), name, QStringLiteral("0"), QStringLiteral("enable")});

    auto scan = [&](QVector<quint64> *bad) {
        const int fd = ::open(dev.toLocal8Bit().constData(), O_RDONLY | O_DIRECT | O_CLOEXEC);
        if (fd < 0)
            return false;
        bool completed = false;
        SurfaceScan s(fd, quint64(original.size()));
        QObject::connect(&s, &SurfaceScan::finished, [&](bool c, const QVector<quint64> &b, int) { completed = c; *bad = b; });
        s.run();
        return completed;
    };

    QVector<quint64> bad;
    const bool scanned = scan(&bad);
    report(scanned && bad == QVector<quint64>{1000 * 512ull, 50003 * 512ull}, QStringLiteral("scan finds exactly the two bad sectors"),
           QStringLiteral("%1 found").arg(bad.size()));

    RepairResult result;
    {
        const int fd = ::open(dev.toLocal8Bit().constData(), O_RDWR | O_DIRECT | O_CLOEXEC);
        SectorRepair repair(fd, bad, 4096); // as on a drive with 4 KiB physical sectors, like the HGST
        QObject::connect(&repair, &SectorRepair::finished, [&](const RepairResult &r) { result = r; });
        repair.run();
    }
    report(result.error.isEmpty() && result.blocks == 2 && result.zeroed == 2 && result.stillBad == 0,
           QStringLiteral("repair rewrites both blocks"),
           QStringLiteral("blocks %1, zeroed %2, still bad %3 %4").arg(result.blocks).arg(result.zeroed).arg(result.stillBad).arg(result.error));

    QVector<quint64> after;
    report(scan(&after) && after.isEmpty(), QStringLiteral("a second scan finds nothing"));

    {
        QFile f(dev);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray now = f.readAll();
            auto sector = [](const QByteArray &d, int s) { return d.mid(s * 512, 512); };
            bool neighboursKept = true;
            for (int s : {1001, 1002, 1007, 50000, 50002, 50004, 50007, 999})
                neighboursKept = neighboursKept && sector(now, s) == sector(original, s);
            report(neighboursKept, QStringLiteral("good sectors next to the bad ones keep their data"));
            report(sector(now, 1000) == QByteArray(512, '\0') && sector(now, 50003) == QByteArray(512, '\0'),
                   QStringLiteral("the unreadable sectors are zeros now"));
            report(now.left(1000 * 512) == original.left(1000 * 512), QStringLiteral("nothing else changed"));
        }
    }
    sh(QStringLiteral("dmsetup"), {QStringLiteral("remove"), name});
    sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
}

// Add-on parsing, matching and placeholder filling. Touches no disks.
void addonTests()
{
    // Keep away from the real add-on folder and settings.
    QTemporaryDir home;
    qputenv("XDG_DATA_HOME", QFile::encodeName(home.filePath(QStringLiteral("data"))));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(home.filePath(QStringLiteral("config"))));

    const QString examples = QStringLiteral(SOURCE_DIR "/examples/addons");
    QMap<QString, Addon> byId;
    for (const QString &name : QDir(examples).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const Addon a = Addons::parse(examples + QLatin1Char('/') + name + QStringLiteral("/addon.json"));
        report(a.error.isEmpty(), QStringLiteral("example add-on %1 parses").arg(name), a.error);
        byId.insert(a.id, a);
    }

    QTemporaryDir tmp;
    auto broken = [&](const QByteArray &json, const char *expect, const QString &what) {
        const QString file = tmp.filePath(QStringLiteral("addon.json"));
        QFile f(file);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(json);
        f.close();
        const Addon a = Addons::parse(file);
        report(a.error.contains(QLatin1String(expect)), what, a.error);
    };
    broken(R"({"id":"x","actions":[{"label":"L","command":["echo","{nope}"]}]})", "Unknown placeholder", QStringLiteral("unknown placeholder is rejected"));
    broken(R"({"id":"Bad Id","actions":[{"label":"L","command":["echo"]}]})", "id", QStringLiteral("bad id is rejected"));
    broken(R"({"id":"x","actions":[{"label":"L","when":["sometimes"],"command":["echo"]}]})", "Unknown condition", QStringLiteral("unknown condition is rejected"));
    broken(R"({"id":"x","actions":[]})", "No actions", QStringLiteral("add-on without actions is rejected"));
    broken("not json", "JSON", QStringLiteral("invalid JSON is rejected"));
    broken(R"({"id":"x","actions":[{"label":"L","command":["{mountpoint}/run.sh"]}]})", "program to run",
           QStringLiteral("a program on the drive can't be the command"));
    broken(R"({"id":"x","actions":[{"label":"L","look_only":true,"command":["pkexec","ls"]}]})", "look_only",
           QStringLiteral("look-only actions can't use admin power"));
    broken(R"({"id":"x","actions":[{"label":"Back Up\tCtrl+Q","command":["echo"]}]})", "hidden",
           QStringLiteral("a label with a tab (a fake shortcut in menus) is refused"));
    broken(R"({"id":"x","actions":[{"label":"A","command":["echo"]},{"label":"A","command":["ls"]}]})", "Two actions",
           QStringLiteral("two actions with the same label are refused"));
    broken(R"({"id":"x","name":"Safe\u202Eexe.sh","actions":[{"label":"A","command":["echo"]}]})", "hidden",
           QStringLiteral("a name with hidden characters is refused"));
    {
        const Addon a = Addons::parseData(R"({"id":"x","actions":[{"label":"L","command":["{home}/bin/tool","{device}"]}]})", QString());
        report(a.error.isEmpty(), QStringLiteral("a program in your home folder is fine"), a.error);
    }

    Disk usb;
    usb.device = QStringLiteral("/dev/sdz");
    usb.removable = true;
    usb.model = QStringLiteral("Test Stick");
    Volume vol;
    vol.device = QStringLiteral("/dev/sdz1");
    vol.label = QStringLiteral("My Stuff; rm -rf ~");
    vol.fsType = QStringLiteral("vfat");
    vol.hasFilesystem = true;
    vol.mountPoints = {QStringLiteral("/run/media/me/My Stuff")};
    usb.volumes = {vol};

    const AddonAction &terminal = byId.value(QStringLiteral("open-terminal")).actions.value(0);
    const AddonAction &backup = byId.value(QStringLiteral("backup-rsync")).actions.value(0);
    const AddonAction &smart = byId.value(QStringLiteral("smart-report")).actions.value(0);
    report(Addons::applies(terminal, usb, &usb.volumes[0], false), QStringLiteral("open-terminal is offered on a mounted volume"));
    Volume unmounted = vol;
    unmounted.mountPoints.clear();
    report(!Addons::applies(terminal, usb, &unmounted, false), QStringLiteral("but not on an unmounted one"));
    report(!Addons::applies(terminal, usb, nullptr, false), QStringLiteral("nor on the disk itself"));
    report(!Addons::applies(smart, usb, nullptr, false), QStringLiteral("smart-report isn't offered without health data"));
    Disk system = usb;
    system.isSystem = true;
    system.health.state = Health::State::Healthy;
    report(!Addons::applies(smart, system, nullptr, false), QStringLiteral("smart-report (sudo) isn't offered on the system disk"));
    report(!Addons::applies(backup, system, &system.volumes[0], false), QStringLiteral("other add-ons aren't offered on the system disk"));
    AddonAction look = terminal;
    look.systemDisks = true;
    report(!Addons::applies(look, system, &system.volumes[0], false), QStringLiteral("system_disks alone isn't enough for the system disk"));
    look.lookOnly = true;
    report(Addons::applies(look, system, &system.volumes[0], false), QStringLiteral("a look-only action with system_disks is offered there"));

    QString error;
    const QStringList argv = Addons::expand(backup.command, usb, &usb.volumes[0], &error);
    const QStringList expected = {QStringLiteral("rsync"), QStringLiteral("-a"), QStringLiteral("--info=progress2"), QStringLiteral("--mkpath"),
                                  QStringLiteral("/run/media/me/My Stuff/"), QDir::homePath() + QStringLiteral("/Backups/My Stuff; rm -rf ~/")};
    report(argv == expected, QStringLiteral("placeholders fill in, and a nasty label stays one plain argument"), argv.join(QStringLiteral(" | ")));
    error.clear();
    report(Addons::expand(backup.command, usb, &unmounted, &error).isEmpty() && error.contains(QLatin1String("Mount")),
           QStringLiteral("{mountpoint} on an unmounted volume asks to mount first"), error);

    // A drive's name can't steer a command: no options, no other folders, nothing hidden.
    auto named = [&](const QString &label) {
        Disk d = usb;
        d.volumes[0].label = label;
        return d;
    };
    Disk sneaky = named(QStringLiteral("../.config/autostart"));
    error.clear();
    QStringList filled = Addons::expand(backup.command, sneaky, &sneaky.volumes[0], &error);
    report(filled.value(5) == QDir::homePath() + QStringLiteral("/Backups/.._.config_autostart/"),
           QStringLiteral("a name with slashes stays one folder inside ~/Backups"), filled.value(5) + error);
    for (const QString &bad : {QStringLiteral(".."), QStringLiteral(".")}) {
        Disk d = named(bad);
        error.clear();
        report(Addons::expand(backup.command, d, &d.volumes[0], &error).isEmpty() && error.contains(QLatin1String("another folder")),
               QStringLiteral("a drive named \"%1\" is refused").arg(bad), error);
    }
    Disk dash = named(QStringLiteral("--delete"));
    error.clear();
    report(Addons::expand({QStringLiteral("tool"), QStringLiteral("{label}")}, dash, &dash.volumes[0], &error).isEmpty()
               && error.contains(QLatin1String("option")),
           QStringLiteral("a name starting with - can't become an option"), error);
    error.clear();
    filled = Addons::expand({QStringLiteral("tool"), QStringLiteral("--name={label}")}, dash, &dash.volumes[0], &error);
    report(filled.value(1) == QLatin1String("--name=--delete"), QStringLiteral("but it's fine after the add-on's own option"), filled.join(QLatin1Char(' ')) + error);
    error.clear();
    report(Addons::expandText(QStringLiteral("{label} will be copied"), dash, &dash.volumes[0], &error) == QLatin1String("--delete will be copied"),
           QStringLiteral("and fine in the confirm question"), error);
    for (const QString &hidden : {QStringLiteral("SATA\u202E005"), QStringLiteral("two\nlines"), QStringLiteral("zero\u200Bwidth")}) {
        Disk d = named(hidden);
        error.clear();
        report(Addons::expand(backup.command, d, &d.volumes[0], &error).isEmpty() && error.contains(QLatin1String("hidden")),
               QStringLiteral("a name with hidden characters is refused (%1)").arg(QString(hidden).replace(QLatin1Char('\n'), QLatin1Char(' '))), error);
    }
    Disk mountedOdd = usb;
    mountedOdd.volumes[0].mountPoints = {QStringLiteral("/run/media/me/a\nb")};
    error.clear();
    report(Addons::expand(backup.command, mountedOdd, &mountedOdd.volumes[0], &error).isEmpty(),
           QStringLiteral("a mount folder with a line break is refused"), error);
    report(cleanName(QStringLiteral("SATA\u202E005\u200B")) == QLatin1String("SATA005") && cleanName(QStringLiteral("a\tb")) == QLatin1String("a b")
               && cleanName(QStringLiteral("Täst 💾")) == QStringLiteral("Täst 💾"),
           QStringLiteral("cleanName drops invisible characters and keeps the rest"));

    // What each command could do.
    auto risksOf = [](const QStringList &command, bool lookOnly = false) {
        AddonAction act;
        act.command = command;
        act.lookOnly = lookOnly;
        return Addons::risks(act);
    };
    report(risksOf({QStringLiteral("pkexec"), QStringLiteral("fsck"), QStringLiteral("{device}")}).admin == QLatin1String("pkexec"),
           QStringLiteral("pkexec counts as admin power"));
    report(risksOf({QStringLiteral("konsole"), QStringLiteral("-e"), QStringLiteral("/usr/bin/sudo"), QStringLiteral("x")}).admin == QLatin1String("sudo"),
           QStringLiteral("so does sudo further along the command"));
    report(risksOf({QStringLiteral("bash"), QStringLiteral("-c"), QStringLiteral("x")}).anything == QLatin1String("bash")
               && risksOf({QStringLiteral("/usr/bin/python3.13"), QStringLiteral("x.py")}).anything == QLatin1String("python3.13")
               && !risksOf({QStringLiteral("env"), QStringLiteral("x")}).anything.isEmpty(),
           QStringLiteral("shells and interpreters can do anything"));
    report(risksOf({QStringLiteral("curl"), QStringLiteral("x")}).network == QLatin1String("curl") && risksOf({QStringLiteral("rm"), QStringLiteral("x")}).deletes == QLatin1String("rm")
               && risksOf({QStringLiteral("mkfs.ext4"), QStringLiteral("x")}).deletes == QLatin1String("mkfs.ext4")
               && !risksOf({QStringLiteral("find"), QStringLiteral("."), QStringLiteral("-delete")}).deletes.isEmpty(),
           QStringLiteral("network and deleting programs are noticed"));
    report(!risksOf({QStringLiteral("du"), QStringLiteral("-h")}).any() && !risksOf({QStringLiteral("bash"), QStringLiteral("-c"), QStringLiteral("x")}, true).any(),
           QStringLiteral("plain tools and look-only actions have no warnings"));
    report(risksOf({QStringLiteral("sudo"), QStringLiteral("x")}).alwaysAsk() && risksOf({QStringLiteral("sh"), QStringLiteral("x")}).alwaysAsk()
               && !risksOf({QStringLiteral("rm"), QStringLiteral("x")}).alwaysAsk(),
           QStringLiteral("admin power and shells always ask"));

    // Installing notes the file; one that turns up or changes some other way is flagged,
    // and trust is tied to the exact file.
    const QByteArray mine = R"({"id":"mine","name":"Mine","actions":[{"label":"Sizes","command":["du","-sh","{mountpoint}"]}]})";
    error.clear();
    report(Addons::install(mine, &error), QStringLiteral("an add-on installs"), error);
    Addons addons;
    addons.load();
    auto find = [&addons](const QString &id) {
        for (const Addon &a : addons.all()) {
            if (a.id == id)
                return a;
        }
        return Addon();
    };
    report(find(QStringLiteral("mine")).id == QLatin1String("mine") && !find(QStringLiteral("mine")).outside,
           QStringLiteral("one installed through DiskForge isn't flagged"));
    Addon a = find(QStringLiteral("mine"));
    report(!Addons::isTrusted(a, a.actions[0]), QStringLiteral("a new action isn't trusted yet"));
    Addons::trust(a, a.actions[0]);
    report(Addons::isTrusted(a, a.actions[0]), QStringLiteral("after 'Don't ask again' it is"));

    const QString planted = Addons::userDir() + QStringLiteral("/planted/addon.json");
    QDir().mkpath(QFileInfo(planted).path());
    auto write = [](const QString &file, const QByteArray &data) {
        QFile f(file);
        return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
    };
    write(planted, R"({"id":"planted","name":"Planted","actions":[{"label":"Hi","command":["echo","hi"]}]})");
    const QString mineFile = Addons::userDir() + QStringLiteral("/mine/addon.json");
    write(mineFile, QByteArray(mine).replace("\"Mine\"", "\"Mine 2\""));
    addons.load();
    report(find(QStringLiteral("planted")).outside, QStringLiteral("an add-on copied in by hand is flagged as added from outside"));
    a = find(QStringLiteral("mine"));
    report(a.outside && !Addons::isTrusted(a, a.actions[0]), QStringLiteral("a changed add-on is flagged and asks again"));
    const QByteArray seen = find(QStringLiteral("planted")).fileHash;
    write(planted, R"({"id":"planted","name":"Planted","actions":[{"label":"Hi","command":["echo","swapped"]}]})");
    addons.load();
    addons.accept(QStringLiteral("planted"), seen);
    report(find(QStringLiteral("planted")).outside, QStringLiteral("'I Added It' doesn't accept a file that changed after it was shown"));
    addons.accept(QStringLiteral("planted"), find(QStringLiteral("planted")).fileHash);
    addons.load();
    report(!find(QStringLiteral("planted")).outside, QStringLiteral("'I Added It' clears the flag"));
    AddonAction shell = a.actions[0];
    shell.command = {QStringLiteral("sh"), QStringLiteral("-c"), QStringLiteral("true")};
    Addons::trust(a, shell);
    a.outside = false;
    report(!Addons::isTrusted(a, shell), QStringLiteral("a shell action is never trusted for good"));

    // Forms, settings, output and themes.
    {
        const QByteArray full = R"({"id":"full","name":"Full","settings":[{"id":"dest","type":"folder","label":"Back up to","default":"{home}/Backups"}],
            "theme":{"colors":{"partition":"#00b4ff","danger":"#ff2d55","nonsense":"#000000"},"filesystems":{"btrfs":"#8844ff"},"usage":["#112233"]},
            "actions":[{"label":"Copy","output":"window","command":["rsync","-a","{ask:check}","{ask:mode}","--name={ask:name}","{mountpoint}/","{setting:dest}/{label}/"],
              "ask":[{"id":"check","type":"check","label":"Compare contents","on":"--checksum","off":""},
                     {"id":"mode","type":"choice","label":"How","choices":[{"label":"Quiet","value":"-q"},{"label":"Chatty","value":"-v"}],"default":"-v"},
                     {"id":"name","type":"text","label":"Name","default":"{label}"},
                     {"id":"count","type":"number","label":"How many","min":1,"max":5}]}]})";
        const Addon a = Addons::parseData(full, QString());
        report(a.error.isEmpty() && a.settings.size() == 1 && a.actions.value(0).ask.size() == 4 && a.actions.value(0).window
                   && a.theme.colors.value(QStringLiteral("partition")) == 0x00b4ff && !a.theme.colors.contains(QStringLiteral("nonsense"))
                   && a.theme.filesystems.value(QStringLiteral("btrfs")) == 0x8844ff && a.theme.usage.size() == 1,
               QStringLiteral("forms, settings, window output and a theme parse"), a.error);
        const Addon themeOnly = Addons::parseData(R"({"id":"t","theme":{"colors":{"free":"#101010"}}})", QString());
        report(themeOnly.error.isEmpty() && themeOnly.actions.isEmpty(), QStringLiteral("an add-on can be just a theme"), themeOnly.error);

        const AddonAction &act = a.actions.value(0);
        QMap<QString, QString> answers = {{QStringLiteral("check"), QStringLiteral("--checksum")}, {QStringLiteral("mode"), QStringLiteral("-q")},
                                          {QStringLiteral("name"), QStringLiteral("stick")}};
        error.clear();
        QStringList filled = Addons::fillCommand(a, act, usb, &usb.volumes[0], answers, &error);
        const QStringList expected = {QStringLiteral("rsync"), QStringLiteral("-a"), QStringLiteral("--checksum"), QStringLiteral("-q"),
                                      QStringLiteral("--name=stick"), QStringLiteral("/run/media/me/My Stuff/"),
                                      QDir::homePath() + QStringLiteral("/Backups/My Stuff; rm -rf ~/")};
        report(filled == expected, QStringLiteral("answers and settings fill in"), filled.join(QStringLiteral(" | ")) + error);
        answers[QStringLiteral("check")] = QString();
        filled = Addons::fillCommand(a, act, usb, &usb.volumes[0], answers, &error);
        report(!filled.contains(QString()) && filled.size() == expected.size() - 1, QStringLiteral("an unticked box leaves its argument out"),
               filled.join(QStringLiteral(" | ")));
        answers[QStringLiteral("check")] = QStringLiteral("--checksum");
        answers[QStringLiteral("name")] = QStringLiteral("-rf");
        filled = Addons::fillCommand(a, act, usb, &usb.volumes[0], answers, &error);
        report(filled.value(4) == QLatin1String("--name=-rf"), QStringLiteral("a typed \"-\" is fine after the add-on's own option"), filled.value(4));
        Addon bare = a;
        bare.actions[0].command = {QStringLiteral("tool"), QStringLiteral("{ask:name}")};
        error.clear();
        report(Addons::fillCommand(bare, bare.actions[0], usb, &usb.volumes[0], answers, &error).isEmpty() && error.contains(QLatin1String("option")),
               QStringLiteral("but a typed answer can't start an argument with \"-\""), error);
        answers[QStringLiteral("name")] = QStringLiteral("stick");
        answers[QStringLiteral("mode")] = QStringLiteral("--delete");
        error.clear();
        report(Addons::fillCommand(a, act, usb, &usb.volumes[0], answers, &error).isEmpty() && error.contains(QLatin1String("choices")),
               QStringLiteral("a choice that isn't in the add-on's list is refused"), error);
        answers[QStringLiteral("mode")] = QStringLiteral("-v");
        answers[QStringLiteral("name")] = QStringLiteral("a\u202Eb");
        error.clear();
        report(Addons::fillCommand(a, act, usb, &usb.volumes[0], answers, &error).isEmpty() && error.contains(QLatin1String("hidden")),
               QStringLiteral("a typed answer with hidden characters is refused"), error);
        answers.remove(QStringLiteral("name"));
        Disk braces = named(QStringLiteral("{ask:mode}"));
        error.clear();
        filled = Addons::fillCommand(a, act, braces, &braces.volumes[0], answers, &error);
        report(filled.value(4) == QLatin1String("--name={ask:mode}") && filled.value(6).endsWith(QLatin1String("/{ask:mode}/")),
               QStringLiteral("a drive named \"{ask:mode}\" stays just that (one pass)"), filled.join(QStringLiteral(" | ")) + error);
        Addons::setSetting(a, QStringLiteral("dest"), QStringLiteral("/srv/backups"));
        filled = Addons::fillCommand(a, act, usb, &usb.volumes[0], answers, &error);
        report(filled.value(6).startsWith(QLatin1String("/srv/backups/")), QStringLiteral("a saved setting is used"), filled.value(6));
        report(Addons::fieldDefault(act.ask[2], usb, &usb.volumes[0]) == QLatin1String("My Stuff; rm -rf ~")
                   && Addons::fieldDefault(act.ask[1], usb, nullptr) == QLatin1String("-v") && Addons::fieldDefault(act.ask[3], usb, nullptr) == QLatin1String("1"),
               QStringLiteral("form defaults fill in"));

        // Values in a form count toward what an action can do.
        const Addon sneakyChoice = Addons::parseData(R"({"id":"s","actions":[{"label":"L","command":["{home}/bin/tool","{ask:how}"],
            "ask":[{"id":"how","type":"choice","choices":["fast","sudo"]}]}]})", QString());
        report(sneakyChoice.error.isEmpty() && Addons::risks(sneakyChoice.actions[0]).admin == QLatin1String("sudo"),
               QStringLiteral("a choice of \"sudo\" counts as admin power"), sneakyChoice.error);
        Addon trusted = a;
        trusted.outside = false;
        Addons::trust(trusted, trusted.actions[0]);
        report(Addons::isTrusted(trusted, trusted.actions[0], {QStringLiteral("rsync")})
                   && !Addons::isTrusted(trusted, trusted.actions[0], {QStringLiteral("rsync"), QStringLiteral("bash")}),
               QStringLiteral("\"Don't ask again\" doesn't cover a command that turns out to run a shell"));
        report(Addons::actionKey(a, act) == QLatin1String("full/Copy")
                   && Addons::actionKey(a, AddonAction{QStringLiteral("a/b & c")}) == QLatin1String("full/a%2Fb%20%26%20c"),
               QStringLiteral("action keys are stable and safe"), Addons::actionKey(a, AddonAction{QStringLiteral("a/b & c")}));
    }
    broken(R"({"id":"x","actions":[{"label":"L","command":["echo","{ask:nope}"]}]})", "Unknown placeholder", QStringLiteral("a form field that doesn't exist is refused"));
    broken(R"({"id":"x","actions":[{"label":"L","command":["echo","{setting:x}"]}]})", "Unknown placeholder", QStringLiteral("a setting that doesn't exist is refused"));
    broken(R"({"id":"x","actions":[{"label":"L","command":["echo","{device:x}"]}]})", "Unknown placeholder", QStringLiteral("{device:x} is refused"));
    broken(R"({"id":"x","actions":[{"label":"L","command":["{ask:p}"],"ask":[{"id":"p"}]}]})", "program to run",
           QStringLiteral("the program to run can't come from a form"));
    broken(R"({"id":"x","actions":[{"label":"L","command":["echo"],"ask":[{"id":"n","type":"number","min":-5}]}]})", "min",
           QStringLiteral("a number field that allows negatives is refused"));
    broken(R"({"id":"x","actions":[{"label":"L","command":["echo"],"ask":[{"id":"c","type":"choice"}]}]})", "choices",
           QStringLiteral("a choice field without choices is refused"));
    broken(R"({"id":"x","actions":[{"label":"L","command":["echo"],"ask":[{"id":"a"},{"id":"a"}]}]})", "own",
           QStringLiteral("two fields with the same id are refused"));
    broken(R"({"id":"x","actions":[{"label":"L","command":["echo"],"ask":[{"id":"a","type":"slider"}]}]})", "unknown type",
           QStringLiteral("an unknown field type is refused"));
    broken(R"({"id":"x","settings":[{"id":"d","default":"{label}"}],"actions":[{"label":"L","command":["echo"]}]})", "default",
           QStringLiteral("a setting's default can't depend on a drive"));
    broken(R"({"id":"x","actions":[{"label":"L","output":"sideways","command":["echo"]}]})", "output", QStringLiteral("an unknown output is refused"));
    broken(R"({"id":"x","actions":[{"label":"L","output":"window","command":["sudo","ls"]}]})", "terminal",
           QStringLiteral("sudo in a DiskForge window is refused (it needs a terminal)"));
    broken(R"({"id":"x","theme":{"colors":{"danger":"red"}}})", "color", QStringLiteral("a theme color that isn't #rrggbb is refused"));
    broken(R"({"id":"x","theme":{"colors":{"danger":"#ff000080"}}})", "color", QStringLiteral("a see-through theme color is refused"));
    broken(R"({"id":"x"})", "No actions", QStringLiteral("an add-on with no actions and no theme is refused"));
    broken(R"({"id":"x","actions":[{"label":"L","command":["ls","safe\u202Egnp.exe"]}]})", "hidden",
           QStringLiteral("a command with a text-direction flip in it is refused"));

    // Why an action isn't offered.
    {
        AddonAction mountedOnly;
        mountedOnly.when = {QStringLiteral("mounted"), QStringLiteral("filesystem:ext4|btrfs")};
        report(Addons::whyNot(mountedOnly, usb, &unmounted, false) == QLatin1String("Mount it first")
                   && Addons::whyNot(mountedOnly, usb, &usb.volumes[0], false) == QLatin1String("Only for ext4, btrfs")
                   && Addons::whyNot(mountedOnly, usb, nullptr, false) == QLatin1String("Pick a partition first")
                   && Addons::whyNot(mountedOnly, system, &system.volumes[0], false).contains(QLatin1String("system disk")),
               QStringLiteral("the reasons an action isn't offered are plain"), Addons::whyNot(mountedOnly, usb, &usb.volumes[0], false));
    }

    // What the output window makes of a command's raw output.
    {
        OutputFilter f;
        QStringList lines = f.feed("caf\xc3");
        lines += f.feed("\xa9 \x1b[31mred\x1b[0m\x1b]0;window title\x07!\n");
        lines += f.feed("10%\r20%\r30%\nab\b\nx\xe2\x80\xaey\xe2\x80\x8bz\n\x1b[3");
        lines += f.feed("1mtail\n");
        lines += f.feed(QByteArray(3000, 'x') + "\n4.0K\tnotes\n50%\r");
        lines += f.feed("60%");
        const QStringList expected = {QStringLiteral("café red!"), QStringLiteral("30%"), QStringLiteral("a"), QStringLiteral("xyz"),
                                      QStringLiteral("tail"), QString(OutputFilter::kMaxLine, QLatin1Char('x')) + QChar(0x2026),
                                      QStringLiteral("4.0K    notes")};
        report(lines == expected && f.current() == QLatin1String("60%"),
               QStringLiteral("output shows as plain text: split characters, colors, progress bars, hidden characters, long lines"),
               lines.mid(0, 5).join(QStringLiteral(" | ")) + QStringLiteral(" | current: ") + f.current());
    }

    // The look-only sandbox: really read-only, and other programs' sockets aren't there.
    AddonAction looking;
    looking.lookOnly = true;
    QStringList wrapped = Addons::commandLine(looking, {QStringLiteral("du"), QStringLiteral("-sh")}, &error);
    report(wrapped.value(0) == QLatin1String("bwrap") && wrapped.endsWith(QLatin1String("-sh")), QStringLiteral("a look-only action runs in the sandbox"),
           wrapped.join(QLatin1Char(' ')));
    looking.terminal = true;
    wrapped = Addons::commandLine(looking, {QStringLiteral("du"), QStringLiteral("-sh")}, &error);
    if (!wrapped.isEmpty())
        report(wrapped.indexOf(QLatin1String("bwrap")) > 0, QStringLiteral("inside the terminal, not around it"), wrapped.join(QLatin1Char(' ')));
    if (QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty()) {
        out << "SKIP  bwrap isn't installed, so the sandbox itself isn't tested" << Qt::endl;
        return;
    }
    auto inSandbox = [](const QString &script) {
        QStringList command = Addons::sandboxed({QStringLiteral("sh"), QStringLiteral("-c"), script});
        return QProcess::execute(command.takeFirst(), command);
    };
    // Not under /tmp: the sandbox gets an empty one of its own.
    QDir().mkpath(QDir::homePath() + QStringLiteral("/.cache"));
    QTemporaryDir scratchDir(QDir::homePath() + QStringLiteral("/.cache/diskforge-sandbox-test-XXXXXX"));
    const QString scratch = scratchDir.path();
    write(scratch + QStringLiteral("/readme"), "hello");
    report(inSandbox(QStringLiteral("grep -q hello '%1/readme'").arg(scratch)) == 0, QStringLiteral("the sandbox can read your files"));
    report(inSandbox(QStringLiteral("echo x 2>/dev/null > '%1/new'").arg(scratch)) != 0 && !QFile::exists(scratch + QStringLiteral("/new")),
           QStringLiteral("but can't write them"));
    report(inSandbox(QStringLiteral("rm '%1/readme' 2>/dev/null").arg(scratch)) != 0 && QFile::exists(scratch + QStringLiteral("/readme")),
           QStringLiteral("or delete them"));
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!runtime.isEmpty() && QFile::exists(runtime + QStringLiteral("/bus")))
        report(inSandbox(QStringLiteral("test -e '%1/bus'").arg(runtime)) != 0, QStringLiteral("the session bus isn't reachable from it"));
    report(inSandbox(QStringLiteral("test -z \"$(ls -A /tmp)\" && ! ls /dev/sd* /dev/nvme* 2>/dev/null")) == 0,
           QStringLiteral("it gets an empty /tmp and no disks in /dev"));
    report(inSandbox(QStringLiteral("test $(grep -c : /proc/net/dev) -gt 1")) != 0, QStringLiteral("it has no network but loopback"));
    // A drive mounted under /run/media, if one is: readable in there, but not writable.
    const QString media = QStringLiteral("/run/media/") + qEnvironmentVariable("USER");
    for (const QFileInfo &mount : QDir(media).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!mount.isWritable())
            continue;
        const QString probe = mount.filePath() + QStringLiteral("/.diskforge-sandbox-test");
        report(inSandbox(QStringLiteral("ls '%1' >/dev/null && ! touch '%2' 2>/dev/null").arg(mount.filePath(), probe)) == 0 && !QFile::exists(probe),
               QStringLiteral("a mounted drive (%1) is readable in it, but not writable").arg(mount.fileName()));
        QFile::remove(probe);
        break;
    }
}

void guard(UDisks &udisks)
{
    for (const Disk &d : udisks.disks()) {
        if (!d.isSystem || d.volumes.isEmpty())
            continue;
        const Volume &v = d.volumes.last();
        const QString name = shortDevice(v.device);
        QString message;
        bool ok = run(udisks, {}, [&] { udisks.format(v, QStringLiteral("ext4"), {}); }, &message);
        report(!ok && message.contains(QLatin1String("running system")), QStringLiteral("format %1 is refused").arg(name), message);
        ok = run(udisks, {}, [&] { udisks.deletePartition(v); }, &message);
        report(!ok && message.contains(QLatin1String("running system")), QStringLiteral("delete %1 is refused").arg(name), message);
        ok = run(udisks, {}, [&] { udisks.createPartitionTable(d, QStringLiteral("gpt")); }, &message);
        report(!ok && message.contains(QLatin1String("running system")), QStringLiteral("new partition table on %1 is refused").arg(shortDevice(d.device)), message);
        ok = run(udisks, {}, [&] { udisks.wipe(d); }, &message);
        report(!ok && message.contains(QLatin1String("running system")), QStringLiteral("wiping %1 is refused").arg(shortDevice(d.device)), message);
        for (auto method : {UDisks::EraseMethod::NvmeUserData, UDisks::EraseMethod::AtaNormal}) {
            ok = run(udisks, {}, [&] { udisks.secureErase(d, method); }, &message);
            report(!ok && message.contains(QLatin1String("running system")), QStringLiteral("secure erase of %1 is refused").arg(shortDevice(d.device)), message);
        }
        ok = run(udisks, {}, [&] { udisks.powerOff(d); }, &message);
        report(!ok && message.contains(QLatin1String("running system")), QStringLiteral("powering off %1 is refused").arg(shortDevice(d.device)), message);
        ok = run(udisks, {}, [&] { udisks.setMountAtStartup(v, true); }, &message);
        report(!ok && message.contains(QLatin1String("running system")), QStringLiteral("startup mount on %1 is refused").arg(name), message);
        ok = run(udisks, {}, [&] { udisks.setPartitionTypeAndFlags(v, QStringLiteral("0fc63daf-8483-4772-8e79-3d69d8477de4"), true, 0); }, &message);
        report(!ok && message.contains(QLatin1String("running system")), QStringLiteral("changing the type and flags of %1 is refused").arg(name), message);
        report(!RecoverDialog::refusal(d).isEmpty(), QStringLiteral("Recover Partitions won't write %1's table").arg(shortDevice(d.device)), RecoverDialog::refusal(d));
        const int fd = openFd(udisks, d, true, false);
        report(fd < 0, QStringLiteral("raw write access to %1 is refused").arg(shortDevice(d.device)));
        // Reading (a backup) would unmount it first, so that's refused as well.
        // refresh() rebuilds the disk list, so keep copies, not references into it.
        QString path, mountedName;
        for (const Volume &mounted : d.volumes) {
            if (!mounted.mounts().isEmpty()) {
                path = mounted.objectPath;
                mountedName = shortDevice(mounted.device);
                break;
            }
        }
        if (!path.isEmpty()) {
            const int readFd = openBlockFd(udisks, path, int(UDisks::OpenMode::Read));
            udisks.refresh();
            bool stillMounted = false;
            for (const Disk &now : udisks.disks()) {
                for (const Volume &v : now.volumes)
                    stillMounted = stillMounted || (v.objectPath == path && !v.mounts().isEmpty());
            }
            report(readFd < 0 && stillMounted, QStringLiteral("backing up mounted %1 is refused and it stays mounted").arg(mountedName));
        }
        return;
    }
    report(false, QStringLiteral("find the system disk"));
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const bool root = geteuid() == 0;

    const QStringList args = app.arguments();
    auto needsRoot = [&](const char *suite) {
        if (!root)
            out << suite << " needs root (it sets up test devices)" << Qt::endl;
        return !root;
    };
    if (args.contains(QStringLiteral("--addons"))) {
        addonTests();
    } else if (args.contains(QStringLiteral("--gpt"))) {
        gptTests();
    } else if (args.contains(QStringLiteral("--copy"))) {
        copyTests();
    } else if (args.contains(QStringLiteral("--backup"))) {
        backupTests();
    } else if (args.contains(QStringLiteral("--usage"))) {
        usageTests();
    } else if (args.contains(QStringLiteral("--catalog"))) {
        catalogTests();
    } else if (args.contains(QStringLiteral("--fuzz"))) {
        fuzzTests();
    } else if (args.contains(QStringLiteral("--health"))) {
        healthTests();
    } else if (args.contains(QStringLiteral("--jobs"))) {
        jobsTests();
    } else if (args.contains(QStringLiteral("--recover"))) {
        recoverTests();
    } else if (args.contains(QStringLiteral("--rescueusb"))) {
        rescueUsbTests();
    } else if (args.contains(QStringLiteral("--stickcheck"))) {
        stickCheckTests();
    } else if (args.contains(QStringLiteral("--images"))) {
        imageTests();
    } else if (args.contains(QStringLiteral("--isomode"))) {
        isoModeTests();
    } else if (args.contains(QStringLiteral("--windows"))) {
        windowsTests();
    } else if (args.contains(QStringLiteral("--lostfiles"))) {
        lostFilesTests();
    } else if (args.contains(QStringLiteral("--viruses"))) {
        virusTests();
    } else if (args.contains(QStringLiteral("--registry"))) {
        registryTests();
    } else if (args.contains(QStringLiteral("--startup"))) {
        startupTests();
    } else if (args.contains(QStringLiteral("--boot"))) {
        bootTests();
    } else if (args.contains(QStringLiteral("--newids"))) {
        newIdsTests();
    } else if (args.contains(QStringLiteral("--clonezilla"))) {
        clonezillaTests();
        evidenceTests();
    } else if (args.contains(QStringLiteral("--savefiles"))) {
        saveFilesTests();
    } else if (args.contains(QStringLiteral("--luks"))) {
        luksTests();
    } else if (args.contains(QStringLiteral("--fat32"))) {
        fat32Tests();
        sdcardTests();
    } else if (args.contains(QStringLiteral("--shadow-image"))) {
        // The same for shadow copies: each one, its files and its volume a MiB at a time.
        return shadowImage(args.value(args.indexOf(QStringLiteral("--shadow-image")) + 1));
    } else if (args.contains(QStringLiteral("--scan-image"))) {
        // For looking into a problem by hand: what Find Lost Files' quick scan lists in an image.
        return scanImage(args.value(args.indexOf(QStringLiteral("--scan-image")) + 1));
    } else if (args.contains(QStringLiteral("--raid"))) {
        if (needsRoot("--raid"))
            return 2;
        raidTests();
    } else if (args.contains(QStringLiteral("--lvm"))) {
        if (needsRoot("--lvm"))
            return 2;
        lvmTests();
    } else if (args.contains(QStringLiteral("--stop"))) {
        if (needsRoot("--stop"))
            return 2;
        stopTests();
    } else if (args.contains(QStringLiteral("--cleanup"))) {
        cleanupTests();
        snapperTests();
    } else if (args.contains(QStringLiteral("--btrfs"))) {
        if (needsRoot("--btrfs"))
            return 2;
        btrfsTests();
    } else if (args.contains(QStringLiteral("--optimize"))) {
        if (needsRoot("--optimize"))
            return 2;
        optimizeTests();
    } else if (args.contains(QStringLiteral("--clone"))) {
        if (needsRoot("--clone"))
            return 2;
        cloneTests();
    } else if (args.contains(QStringLiteral("--rescuemap"))) {
        rescueMapTests();
    } else if (args.contains(QStringLiteral("--rescue"))) {
        if (needsRoot("--rescue"))
            return 2;
        rescueTests();
    } else if (args.contains(QStringLiteral("--blockmap"))) {
        if (needsRoot("--blockmap"))
            return 2;
        blockMapTests();
    } else if (app.arguments().contains(QStringLiteral("--badsectors"))) {
        if (!root) {
            out << "--badsectors needs root (it creates a test device with dmsetup)" << Qt::endl;
            return 2;
        }
        badSectorTests();
    } else if (app.arguments().contains(QStringLiteral("--guard"))) {
        // root bypasses polkit, so test the guard as a normal user
        if (root) {
            out << "Run --guard as your normal user, not root." << Qt::endl;
            return 2;
        }
        UDisks udisks;
        udisks.setInteractive(false);
        guard(udisks);
    } else {
        if (!root) {
            out << "Disk changes need a password, so the unattended run uses root:\n"
                   "  sudo ./build/diskforge-selftest      operations on a throwaway 512 MB image\n"
                   "  ./build/diskforge-selftest --guard   system disk is refused (as you)" << Qt::endl;
            return 2;
        }
        QTemporaryDir dir;
        imagePath = dir.filePath(QStringLiteral("selftest.img"));
        QFile image(imagePath);
        if (!image.open(QIODevice::WriteOnly) || !image.resize(512ll * 1024 * 1024)) {
            out << "Couldn't create " << imagePath << Qt::endl;
            return 2;
        }
        image.close();

        QString error;
        const QString loop = loopSetup(imagePath, &error);
        report(!loop.isEmpty(), QStringLiteral("attach image as a loop device"), loop.isEmpty() ? error : loop);
        if (loop.isEmpty())
            return 1;
        UDisks udisks;
        udisks.setInteractive(false);
        operations(udisks);
        if (const Disk *d = testDisk(udisks)) {
            for (const Volume &v : d->volumes) {
                if (!v.mountPoints.isEmpty()) {
                    QString ignored;
                    run(udisks, {}, [&] { udisks.unmount(v); }, &ignored);
                }
            }
        }
        for (const Disk &d : udisks.disks()) {
            if (d.isLoop && d.backingFile.startsWith(dir.path()) && d.blockPath != loop)
                loopDelete(d.blockPath);
        }
        loopDelete(loop);
    }

    out << (failures ? QStringLiteral("%1 step(s) failed").arg(failures) : QStringLiteral("All steps passed")) << Qt::endl;
    return failures ? 1 : 0;
}
