// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "exfatformat.h"

#include "blockio.h"

#include <QChar>
#include <QCoreApplication>
#include <QRandomGenerator>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace exfat {

namespace {

constexpr quint64 kChunk = 1024 * 1024;
constexpr quint32 kMostClusters = 0xFFFFFFF5 - 2;

QString tr(const char *text)
{
    return QCoreApplication::translate("exfat", text);
}

quint64 roundUp(quint64 value, quint64 unit)
{
    return (value + unit - 1) / unit * unit;
}

int shiftOf(quint64 value)
{
    int shift = 0;
    while ((quint64(1) << shift) < value)
        ++shift;
    return shift;
}

} // namespace

int clusterSize(quint64 size)
{
    if (size <= 256ull * 1024 * 1024)
        return 4096;
    if (size <= 32ull * 1024 * 1024 * 1024)
        return 32 * 1024;
    return 128 * 1024;
}

quint32 checksum(const uchar *data, size_t size, quint32 sum, std::initializer_list<size_t> skip)
{
    for (size_t i = 0; i < size; ++i) {
        if (std::find(skip.begin(), skip.end(), i) != skip.end())
            continue;
        sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + data[i];
    }
    return sum;
}

QByteArray upcaseTable()
{
    QByteArray out;
    auto put = [&out](quint16 v) {
        char b[2];
        qToLittleEndian<quint16>(v, b);
        out.append(b, 2);
    };
    // Each character's capital, in order; a run of characters that are their own capital is
    // written as 0xFFFF and how many there are.
    quint32 c = 0;
    while (c <= 0xFFFF) {
        const char16_t upper = (c >= 0xD800 && c <= 0xDFFF) ? char16_t(c) : QChar(char16_t(c)).toUpper().unicode();
        if (upper != c) {
            put(upper);
            ++c;
            continue;
        }
        quint32 run = c;
        while (run <= 0xFFFF && (run >= 0xD800 && run <= 0xDFFF ? true : QChar(char16_t(run)).toUpper().unicode() == run))
            ++run;
        const quint32 count = run - c;
        if (count >= 3 && c != 0xFFFF) {
            // A count over 0xFFFF can't be written: the rest is split.
            for (quint32 left = count; left > 0;) {
                const quint32 n = std::min<quint32>(left, 0xFFFE);
                put(0xFFFF);
                put(quint16(n));
                left -= n;
            }
        } else {
            for (quint32 i = c; i < run; ++i)
                put(quint16(i));
        }
        c = run;
    }
    return out;
}

