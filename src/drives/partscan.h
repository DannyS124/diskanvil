// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Finding lost partitions by their file systems: where a partition table is gone, the file
// systems usually still start where the partitions did. libblkid (the same library UDisks
// and udev use) looks at every megabyte of the drive, and at the old 63-sector cylinder
// positions, for one it knows; their sizes come from the file systems themselves.

#include "partrecover.h"

#include <QString>
#include <QVector>

#include <functional>
#include <optional>

namespace partscan {

struct Found {
    quint64 offset = 0;
    quint64 size = 0; // what the file system says; 0 = it doesn't say (LUKS)
    QString type;     // libblkid's name: ext4, vfat, btrfs, swap, crypto_LUKS...
    QString label;
    QString uuid;
    bool sizeGuessed = false; // set by tidy() when the size ran up to the next one
};

// The file system at one spot, read-only; nothing for none, two at once, or an error.
std::optional<Found> probe(int fd, quint64 offset, quint64 deviceSize);
// Every MiB and every old cylinder start, past the first MiB. progress(done, total)
// returns false to stop; what was found so far is returned then.
QVector<Found> scan(int fd, quint64 deviceSize, const std::function<bool(quint64, quint64)> &progress);
// In order, none inside another one, sizes rounded up to whole MiB and kept clear of the
// next one, the end of the drive and the backup GPT. Ones that don't say run up to the next.
QVector<Found> tidy(QVector<Found> found, quint64 deviceSize, int sectorSize = 512);
// The partition type for a file system.
QString typeFor(const QString &fsType, const QString &table);
recover::Layout toLayout(const QVector<Found> &parts, const QString &table, quint64 deviceSize);

} // namespace partscan
