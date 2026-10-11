// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fat32format.h"

#include "blockio.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QRandomGenerator>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace fat32 {

namespace {

constexpr quint32 kLeastClusters = 65525; // fewer and it's FAT16 to everything that reads it
constexpr quint32 kMostClusters = 0x0FFFFFF5;
constexpr quint64 kChunk = 1024 * 1024;

// Our own boot code, for a PC that tries to start from the drive: it says there's nothing to
// start, waits for a key and hands back to the PC, which tries its next drive. (Loaded at
// 0x7C00; the message follows at 0x7C7C.)
constexpr uchar kBootCode[] = {
    0xFA,             // cli
    0x31, 0xC0,       // xor ax, ax
    0x8E, 0xD8,       // mov ds, ax
    0x8E, 0xD0,       // mov ss, ax
    0xBC, 0x00, 0x7C, // mov sp, 0x7C00
    0xFB,             // sti
    0xBE, 0x7C, 0x7C, // mov si, message
    0xAC,             // next: lodsb
    0x84, 0xC0,       // test al, al
    0x74, 0x09,       // jz wait
    0xB4, 0x0E,       // mov ah, 0x0E (write a character)
    0xBB, 0x07, 0x00, // mov bx, 7
    0xCD, 0x10,       // int 0x10
    0xEB, 0xF2,       // jmp next
    0x31, 0xC0,       // wait: xor ax, ax
    0xCD, 0x16,       // int 0x16 (a key)
    0xCD, 0x19,       // int 0x19 (back to the PC)
};
constexpr char kMessage[] = "This drive has no system to start from. Take it out and press a key.\r\n";
static_assert(0x5A + sizeof kBootCode == 0x7C && 0x7C + sizeof kMessage <= 0x1FE);

QString tr(const char *text)
{
    return QCoreApplication::translate("fat32", text);
}

// A label the way FAT keeps it: 11 characters, capitals, no characters it doesn't allow.
QByteArray labelOf(const QString &label)
{
    QByteArray out;
    for (const QChar c : label.toUpper()) {
        const char ch = c.unicode() < 128 ? char(c.unicode()) : '_';
        out += (ch < 0x20 || QByteArrayView("\"*+,./:;<=>?[\\]|").contains(ch)) ? '_' : ch;
    }
    out = out.left(11);
    return out.isEmpty() ? QByteArray("NO NAME    ") : out.leftJustified(11, ' ');
}

} // namespace

int clusterSize(quint64 size)
{
    // Windows' choice (the FAT specification's table), by the volume's size in 512-byte sectors.
    const quint64 sectors = size / 512;
    if (sectors <= 532480)
        return 512; // up to 260 MB
    if (sectors <= 16777216)
        return 4096; // up to 8 GB
    if (sectors <= 33554432)
        return 8192; // up to 16 GB
    if (sectors <= 67108864)
        return 16384; // up to 32 GB
    return 32768;
}

