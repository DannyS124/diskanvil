// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Clone Drive on loop devices through UDisks: GPT and MBR sources, a bigger target,
// growing the last partition, and "keep both" with new IDs. Needs root.

#include "testkit.h"

#include "../src/backup/clone.h"
#include "../src/drives/udisks.h"

#include <QFile>
#include <QTemporaryDir>

namespace {

constexpr qint64 MiB = 1024 * 1024;

bool makeImage(const QString &path, qint64 size, char fill)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    if (fill == '\0')
        return f.resize(size);
    const QByteArray chunk(4 * MiB, fill);
    for (qint64 done = 0; done < size; done += chunk.size())
        f.write(chunk);
    return true;
}

bool partition(const QString &path, const QString &script)
{
    int code = -1;
    sh(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("printf '%1' | sfdisk -q '%2'").arg(script, path)}, &code);
    return code == 0;
}

bool allBytes(const QString &device, qint64 from, qint64 to, char value)
{
    QFile f(device);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(from))
        return false;
    while (from < to) {
        const QByteArray chunk = f.read(std::min<qint64>(to - from, 4 * MiB));
        if (chunk.isEmpty() || chunk.count(value) != chunk.size())
            return false;
        from += chunk.size();
    }
    return true;
}

QString uuidOf(const Disk &d, int number)
{
    for (const Volume &v : d.volumes) {
        if (v.number == number)
            return v.uuid;
    }
    return {};
}

const Volume *volumeOf(const Disk &d, int number)
{
    for (const Volume &v : d.volumes) {
        if (v.number == number)
            return &v;
    }
    return nullptr;
}

bool cloneTo(UDisks &udisks, const Disk &source, const Disk &target, bool newIds, QString *message)
{
    const diskclone::Plan plan = diskclone::plan(source, target);
    if (!plan.error.isEmpty()) {
        *message = plan.error;
        return false;
    }
    const int in = openBlockFd(udisks, source.blockPath, int(UDisks::OpenMode::Read));
    const int out = openBlockFd(udisks, target.blockPath, int(UDisks::OpenMode::ReadWrite));
    if (in < 0 || out < 0) {
        *message = QStringLiteral("couldn't open the drives");
        return false;
    }
    bool ok = false;
    CloneJob job(in, out, plan, newIds, true);
    QObject::connect(&job, &CloneJob::finished, [&](bool success, const QString &m) {
        ok = success;
        *message = m;
    });
    job.run();
    return ok;
}

