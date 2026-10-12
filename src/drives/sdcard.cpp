// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sdcard.h"

#include "blockio.h"
#include "exfatformat.h"
#include "fat32format.h"

#include <QCoreApplication>
#include <QRandomGenerator>
#include <QtEndian>

#include <cstring>

namespace sdcard {

namespace {

constexpr quint64 KiB = 1024, MiB = 1024 * KiB, GiB = 1024 * MiB, TiB = 1024 * GiB;

QString tr(const char *text)
{
    return QCoreApplication::translate("sdcard", text);
}

// A partition entry's start or end as cylinder, head and sector (255 heads, 63 sectors a
// track), for old devices; past what that can say, the usual "too far" value.
void chs(uchar *at, quint64 lba)
{
    const quint64 cylinder = lba / (255 * 63);
    if (cylinder > 1023) {
        at[0] = 0xFE;
        at[1] = 0xFF;
        at[2] = 0xFF;
        return;
    }
    const quint64 head = (lba / 63) % 255, sector = lba % 63 + 1;
    at[0] = uchar(head);
    at[1] = uchar(sector | ((cylinder >> 2) & 0xC0));
    at[2] = uchar(cylinder & 0xFF);
}

} // namespace

QString Plan::fsName() const
{
    switch (fs) {
    case Fs::Fat16:
        return QStringLiteral("FAT16");
    case Fs::Fat32:
        return QStringLiteral("FAT32");
    case Fs::Exfat:
        break;
    }
    return QStringLiteral("exFAT");
}

Plan plan(quint64 size, int sectorSize)
{
    Plan p;
    if (sectorSize != 512) {
        p.problem = tr("memory cards have 512-byte sectors, and this drive doesn't");
        return p;
    }
    // The SD Card Association's table: the card's size, its cluster size and boundary unit.
    struct Row {
        quint64 upTo;
        int cluster;
        quint64 boundary;
        Plan::Fs fs;
    };
    static const Row rows[] = {
        {256 * MiB, int(16 * KiB), 32 * KiB, Plan::Fs::Fat16}, {1 * GiB, int(16 * KiB), 64 * KiB, Plan::Fs::Fat16},
        {2 * GiB, int(32 * KiB), 64 * KiB, Plan::Fs::Fat16},   {32 * GiB, int(32 * KiB), 4 * MiB, Plan::Fs::Fat32},
        {128 * GiB, int(128 * KiB), 16 * MiB, Plan::Fs::Exfat}, {512 * GiB, int(256 * KiB), 32 * MiB, Plan::Fs::Exfat},
        {2 * TiB, int(512 * KiB), 64 * MiB, Plan::Fs::Exfat},
    };
    if (size <= 64 * MiB) {
        p.problem = tr("cards of 64 MB and less use FAT12, which DiskForge doesn't make: use Format");
        return p;
    }
    const Row *row = nullptr;
    for (const Row &r : rows) {
        if (size <= r.upTo) {
            row = &r;
            break;
        }
    }
    if (!row) {
        p.problem = tr("cards over 2 TB (SDUC) aren't covered: use Format");
        return p;
    }
    p.fs = row->fs;
    p.cluster = row->cluster;
    p.boundary = row->boundary;
    if (p.fs == Plan::Fs::Fat16) {
        // FAT16 keeps one reserved sector, so the partition starts where its data lands on a
        // boundary: the space before it is the boundary less what comes before the data.
        quint64 start = p.boundary;
        for (int round = 0; round < 8; ++round) {
            const quint64 overhead = fat32::fat16Overhead(size - start, p.cluster) * 512;
            quint64 next = (p.boundary - overhead % p.boundary) % p.boundary;
            if (next == 0)
                next = p.boundary; // the MBR needs a sector of its own
            if (next == start)
                break;
            start = next;
        }
        p.partitionStart = start;
        p.mbrType = 0x06;
    } else {
        p.partitionStart = p.boundary;
        p.mbrType = p.fs == Plan::Fs::Fat32 ? 0x0C : 0x07;
    }
    p.partitionSize = (size - p.partitionStart) / 512 * 512;
    return p;
}

bool write(int fd, quint64 cardSize, int sectorSize, const QString &label, QString *error,
           const std::function<void(quint64 done, quint64 total)> &progress)
{
    const Plan p = plan(cardSize, sectorSize);
    if (!p.problem.isEmpty()) {
        *error = p.problem;
        return false;
    }
    // The space before the partition is zeroed (an old partition table or file system there
    // goes), with the new MBR in its first sector.
    const quint64 lead = std::min<quint64>(p.partitionStart, 4 * MiB);
    blockio::Buffer zeros = blockio::alignedBuffer(lead);
    if (!zeros) {
        *error = tr("out of memory");
        return false;
    }
    memset(zeros.get(), 0, lead);
    for (quint64 at = 0; at < p.partitionStart; at += lead) {
        if (!blockio::writeAt(fd, zeros.get(), std::min(lead, p.partitionStart - at), at)) {
            *error = tr("writing to it failed");
            return false;
        }
    }

    const quint64 first = p.partitionStart / 512, sectors = p.partitionSize / 512;
    bool ok = false;
    if (p.fs == Plan::Fs::Exfat) {
        exfat::Options o;
        o.label = label;
        o.partitionOffset = first;
        o.clusterBytes = p.cluster;
        // The SD Card Association puts exFAT's FAT half a boundary unit in, and its clusters on
        // the next boundary (mkfs.exfat's notes say the same).
        o.alignBytes = p.boundary / 2;
        ok = exfat::format(fd, p.partitionStart, p.partitionSize, o, error, progress);
    } else {
        fat32::Options o;
        o.label = label;
        o.hiddenSectors = first;
        o.clusterBytes = p.cluster;
        o.alignBytes = p.boundary;
        ok = p.fs == Plan::Fs::Fat16 ? fat32::formatFat16(fd, p.partitionStart, p.partitionSize, o, error)
                                     : fat32::format(fd, p.partitionStart, p.partitionSize, o, error, progress);
    }
    if (!ok)
        return false;

    // The MBR last: a new disk signature, and the one partition.
    uchar *mbr = reinterpret_cast<uchar *>(zeros.get());
    memset(mbr, 0, 512);
    qToLittleEndian<quint32>(QRandomGenerator::system()->generate() | 1, mbr + 440);
    uchar *e = mbr + 446;
    chs(e + 1, first);
    e[4] = p.mbrType;
    chs(e + 5, first + sectors - 1);
    qToLittleEndian<quint32>(quint32(first), e + 8);
    qToLittleEndian<quint32>(quint32(sectors), e + 12);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    if (!blockio::writeAt(fd, zeros.get(), 512, 0) || !blockio::flush(fd)) {
        *error = tr("writing to it failed");
        return false;
    }
    return true;
}

} // namespace sdcard
