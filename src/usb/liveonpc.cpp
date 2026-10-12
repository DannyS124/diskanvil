// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "liveonpc.h"

#include "format.h"

#include <QCoreApplication>

namespace liveonpc {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("liveonpc", text);
}

constexpr quint64 MiB = 1024 * 1024;

} // namespace

bool suits(const Disk &disk)
{
    return !disk.removable && disk.bus != QLatin1String("usb") && !disk.sdCard && !disk.isRaid && !disk.isLvm && !disk.isDynamicVolume
           && disk.tableType == QLatin1String("gpt") && !disk.readOnly;
}

Place placeOn(const Disk &disk, quint64 needed)
{
    Place place;
    if (disk.tableType != QLatin1String("gpt")) {
        place.why = disk.tableType.isEmpty() ? tr("This drive has no partition table.")
                                             : tr("This drive has an MBR partition table: UEFI PCs start from GPT drives.");
        return place;
    }
    quint64 bestOffset = 0;
    for (const Span &s : diskSpans(disk)) {
        // The last MiB stays free for GPT's backup.
        const quint64 end = std::min<quint64>(s.offset + s.size, disk.size > MiB ? disk.size - MiB : 0);
        const quint64 start = (s.offset + MiB - 1) / MiB * MiB;
        if (!s.isFree() || end <= start)
            continue;
        if (end / MiB * MiB - start > place.freeBytes) {
            place.freeBytes = end / MiB * MiB - start;
            bestOffset = start;
        }
    }
    const quint64 size = (needed + MiB - 1) / MiB * MiB;
    if (place.freeBytes < size) {
        place.why = place.freeBytes < MiB
                        ? tr("This drive has no free space. Shrink a partition first (Resize), to leave %1 free.").arg(formatSize(size))
                        : tr("This drive has %1 free; DiskAnvil Live needs %2, so %3 more. Shrink a partition first (Resize) to leave it.")
                              .arg(formatSize(place.freeBytes), formatSize(size), formatSize(size - place.freeBytes));
        return place;
    }
    // At the end of the free space, out of the way of a partition that grows into it later.
    place.offset = bestOffset + place.freeBytes - size;
    place.size = size;
    return place;
}

const Volume *installed(const QVector<Disk> &disks, const Disk **disk)
{
    for (const Disk &d : disks) {
        if (d.removable || d.bus == QLatin1String("usb"))
            continue;
        for (const Volume &v : d.volumes) {
            if (v.fsType == QLatin1String("vfat") && v.label == kLabel && d.tableType == QLatin1String("gpt")) {
                if (disk)
                    *disk = &d;
                return &v;
            }
        }
    }
    return nullptr;
}

QString addScript()
{
    return QStringLiteral(R"(set -e
label='%1'
ours() { efibootmgr | sed -n "s/^Boot\([0-9A-Fa-f]\{4\}\)\*\{0,1\} $label	.*/\1/p"; }
numbers() { efibootmgr | sed -n 's/^Boot\([0-9A-Fa-f]\{4\}\).*/\1/p' | sort; }
for n in $(ours); do efibootmgr -q -b "$n" -B; done
order=$(efibootmgr | sed -n 's/^BootOrder: //p')
before=$(numbers)
efibootmgr -q -C -d "$1" -p "$2" -l '%2' -L "$label"
new=$(comm -13 <(echo "$before") <(numbers) | head -1)
[ -n "$new" ] || { echo "the firmware didn't keep the new entry"; exit 1; }
efibootmgr -q -o "${order:+$order,}$new"
echo "Boot$new"
)")
        .arg(kEntry, kLoader);
}

QString removeScript()
{
    return QStringLiteral(R"(set -e
label='%1'
for n in $(efibootmgr | sed -n "s/^Boot\([0-9A-Fa-f]\{4\}\)\*\{0,1\} $label	.*/\1/p"); do efibootmgr -q -b "$n" -B; done
)")
        .arg(kEntry);
}

QVector<int> orderWithLast(QVector<int> order, int added)
{
    order.removeAll(added);
    order.append(added);
    return order;
}

} // namespace liveonpc