QString dumpWithoutDevice(const QString &device)
{
    QStringList lines;
    for (const QString &line : sh(QStringLiteral("sfdisk"), {QStringLiteral("--dump"), device}).split(QLatin1Char('\n'))) {
        if (!line.startsWith(QLatin1String("device:")) && !line.startsWith(QLatin1String("last-lba:")))
            lines << line.section(QLatin1Char(':'), 1); // drop the /dev/loopNpM part
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace

void cloneTests()
{
    QTemporaryDir dir;
    UDisks udisks;
    udisks.setInteractive(false);
    QStringList loops;
    auto attach = [&](const QString &file) {
        QString error;
        const QString path = loopSetup(file, &error);
        if (!path.isEmpty())
            loops << path;
        return path;
    };

    // GPT source: FAT, Btrfs, ext4, then 250 MiB of free space.
    const QString srcFile = dir.filePath(QStringLiteral("source.img"));
    makeImage(srcFile, 512 * MiB, '\0');
    partition(srcFile, QStringLiteral("label: gpt\\nsize=32MiB, type=uefi\\nsize=160MiB, type=linux\\nsize=64MiB, type=linux\\n"));
    attach(srcFile);
    const Disk *src = waitForDisk(udisks, srcFile, [](const Disk &d) { return d.volumes.size() == 3; });
    report(src, QStringLiteral("attach a GPT test drive"));
    if (!src)
        return;
    sh(QStringLiteral("mkfs.vfat"), {QStringLiteral("-n"), QStringLiteral("SRCFAT"), volumeOf(*src, 1)->device});
    sh(QStringLiteral("mkfs.btrfs"), {QStringLiteral("-q"), QStringLiteral("-f"), QStringLiteral("-L"), QStringLiteral("srcbtrfs"), volumeOf(*src, 2)->device});
    sh(QStringLiteral("mkfs.ext4"), {QStringLiteral("-q"), QStringLiteral("-F"), QStringLiteral("-L"), QStringLiteral("srcext"), volumeOf(*src, 3)->device});
    src = waitForDisk(udisks, srcFile, [](const Disk &d) {
        return d.volumes.size() == 3 && !d.volumes[0].uuid.isEmpty() && !d.volumes[1].uuid.isEmpty() && !d.volumes[2].uuid.isEmpty();
    });
    report(src, QStringLiteral("make file systems on it"));
    if (!src)
        return;
    const Disk source = *src;

    // Refusals, before anything is opened.
    Disk system = source;
    system.isSystem = true;
    Disk elsewhere = source;
    elsewhere.blockPath += QStringLiteral("y"); // any other drive
    Disk otherSectors = source;
    otherSectors.blockPath += QStringLiteral("x");
    otherSectors.sectorSize = 4096;
    report(!diskclone::plan(source, source).error.isEmpty(), QStringLiteral("cloning a drive onto itself is refused"));
    report(diskclone::plan(system, elsewhere).error.contains(QLatin1String("live USB")), QStringLiteral("cloning the running system is refused"),
           diskclone::plan(system, elsewhere).error);
    report(!diskclone::plan(source, otherSectors).error.isEmpty(), QStringLiteral("different sector sizes are refused"));
    const QString smallFile = dir.filePath(QStringLiteral("small.img"));
    makeImage(smallFile, 200 * MiB, '\0');
    attach(smallFile);
    if (const Disk *small = waitForDisk(udisks, smallFile, [](const Disk &) { return true; }))
        report(diskclone::plan(source, *small).error.contains(QLatin1String("too small")), QStringLiteral("a too-small target is refused"),
               diskclone::plan(source, *small).error);

    // Replacing the drive: same IDs, bigger target full of 0xAA.
    const QString tgtFile = dir.filePath(QStringLiteral("target.img"));
    makeImage(tgtFile, 640 * MiB, char(0xAA));
    attach(tgtFile);
    const Disk *tgt = waitForDisk(udisks, tgtFile, [](const Disk &) { return true; });
    QString message;
    const diskclone::Plan plan = tgt ? diskclone::plan(source, *tgt) : diskclone::Plan();
    report(plan.error.isEmpty() && plan.bytes < quint64(260 * MiB), QStringLiteral("only the partitions are copied, not the free space"),
           QStringLiteral("%1 MiB").arg(plan.bytes / MiB));
    report(tgt && cloneTo(udisks, source, *tgt, false, &message), QStringLiteral("clone to a bigger drive"), message);
    if (!tgt)
        return;
    run(udisks, QStringLiteral("re-read its partition table"), [&] { udisks.rescan(tgt->blockPath); });
    tgt = waitForDisk(udisks, tgtFile, [](const Disk &d) { return d.volumes.size() == 3 && !d.volumes[2].uuid.isEmpty(); }, 2000);
    report(tgt, QStringLiteral("the clone shows three partitions"));
    if (!tgt)
        return;
    const Disk target = *tgt;
    bool same = true;
    for (int n = 1; n <= 3; ++n) {
        const Volume *a = volumeOf(source, n), *b = volumeOf(target, n);
        same = same && a && b && a->uuid == b->uuid && a->label == b->label && a->offset == b->offset
               && sha256File(source.device, qint64(a->offset), qint64(a->size)) == sha256File(target.device, qint64(b->offset), qint64(b->size));
    }
    report(same, QStringLiteral("partitions, labels and IDs match the original"));
    const qint64 lastEnd = qint64(volumeOf(source, 3)->offset + volumeOf(source, 3)->size);
    report(allBytes(target.device, lastEnd, 511 * MiB, char(0xAA)) && allBytes(target.device, 513 * MiB, 639 * MiB, char(0xAA)),
           QStringLiteral("free space on the target is left alone"));
    const QString verify = sh(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("sfdisk --verify '%1' 2>&1").arg(target.device)});
    report(verify.contains(QLatin1String("No errors detected")) && !verify.contains(QLatin1String("not on the end")),
           QStringLiteral("the partition table checks out on the bigger drive"), verify.contains(QLatin1String("No errors")) ? QString() : verify);

    const Volume last = *volumeOf(target, 3);
    const ResizeLimits limits = udisks.resizeLimits(last);
    run(udisks, QStringLiteral("grow the last partition into the extra space"), [&] { udisks.resize(last, limits.maxSize); });
    tgt = waitForDisk(udisks, tgtFile, [&](const Disk &d) { const Volume *v = volumeOf(d, 3); return v && v->size > 300 * quint64(MiB); });
    int code = -1;
    sh(QStringLiteral("e2fsck"), {QStringLiteral("-fn"), volumeOf(target, 3)->device}, &code);
    report(tgt && code == 0, QStringLiteral("it grew and its file system is clean"), QStringLiteral("e2fsck exit %1").arg(code));

    // Keeping both: new partition IDs and new file system IDs.
    const QString bothFile = dir.filePath(QStringLiteral("both.img"));
    makeImage(bothFile, 512 * MiB, '\0');
    attach(bothFile);
    const Disk *both = waitForDisk(udisks, bothFile, [](const Disk &) { return true; });
    report(both && cloneTo(udisks, source, *both, true, &message), QStringLiteral("clone for keeping both drives"), message);
    if (both) {
        run(udisks, {}, [&] { udisks.rescan(both->blockPath); }, &message);
        both = waitForDisk(udisks, bothFile, [](const Disk &d) { return d.volumes.size() == 3 && !d.volumes[2].uuid.isEmpty(); }, 2000);
    }
    if (both) {
        const QString srcDump = sh(QStringLiteral("sfdisk"), {QStringLiteral("--dump"), source.device});
        const QString bothDump = sh(QStringLiteral("sfdisk"), {QStringLiteral("--dump"), both->device});
        report(srcDump.section(QStringLiteral("label-id: "), 1).left(36) != bothDump.section(QStringLiteral("label-id: "), 1).left(36)
                   && srcDump.section(QStringLiteral("uuid="), 1).left(36) != bothDump.section(QStringLiteral("uuid="), 1).left(36),
               QStringLiteral("the clone has its own disk and partition IDs"));
        const Disk copy = *both;
        for (const Volume &v : copy.volumes) {
            const QString uuid = diskclone::newUuid(v.fsType);
            run(udisks, QStringLiteral("new %1 ID").arg(v.fsType), [&] { udisks.setUuid(v, uuid); });
        }
        both = waitForDisk(udisks, bothFile, [&](const Disk &d) {
            for (int n = 1; n <= 3; ++n) {
                if (uuidOf(d, n).isEmpty() || uuidOf(d, n) == uuidOf(source, n))
                    return false;
            }
            return true;
        });
        report(both, QStringLiteral("every file system on it has a new ID"));
        sh(QStringLiteral("e2fsck"), {QStringLiteral("-fn"), volumeOf(copy, 3)->device}, &code);
        report(code == 0, QStringLiteral("ext4 is clean after its new ID"));
        sh(QStringLiteral("btrfs"), {QStringLiteral("check"), QStringLiteral("--readonly"), volumeOf(copy, 2)->device}, &code);
        report(code == 0, QStringLiteral("Btrfs is clean after its new ID"));
        sh(QStringLiteral("fsck.vfat"), {QStringLiteral("-n"), volumeOf(copy, 1)->device}, &code);
        report(code == 0, QStringLiteral("FAT is clean after its new ID"));
    }

    // MBR with logical partitions: their little tables between them have to come along.
    const QString mbrFile = dir.filePath(QStringLiteral("mbr.img"));
    makeImage(mbrFile, 128 * MiB, '\0');
    partition(mbrFile, QStringLiteral("label: dos\\nsize=16MiB, type=83\\ntype=5\\nsize=24MiB, type=83\\nsize=32MiB, type=83\\n"));
    attach(mbrFile);
    const Disk *mbr = waitForDisk(udisks, mbrFile, [](const Disk &d) { return d.volumes.size() == 4; });
    report(mbr, QStringLiteral("attach an MBR drive with logical partitions"));
    if (mbr) {
        for (int n : {5, 6})
            sh(QStringLiteral("mkfs.ext4"), {QStringLiteral("-q"), QStringLiteral("-F"), volumeOf(*mbr, n)->device});
        mbr = waitForDisk(udisks, mbrFile, [](const Disk &d) { return d.volumes.size() == 4 && !d.volumes.last().uuid.isEmpty(); });
    }
    const QString mbrTarget = dir.filePath(QStringLiteral("mbr-target.img"));
    makeImage(mbrTarget, 160 * MiB, char(0xAA));
    attach(mbrTarget);
    const Disk *mt = waitForDisk(udisks, mbrTarget, [](const Disk &) { return true; });
    if (mbr && mt) {
        const Disk mbrSource = *mbr;
        report(cloneTo(udisks, mbrSource, *mt, false, &message), QStringLiteral("clone the MBR drive"), message);
        run(udisks, {}, [&] { udisks.rescan(mt->blockPath); }, &message);
        mt = waitForDisk(udisks, mbrTarget, [](const Disk &d) { return d.volumes.size() == 4 && !d.volumes.last().uuid.isEmpty(); }, 2000);
        report(mt && dumpWithoutDevice(mt->device) == dumpWithoutDevice(mbrSource.device), QStringLiteral("all four partitions come across, logical ones too"));
        bool clean = mt;
        for (int n : {5, 6}) {
            sh(QStringLiteral("e2fsck"), {QStringLiteral("-fn"), mt ? volumeOf(*mt, n)->device : QString()}, &code);
            clean = clean && code == 0 && uuidOf(*mt, n) == uuidOf(mbrSource, n);
        }
        report(clean, QStringLiteral("the logical partitions' file systems are intact"));
    }

    for (const QString &loop : loops)
        loopDelete(loop);
}