bool format(int fd, quint64 offset, quint64 size, const Options &options, QString *error,
            const std::function<void(quint64 done, quint64 total)> &progress)
{
    const quint64 ss = quint64(options.sectorSize);
    if (ss < 512 || ss > 4096 || (ss & (ss - 1))) {
        *error = tr("the drive's sector size isn't one exFAT has");
        return false;
    }
    const quint64 cluster = quint64(options.clusterBytes > 0 ? options.clusterBytes : clusterSize(size));
    if (cluster < ss || (cluster & (cluster - 1)) || cluster > 32ull * 1024 * 1024) {
        *error = tr("that cluster size can't be used here");
        return false;
    }
    const quint64 spc = cluster / ss;
    const quint64 total = size / ss;
    const quint64 unit = std::max<quint64>(1, (options.alignBytes ? options.alignBytes : kChunk) / ss);
    const quint64 start = options.partitionOffset;

    // The boot regions take 24 sectors; the FAT starts on the first boundary after them, and the
    // clusters on the first one after the FAT, which has an entry for each cluster (and two more).
    const quint64 fatOffset = roundUp(start + 24, unit) - start;
    quint64 fatLength = 1, heapOffset = 0, clusters = 0;
    for (int round = 0; round < 64; ++round) {
        heapOffset = roundUp(start + fatOffset + fatLength, unit) - start;
        clusters = total > heapOffset ? std::min<quint64>((total - heapOffset) / spc, kMostClusters) : 0;
        const quint64 needed = std::max<quint64>(1, roundUp((clusters + 2) * 4, ss) / ss);
        if (needed <= fatLength)
            break;
        fatLength = needed;
    }
    const QByteArray upcase = upcaseTable();
    const quint64 bitmapBytes = (clusters + 7) / 8;
    const quint64 bitmapClusters = roundUp(bitmapBytes, cluster) / cluster;
    const quint64 upcaseClusters = roundUp(quint64(upcase.size()), cluster) / cluster;
    const quint64 used = bitmapClusters + upcaseClusters + 1; // and the root folder
    if (clusters < used + 1 || heapOffset > 0xFFFFFFFFull) {
        *error = tr("it's too small for exFAT");
        return false;
    }
    const quint32 bitmapCluster = 2, upcaseCluster = quint32(2 + bitmapClusters), rootCluster = quint32(upcaseCluster + upcaseClusters);

    // The boot region: the boot sector, eight more, the maker's parameters (none), a reserved
    // sector, and the checksum of all of them; then the same again as its backup.
    const size_t region = size_t(12 * ss);
    blockio::Buffer boot = blockio::alignedBuffer(2 * region);
    blockio::Buffer chunk = blockio::alignedBuffer(kChunk);
    if (!boot || !chunk) {
        *error = tr("out of memory");
        return false;
    }
    uchar *b = reinterpret_cast<uchar *>(boot.get());
    memset(b, 0, 2 * region);
    b[0] = 0xEB;
    b[1] = 0x76;
    b[2] = 0x90;
    memcpy(b + 3, "EXFAT   ", 8);
    qToLittleEndian<quint64>(start, b + 64);
    qToLittleEndian<quint64>(total, b + 72);
    qToLittleEndian<quint32>(quint32(fatOffset), b + 80);
    qToLittleEndian<quint32>(quint32(fatLength), b + 84);
    qToLittleEndian<quint32>(quint32(heapOffset), b + 88);
    qToLittleEndian<quint32>(quint32(clusters), b + 92);
    qToLittleEndian<quint32>(rootCluster, b + 96);
    qToLittleEndian<quint32>(QRandomGenerator::system()->generate(), b + 100);
    qToLittleEndian<quint16>(0x0100, b + 104); // version 1.0
    b[108] = uchar(shiftOf(ss));
    b[109] = uchar(shiftOf(spc));
    b[110] = 1;    // one FAT
    b[111] = 0x80; // drive select
    b[112] = uchar(used * 100 / clusters);
    memset(b + 120, 0xF4, 390); // no boot code: halt
    b[510] = 0x55;
    b[511] = 0xAA;
    for (int s = 1; s <= 8; ++s)
        qToLittleEndian<quint32>(0xAA550000, b + s * ss + ss - 4);
    const quint32 sum = checksum(b, size_t(11 * ss), 0, {106, 107, 112});
    for (quint64 i = 0; i < ss; i += 4)
        qToLittleEndian<quint32>(sum, b + 11 * ss + i);
    memcpy(b + region, b, region);

    auto failed = [error] {
        *error = tr("writing to it failed");
        return false;
    };
    // Whatever was there before stops at the end too, and between the boot regions and the FAT.
    memset(chunk.get(), 0, kChunk);
    const quint64 tail = std::min<quint64>(kChunk, total * ss);
    if (!blockio::writeAt(fd, chunk.get(), tail, offset + total * ss - tail))
        return failed();
    for (quint64 at = 24 * ss; at < fatOffset * ss;) {
        const quint64 n = std::min<quint64>(kChunk, fatOffset * ss - at);
        if (!blockio::writeAt(fd, chunk.get(), n, offset + at))
            return failed();
        at += n;
    }

    // The FAT: its first two entries, then the bitmap's, the up-case table's and the root
    // folder's clusters, each a chain to its end.
    const quint64 fatBytes = fatLength * ss;
    const quint64 work = fatBytes + used * cluster;
    quint64 done = 0;
    const quint32 ends[] = {quint32(upcaseCluster - 1), quint32(rootCluster - 1), rootCluster};
    for (quint64 written = 0; written < fatBytes;) {
        const quint64 n = std::min<quint64>(kChunk, fatBytes - written);
        memset(chunk.get(), 0, n);
        for (quint64 i = 0; i < n; i += 4) {
            const quint64 entry = (written + i) / 4;
            quint32 value = 0;
            if (entry > rootCluster)
                break;
            if (entry == 0)
                value = 0xFFFFFFF8;
            else if (entry == 1 || std::find(std::begin(ends), std::end(ends), quint32(entry)) != std::end(ends))
                value = 0xFFFFFFFF;
            else
                value = quint32(entry + 1);
            qToLittleEndian<quint32>(value, chunk.get() + i);
        }
        if (!blockio::writeAt(fd, chunk.get(), n, offset + fatOffset * ss + written))
            return failed();
        written += n;
        done += n;
        if (progress)
            progress(done, work);
    }

    // The clusters in use: the bitmap (its first `used` bits set), the up-case table, and the
    // root folder with the label, the bitmap's and the table's entries.
    QByteArray heap(qsizetype(used * cluster), '\0');
    uchar *h = reinterpret_cast<uchar *>(heap.data());
    for (quint64 i = 0; i < used; ++i)
        h[i / 8] |= uchar(1u << (i % 8));
    memcpy(h + bitmapClusters * cluster, upcase.constData(), size_t(upcase.size()));
    uchar *root = h + (bitmapClusters + upcaseClusters) * cluster;
    const QString name = options.label.trimmed().left(11);
    root[0] = 0x83; // the volume label
    root[1] = uchar(name.size());
    for (qsizetype i = 0; i < name.size(); ++i)
        qToLittleEndian<quint16>(name.at(i).unicode(), root + 2 + 2 * i);
    root[32] = 0x81; // the allocation bitmap
    qToLittleEndian<quint32>(bitmapCluster, root + 32 + 20);
    qToLittleEndian<quint64>(bitmapBytes, root + 32 + 24);
    root[64] = 0x82; // the up-case table
    qToLittleEndian<quint32>(checksum(reinterpret_cast<const uchar *>(upcase.constData()), size_t(upcase.size())), root + 64 + 4);
    qToLittleEndian<quint32>(upcaseCluster, root + 64 + 20);
    qToLittleEndian<quint64>(quint64(upcase.size()), root + 64 + 24);
    for (quint64 written = 0; written < quint64(heap.size());) {
        const quint64 n = std::min<quint64>(kChunk, quint64(heap.size()) - written);
        memcpy(chunk.get(), h + written, n);
        if (!blockio::writeAt(fd, chunk.get(), n, offset + heapOffset * ss + written))
            return failed();
        written += n;
        done += n;
        if (progress)
            progress(done, work);
    }
    // The boot regions last: until they're there, nothing takes the volume for exFAT.
    if (!blockio::writeAt(fd, boot.get(), 2 * region, offset))
        return failed();
    if (!blockio::flush(fd)) {
        *error = tr("what was written couldn't be saved to the drive");
        return false;
    }
    return true;
}

} // namespace exfat
