// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// DiskAnvil's own exFAT formatter, from Microsoft's published exFAT specification (2019). It's
// here so a memory card gets the SD Card Association's layout on every system (sdcard.h):
// Windows' own format can't place exFAT's tables on a card's erase blocks, and UDisks doesn't
// pass mkfs.exfat's options on.

#include <QString>

#include <functional>

namespace exfat {

struct Options {
    QString label;             // up to 11 characters
    int sectorSize = 512;      // the drive's logical sector size: 512 to 4096
    quint64 partitionOffset = 0; // where the volume starts on its drive, in sectors
    int clusterBytes = 0;      // 0: Windows' choice for the size
    // The FAT starts on a multiple of this many bytes from the drive's start, and the clusters on
    // the next one after the FAT. 0: 1 MiB.
    quint64 alignBytes = 0;
};

// Writes an empty exFAT file system over the `size` bytes at `offset` in `fd`: its boot regions,
// the FAT, the bitmap of used clusters, the up-case table and the root folder (with the label).
bool format(int fd, quint64 offset, quint64 size, const Options &options, QString *error,
            const std::function<void(quint64 done, quint64 total)> &progress = {});

// The cluster size Windows would choose for exFAT on a volume this big, in bytes.
int clusterSize(quint64 size);

// The up-case table, compressed the way the specification allows (runs that stay the same are
// counted), from Unicode's upper case for each character.
QByteArray upcaseTable();
// The specification's 32-bit checksum (rotate right, add), skipping the bytes given.
quint32 checksum(const uchar *data, size_t size, quint32 sum = 0, std::initializer_list<size_t> skip = {});

} // namespace exfat
