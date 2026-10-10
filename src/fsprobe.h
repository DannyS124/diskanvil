// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What starts at a spot on a drive, from its first bytes: a file system, an encrypted
// container, swap, a RAID or LVM member. Windows only knows its own file systems, so on
// Windows this is how a Linux partition shows up as ext4 or LUKS instead of "unknown".
// The names are the ones Linux (libblkid) uses, so the rest of DiskForge needn't care.

#include <QByteArray>
#include <QString>

namespace fsprobe {

// How much to read from the start: the Btrfs superblock is the furthest in, at 64 KiB.
constexpr int kHeadSize = 68 * 1024;

struct Result {
    QString type;  // ext4, btrfs, xfs, swap, crypto_LUKS, ntfs, vfat, exfat, BitLocker...
    QString usage; // filesystem, crypto, other (swap), raid (RAID and LVM members)
    QString label;
    QString uuid;
    quint64 size = 0; // what the file system says it covers, in bytes; 0 when it doesn't say (LUKS)
    bool found() const { return !type.isEmpty(); }
};

// `head` is the first kHeadSize bytes, or as many as there are.
Result probe(const QByteArray &head);

} // namespace fsprobe
