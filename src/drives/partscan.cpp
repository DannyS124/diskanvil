// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "partscan.h"

#include "format.h"

#ifdef Q_OS_WIN
#include "blockio.h"
#include "fsprobe.h"
#else
#include <blkid.h>
#endif

#include <algorithm>
#include <cstring>

namespace {

constexpr quint64 kMiB = 1024 * 1024;

} // namespace

#ifndef Q_OS_WIN

namespace {

// The file systems worth finding; anything else stays unknown rather than a false match.
const char *const kTypes[] = {"ext2", "ext3", "ext4", "btrfs", "xfs", "vfat", "exfat", "ntfs", "f2fs", "swap",
                              "crypto_LUKS", "LVM2_member", "jfs", "reiserfs", "hfsplus", "udf", nullptr};

QString value(blkid_probe pr, const char *name)
{
    const char *data = nullptr;
    size_t length = 0; // counts the closing NUL
    if (blkid_probe_lookup_value(pr, name, &data, &length) != 0 || !data)
        return {};
    return cleanName(QString::fromUtf8(data, qsizetype(strnlen(data, qMin<size_t>(length, 256))))).trimmed();
}

} // namespace

std::optional<partscan::Found> partscan::probe(int fd, quint64 offset, quint64 deviceSize)
{
    if (offset >= deviceSize)
        return std::nullopt;
    blkid_probe pr = blkid_new_probe();
    if (!pr)
        return std::nullopt;
    std::optional<Found> out;
    if (blkid_probe_set_device(pr, fd, blkid_loff_t(offset), blkid_loff_t(deviceSize - offset)) == 0) {
        blkid_probe_enable_partitions(pr, 0);
        blkid_probe_enable_superblocks(pr, 1);
        // File system sizes need libblkid 2.39 (2023). Older ones still say what's there, so
        // the scan works; sizes then run up to the next thing found.
#ifdef BLKID_SUBLKS_FSINFO
        blkid_probe_set_superblocks_flags(pr, BLKID_SUBLKS_TYPE | BLKID_SUBLKS_LABEL | BLKID_SUBLKS_UUID | BLKID_SUBLKS_FSINFO);
#else
        blkid_probe_set_superblocks_flags(pr, BLKID_SUBLKS_TYPE | BLKID_SUBLKS_LABEL | BLKID_SUBLKS_UUID);
#endif
        blkid_probe_filter_superblocks_type(pr, BLKID_FLTR_ONLYIN, const_cast<char **>(kTypes));
        if (blkid_do_safeprobe(pr) == 0) { // 1 = nothing, -2 = two at once, -1 = error
            Found f;
            f.offset = offset;
            f.type = value(pr, "TYPE");
            f.label = value(pr, "LABEL");
            f.uuid = value(pr, "UUID");
            const quint64 size = value(pr, "FSSIZE").toULongLong();
            f.size = size <= deviceSize - offset ? size : 0;
            if (!f.type.isEmpty())
                out = f;
        }
    }
    blkid_free_probe(pr);
    return out;
}

#else

// Windows has no libblkid: DiskAnvil's own probe, which knows the same file systems' headers
// and what size they say they are. Read at each spot: the first 16 KiB (boot sectors,
// superblocks, swap's page) and the 4 KiB 64 KiB in, where Btrfs keeps its superblock.
std::optional<partscan::Found> partscan::probe(int fd, quint64 offset, quint64 deviceSize)
{
    static const QStringList types = {QStringLiteral("ext2"), QStringLiteral("ext3"),  QStringLiteral("ext4"),        QStringLiteral("btrfs"),
                                      QStringLiteral("xfs"),  QStringLiteral("vfat"),  QStringLiteral("exfat"),       QStringLiteral("ntfs"),
                                      QStringLiteral("swap"), QStringLiteral("hfsplus"), QStringLiteral("crypto_LUKS"), QStringLiteral("LVM2_member")};
    constexpr quint64 kFirst = 16 * 1024, kBtrfs = 64 * 1024, kBtrfsLength = 4096;
    if (offset >= deviceSize)
        return std::nullopt;
    blockio::Buffer buffer = blockio::alignedBuffer(kFirst);
    const quint64 first = std::min(kFirst, deviceSize - offset);
    if (!buffer || !blockio::readAt(fd, buffer.get(), first, offset))
        return std::nullopt;
    QByteArray head(fsprobe::kHeadSize, '\0');
    memcpy(head.data(), buffer.get(), first);
    if (deviceSize - offset >= kBtrfs + kBtrfsLength && blockio::readAt(fd, buffer.get(), kBtrfsLength, offset + kBtrfs))
        memcpy(head.data() + kBtrfs, buffer.get(), kBtrfsLength);
    const fsprobe::Result r = fsprobe::probe(head);
    if (!r.found() || !types.contains(r.type))
        return std::nullopt;
    Found f;
    f.offset = offset;
    f.type = r.type;
    f.label = cleanName(r.label).trimmed();
    f.uuid = r.uuid;
    f.size = r.size <= deviceSize - offset ? r.size : 0;
    return f;
}

