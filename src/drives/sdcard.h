// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A memory card formatted the way the SD Card Association specifies (SD Specifications Part 2,
// File System Specification): FAT16 up to 2 GB, FAT32 up to 32 GB, exFAT above, with the cluster
// size for the card's size, and the partition and the file system's data placed on the card's
// erase blocks ("boundary units"). Cameras and phones expect it, and the card wears less and
// writes faster than with a computer's usual format. The same on Linux, DiskForge Live and
// Windows: DiskForge writes it all itself.

#include <QString>

#include <functional>

namespace sdcard {

struct Plan {
    enum class Fs { Fat16, Fat32, Exfat } fs = Fs::Fat32;
    quint64 boundary = 0;     // the boundary unit, in bytes
    int cluster = 0;          // bytes
    quint64 partitionStart = 0; // bytes from the card's start
    quint64 partitionSize = 0;
    quint8 mbrType = 0;       // 0x06 FAT16, 0x0C FAT32 (LBA), 0x07 exFAT
    QString problem;          // why the card can't be laid out this way
    QString fsName() const;
};

// The layout for a card of this size.
Plan plan(quint64 cardSize, int sectorSize = 512);

// Writes it: a new MBR with one partition, then the file system. Everything on the card is lost.
bool write(int fd, quint64 cardSize, int sectorSize, const QString &label, QString *error,
           const std::function<void(quint64 done, quint64 total)> &progress = {});

} // namespace sdcard
