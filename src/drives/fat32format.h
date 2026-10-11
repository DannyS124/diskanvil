// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// DiskForge's own FAT32 formatter, from Microsoft's published FAT specification. Windows only
// makes FAT32 up to 32 GB, but a UEFI boot stick (or an old camera) needs FAT32 whatever the
// size; this makes it on any volume from 33 MB to 2 TB, on Linux and on Windows alike.

#include <QString>

#include <functional>

namespace fat32 {

struct Options {
    QString label;            // up to 11 characters; stored in capitals, like Windows does
    int sectorSize = 512;     // the drive's logical sector size: 512 or 4096
    quint64 hiddenSectors = 0; // where the volume starts on its drive, in sectors (for the boot sector)
};

// Writes an empty FAT32 file system over the `size` bytes at `offset` in `fd`: its boot sectors
// and FATs, and the root folder (with the label). `progress` hears about the FATs, the slow part
// on a big volume. False with `error` said when it can't be done.
bool format(int fd, quint64 offset, quint64 size, const Options &options, QString *error,
            const std::function<void(quint64 done, quint64 total)> &progress = {});

// The cluster size Windows would choose for FAT32 on a volume this big, in bytes.
int clusterSize(quint64 size);

} // namespace fat32