bool format(int fd, quint64 offset, quint64 size, const Options &options, QString *error,
            const std::function<void(quint64 done, quint64 total)> &progress)
{
    const quint32 ss = quint32(options.sectorSize);
    if (ss != 512 && ss != 1024 && ss != 2048 && ss != 4096) {
        *error = tr("the drive's sector size isn't one FAT32 has");
        return false;
    }
    if (size / ss > 0xFFFFFFFFull) {
        *error = tr("FAT32 can't be this big: 2 TB is the most (on a drive with 512-byte sectors)");
        return false;
    }
    const quint32 total = quint32(size / ss);
    const quint32 spc = quint32(std::max<int>(clusterSize(size), int(ss))) / ss; // sectors per cluster

    // Each FAT has an entry for every cluster (and two more), and the clusters are what's left
    // after the reserved sectors and the two FATs: worked out until it fits.
    const quint32 perSector = ss / 4;
    quint32 reserved = 32;
    quint64 fatSize = 1;
    for (int round = 0; round < 64; ++round) {
        const qint64 data = qint64(total) - reserved - 2 * qint64(fatSize);
        const quint64 clusters = data > 0 ? quint64(data) / spc : 0;
        const quint64 needed = (clusters + 2 + perSector - 1) / perSector;
        if (needed <= fatSize)
            break;
        fatSize = needed;
    }
    // The data starts on a cluster's edge (flash drives write whole clusters fastest).
    if (const quint64 start = reserved + 2 * fatSize; start % spc)
        reserved += quint32(spc - start % spc);
    const quint64 dataStart = reserved + 2 * fatSize;
    const quint64 clusters = total > dataStart ? (total - dataStart) / spc : 0;
    if (clusters < kLeastClusters) {
        *error = tr("it's too small for FAT32 (33 MB at least)");
        return false;
    }
    if (clusters > kMostClusters || reserved > 0xFFFF) {
        *error = tr("FAT32 can't be this big");
        return false;
    }

    // The reserved sectors: the boot sector, FSInfo, and their copies in sectors 6 and 7.
    const QByteArray label = labelOf(options.label);
    blockio::Buffer head = blockio::alignedBuffer(size_t(reserved) * ss);
    if (!head) {
        *error = tr("out of memory");
        return false;
    }
    uchar *b = reinterpret_cast<uchar *>(head.get());
    memset(b, 0, size_t(reserved) * ss);
    b[0] = 0xEB;
    b[1] = 0x58;
    b[2] = 0x90;
    memcpy(b + 3, "MSWIN4.1", 8); // the specification's advice: what the most devices accept
    qToLittleEndian<quint16>(quint16(ss), b + 11);
    b[13] = uchar(spc);
    qToLittleEndian<quint16>(quint16(reserved), b + 14);
    b[16] = 2; // FATs
    b[21] = 0xF8; // a fixed disk
    qToLittleEndian<quint16>(63, b + 24); // sectors per track and heads, for old BIOSes
    qToLittleEndian<quint16>(255, b + 26);
    qToLittleEndian<quint32>(quint32(options.hiddenSectors), b + 28);
    qToLittleEndian<quint32>(total, b + 32);
    qToLittleEndian<quint32>(quint32(fatSize), b + 36);
    qToLittleEndian<quint32>(2, b + 44); // the root folder's cluster
    qToLittleEndian<quint16>(1, b + 48); // FSInfo's sector
    qToLittleEndian<quint16>(6, b + 50); // the backup boot sector's
    b[64] = 0x80;
    b[66] = 0x29;
    qToLittleEndian<quint32>(QRandomGenerator::system()->generate(), b + 67);
    memcpy(b + 71, label.constData(), 11);
    memcpy(b + 82, "FAT32   ", 8);
    memcpy(b + 0x5A, kBootCode, sizeof kBootCode);
    memcpy(b + 0x7C, kMessage, sizeof kMessage);
    b[510] = 0x55;
    b[511] = 0xAA;
    uchar *info = b + ss;
    qToLittleEndian<quint32>(0x41615252, info);
    qToLittleEndian<quint32>(0x61417272, info + 484);
    qToLittleEndian<quint32>(quint32(clusters - 1), info + 488); // all free but the root folder's
    qToLittleEndian<quint32>(3, info + 492);
    qToLittleEndian<quint32>(0xAA550000, info + 508);
    memcpy(b + 6 * ss, b, 2 * ss);

    const quint64 fatBytes = fatSize * ss;
    const quint64 work = 2 * fatBytes;
    blockio::Buffer chunk = blockio::alignedBuffer(kChunk);
    if (!chunk) {
        *error = tr("out of memory");
        return false;
    }
    auto failed = [error] {
        *error = tr("writing to it failed");
        return false;
    };
    // Whatever was there before stops at the end too: an old file system's backup there (NTFS
    // keeps one) could still be taken for it.
    memset(chunk.get(), 0, kChunk);
    const quint64 volumeBytes = quint64(total) * ss;
    const quint64 tail = std::min<quint64>(kChunk, volumeBytes);
    if (!blockio::writeAt(fd, chunk.get(), tail, offset + volumeBytes - tail))
        return failed();
    if (!blockio::writeAt(fd, head.get(), quint64(reserved) * ss, offset))
        return failed();

    // The FATs: the first three entries (the media, a clean volume, the root folder's one
    // cluster), then nothing used.
    quint64 done = 0;
    for (int f = 0; f < 2; ++f) {
        const quint64 at = offset + (quint64(reserved) + f * fatSize) * ss;
        for (quint64 written = 0; written < fatBytes;) {
            const quint64 n = std::min<quint64>(kChunk, fatBytes - written);
            memset(chunk.get(), 0, n);
            if (written == 0) {
                qToLittleEndian<quint32>(0x0FFFFFF8, chunk.get());
                qToLittleEndian<quint32>(0x0FFFFFFF, chunk.get() + 4);
                qToLittleEndian<quint32>(0x0FFFFFFF, chunk.get() + 8);
            }
            if (!blockio::writeAt(fd, chunk.get(), n, at + written))
                return failed();
            written += n;
            done += n;
            if (progress)
                progress(done, work);
        }
    }

    // The root folder: empty, but for the label.
    const quint64 clusterBytes = quint64(spc) * ss;
    blockio::Buffer root = blockio::alignedBuffer(clusterBytes);
    if (!root) {
        *error = tr("out of memory");
        return false;
    }
    memset(root.get(), 0, clusterBytes);
    if (options.label.trimmed().size() > 0) {
        uchar *e = reinterpret_cast<uchar *>(root.get());
        memcpy(e, label.constData(), 11);
        e[11] = 0x08; // a volume label
        const QDateTime now = QDateTime::currentDateTime();
        qToLittleEndian<quint16>(quint16((now.time().hour() << 11) | (now.time().minute() << 5) | (now.time().second() / 2)), e + 22);
        qToLittleEndian<quint16>(quint16(((now.date().year() - 1980) << 9) | (now.date().month() << 5) | now.date().day()), e + 24);
    }
    if (!blockio::writeAt(fd, root.get(), clusterBytes, offset + dataStart * ss))
        return failed();
    if (!blockio::flush(fd)) {
        *error = tr("what was written couldn't be saved to the drive");
        return false;
    }
    return true;
}

} // namespace fat32