#endif

QVector<partscan::Found> partscan::scan(int fd, quint64 deviceSize, const std::function<bool(quint64, quint64)> &progress)
{
    // Where partitions start: every MiB (anything from this century), plus the cylinder
    // starts old tools used (63 sectors in, then every 255 heads x 63 sectors).
    QVector<quint64> spots;
    for (quint64 at = kMiB; at < deviceSize; at += kMiB)
        spots.push_back(at);
    constexpr quint64 cylinder = 255 * 63 * 512;
    for (quint64 at = 63 * 512; at < deviceSize; at += cylinder) {
        if (at % kMiB)
            spots.push_back(at);
    }
    std::sort(spots.begin(), spots.end());
    QVector<Found> found;
    for (qsizetype i = 0; i < spots.size(); ++i) {
        if (i % 64 == 0 && progress && !progress(spots[i], deviceSize))
            break;
        if (const std::optional<Found> f = probe(fd, spots[i], deviceSize))
            found.push_back(*f);
    }
    if (progress)
        progress(deviceSize, deviceSize);
    return found;
}

QVector<partscan::Found> partscan::tidy(QVector<Found> found, quint64 deviceSize, int sectorSize)
{
    const quint64 sector = quint64(std::max(sectorSize, 512));
    // Room for the backup GPT at the end.
    const quint64 limit = deviceSize > 34 * sector ? (deviceSize - 34 * sector) / sector * sector : 0;
    std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) { return a.offset < b.offset; });
    QVector<Found> kept;
    for (Found f : std::as_const(found)) {
        if (f.offset < 34 * sector || f.offset >= limit)
            continue; // where the table itself goes (old disks start at sector 63, which is fine)
        if (!kept.isEmpty()) {
            const Found &last = kept.last();
            // Inside the one before (a disk image stored in it, say): not a partition.
            if (last.size && f.offset < last.offset + last.size)
                continue;
        }
        kept.push_back(f);
    }
    for (int i = 0; i < kept.size(); ++i) {
        Found &f = kept[i];
        const quint64 next = i + 1 < kept.size() ? kept[i + 1].offset : limit;
        if (f.size) {
            const quint64 rounded = (f.size + kMiB - 1) / kMiB * kMiB;
            f.size = std::min(rounded, next - f.offset) / sector * sector;
        } else {
            f.size = (next - f.offset) / sector * sector;
            f.sizeGuessed = true;
        }
    }
    kept.erase(std::remove_if(kept.begin(), kept.end(), [](const Found &f) { return f.size == 0; }), kept.end());
    return kept;
}

QString partscan::typeFor(const QString &fs, const QString &table)
{
    const bool windows = fs == QLatin1String("vfat") || fs == QLatin1String("exfat") || fs == QLatin1String("ntfs");
    if (table == QLatin1String("dos")) {
        if (fs == QLatin1String("vfat"))
            return QStringLiteral("0x0c");
        if (windows)
            return QStringLiteral("0x07");
        if (fs == QLatin1String("swap"))
            return QStringLiteral("0x82");
        if (fs == QLatin1String("LVM2_member"))
            return QStringLiteral("0x8e");
        return QStringLiteral("0x83");
    }
    if (windows)
        return QStringLiteral("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7");
    if (fs == QLatin1String("swap"))
        return QStringLiteral("0657fd6d-a4ab-43c4-84e5-0933c84b4f4f");
    if (fs == QLatin1String("crypto_LUKS"))
        return QStringLiteral("ca7d7ccb-63ed-4c53-861c-1742536059cc");
    if (fs == QLatin1String("LVM2_member"))
        return QStringLiteral("e6d6d379-f507-44c2-a23c-238f2a3df928");
    return QStringLiteral("0fc63daf-8483-4772-8e79-3d69d8477de4");
}

recover::Layout partscan::toLayout(const QVector<Found> &parts, const QString &table, quint64 deviceSize)
{
    recover::Layout l;
    l.saved = QDateTime::currentDateTimeUtc();
    l.table = table;
    l.diskSize = deviceSize;
    int number = 0;
    for (const Found &f : parts) {
        recover::Part p;
        p.number = ++number;
        p.start = f.offset;
        p.size = f.size;
        p.type = typeFor(f.type, table);
        p.fsType = f.type;
        p.label = f.label;
        if (table == QLatin1String("gpt"))
            p.name = f.label.left(36);
        l.parts.push_back(p);
    }
    return l;
}
