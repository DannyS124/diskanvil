// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// New IDs for a copy. A drive cloned or restored from an image keeps the original's IDs:
// its disk ID, every partition's ID and its file systems' IDs. With both connected, Linux
// may mount or start from the wrong one, and Windows switches one of them off. This finds
// such pairs, and fixes what names the old IDs on a Linux the copy holds.

#include "udisks.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace newids {

// Two connected drives that share IDs: one is a copy of the other.
struct Clash {
    QString first;  // the drives' block paths
    QString second;
    QStringList ids; // what they share, in plain words
};
QVector<Clash> clashes(const QVector<Disk> &disks);
// The same for the same pair and IDs, for remembering that it was dismissed.
QString clashKey(const Clash &clash, const QVector<Disk> &disks);

// New disk and partition IDs, written to the drive open at `fd`. A GPT gets a new disk GUID
// and a new one for each partition (in its backup copy too); an MBR a new disk signature,
// which Linux and Windows make the partitions' IDs from.
bool giveDiskNewIds(int fd, bool gpt, int sectorSize, QString *error);

// A file system's serial number, the ID Windows knows it by, put straight into its boot
// sectors: NTFS, FAT and exFAT (whose boot checksum is worked out again). `boot` holds the
// volume's first `bootBytes()` bytes; an NTFS keeps a copy of its boot sector at the very end
// as well, which gets the same change (setSerial on that sector alone).
qsizetype bootBytes(const QString &fsType, int sectorSize);
bool setSerial(QByteArray &boot, const QString &fsType, const QString &serial, QString *error);

// The settings of a Linux on the file system mounted at `root` that name any of `ids`:
// /etc/fstab, /etc/crypttab, its boot entries and GRUB's menu. (A separate /boot or EFI
// partition mounted at `root` has its own boot entries and menu; those are found too.)
QStringList filesNaming(const QString &root, const QStringList &ids);
// Puts each new ID in place of its old one (`renamed`: old to new, any case) in `files`,
// keeping each as it was next to it ("fstab.diskanvil-2026-10-10"). Returns the files changed.
QStringList rewrite(const QStringList &files, const QHash<QString, QString> &renamed, QString *error);

} // namespace newids
