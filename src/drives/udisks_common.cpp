// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// The parts of UDisks that are the same on Linux and Windows: looking things up in what was
// listed, and turning answers into plain words.

#include "udisks.h"

#include "format.h"

#include <algorithm>

QVector<Span> diskSpans(const Disk &disk)
{
    // skip alignment gaps
    constexpr quint64 kMinFree = 2ull * 1024 * 1024;

    QVector<Span> spans;
    quint64 cursor = 0;
    for (int i = 0; i < disk.volumes.size(); ++i) {
        const Volume &v = disk.volumes[i];
        if (v.isContainer)
            continue; // logicals are drawn instead
        if (v.offset > cursor && v.offset - cursor >= kMinFree)
            spans.push_back({-1, cursor, v.offset - cursor});
        spans.push_back({i, v.offset, v.size});
        cursor = std::max(cursor, v.offset + v.size);
    }
    if (disk.size > cursor && disk.size - cursor >= kMinFree)
        spans.push_back({-1, cursor, disk.size - cursor});
    return spans;
}

const Disk *UDisks::diskOf(const Volume &volume) const
{
    for (const Disk &d : m_disks) {
        for (const Volume &v : d.volumes) {
            if (v.objectPath == volume.objectPath)
                return &d;
        }
    }
    return nullptr;
}

const Disk *UDisks::diskByPath(const QString &blockPath) const
{
    for (const Disk &d : m_disks) {
        if (d.blockPath == blockPath)
            return &d;
    }
    return nullptr;
}

bool UDisks::refuseSystem(const Disk *disk, const QString &failure)
{
    // Dialogs keep copies of a disk; what counts is the disk as it is right now.
    if (disk) {
        if (const Disk *current = diskByPath(disk->blockPath))
            disk = current;
    }
    if (disk && !disk->isSystem)
        return false;
    emit operationFinished(false, failure + QStringLiteral(": ")
                                      + (disk ? tr("%1 holds the running system").arg(disk->device) : tr("disk not found")));
    return true;
}

const FsType *UDisks::filesystem(const QString &id) const
{
    for (const FsType &fs : m_filesystems) {
        if (fs.id == id)
            return &fs;
    }
    return nullptr;
}

int UDisks::findDrive(const QVector<Disk> &disks, const QString &serial, const QString &model, quint64 size)
{
    for (int i = 0; i < disks.size(); ++i) {
        const Disk &d = disks[i];
        if (d.isLoop)
            continue;
        if (!serial.isEmpty() ? d.serial == serial : (!model.isEmpty() && d.model == model && d.size == size))
            return i;
    }
    return -1;
}

QString UDisks::secureEraseRefused(const QString &error, bool usb)
{
    // "error=0x04" is the drive's own answer: it aborted the command. UDisks sets its password
    // first and takes it off again when a later step fails.
    if (!error.contains(QLatin1String("SECURITY")) || !error.contains(QLatin1String("error=0x04")))
        return {};
    const QString why = usb ? tr("The drive refused it. It's connected through USB, and most USB adapters don't pass Secure Erase "
                                 "through to the drive. Connect it to a SATA port inside a PC and try again, or use Wipe Disk, "
                                 "which works through any adapter.")
                            : tr("The drive refused it. Turn the drive off and on once (shut the PC down fully, a restart isn't "
                                 "enough) and try again. If it keeps refusing, use Wipe Disk instead.");
    return why + QStringLiteral(" ")
         + tr("If the drive shows up locked after it's been off, it kept the temporary password xxxx: Help has how to unlock it.");
}

void UDisks::openDevice(const Disk &disk, bool writable, bool forBenchmark, bool direct)
{
    OpenMode mode = OpenMode::Read;
    if (forBenchmark)
        mode = writable ? OpenMode::BenchmarkWritable : OpenMode::Benchmark;
    else if (writable)
        mode = direct ? OpenMode::ReadWriteDirect : OpenMode::ReadWrite;
    openBlock(disk.blockPath, mode);
}

// How far a partition can shrink or grow: the same on Linux and Windows, from what its file
// system can do (the table in detectFilesystems()) and the free space right after it.
ResizeLimits UDisks::resizeLimits(const Volume &v) const
{
    constexpr quint64 MiB = 1024 * 1024;
    ResizeLimits limits;
    limits.minSize = limits.maxSize = v.size;

    const Disk *disk = diskOf(v);
    const FsType *fs = filesystem(v.fsType);
    if (!disk || disk->isSystem) {
        limits.reason = tr("Disk holds the running system");
        return limits;
    }
    if (v.number == 0 || v.isContainer) {
        limits.reason = tr("Only partitions can be resized");
        return limits;
    }
    if (!fs || fs->resizeModes == 0) {
        limits.reason = v.fsType.isEmpty() ? tr("No file system to resize")
                                           : tr("%1 can't be resized").arg(fs ? fs->name : v.fsType);
        return limits;
    }
    if (!fs->resizeAvailable) {
        limits.reason = tr("Install %1 to resize %2").arg(fs->package, fs->name);
        return limits;
    }

    const bool canShrink = fs->resizeModes & (OfflineShrink | OnlineShrink);
    const bool canGrow = fs->resizeModes & (OfflineGrow | OnlineGrow);
    if (v.fsTotal)
        limits.used = v.fsTotal - v.fsFree;

    if (canGrow) {
        // only into free space directly after it
        const QVector<Span> spans = diskSpans(*disk);
        for (int i = 0; i + 1 < spans.size(); ++i) {
            if (!spans[i].isFree() && disk->volumes[spans[i].volume].objectPath == v.objectPath && spans[i + 1].isFree()) {
                const quint64 end = std::min<quint64>(spans[i + 1].offset + spans[i + 1].size, disk->size - MiB);
                limits.maxSize = std::max<quint64>(v.size, (end - v.offset) / MiB * MiB);
            }
        }
    }
    if (canShrink) {
        // unmounted: usage unknown, the resize tool refuses if too small
        quint64 floor = limits.used ? limits.used + std::max<quint64>(limits.used / 10, 32 * MiB) : 32 * MiB;
        if (v.fsType == QLatin1String("btrfs"))
            floor = std::max<quint64>(floor, 256 * MiB); // kernel minimum
        limits.minSize = std::min<quint64>(v.size, (floor + MiB - 1) / MiB * MiB);
    }

    limits.possible = limits.minSize < v.size || limits.maxSize > v.size;
    if (!limits.possible)
        limits.reason = canGrow ? tr("No unallocated space right after it, and %1 can't shrink").arg(fs->name)
                                : tr("%1 can only grow, and there's no unallocated space after it").arg(fs->name);
    return limits;
}

bool UDisks::resizeNeedsRemount(const Volume &v, bool shrink) const
{
    const FsType *fs = filesystem(v.fsType);
    const int modes = fs ? fs->resizeModes : 0;
    const bool mounted = !v.mountPoints.isEmpty();
    const bool online = modes & (shrink ? OnlineShrink : OnlineGrow);
    const bool offline = modes & (shrink ? OfflineShrink : OfflineGrow);
    return mounted ? !online : !offline;
}
