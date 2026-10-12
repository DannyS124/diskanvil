// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Put DiskAnvil Live on This PC: DiskAnvil Live on a partition of its own in free space on the PC's
// drive, and in the PC's boot menu (UEFI) after what's there. The PC starts as it did; DiskAnvil
// Live is one pick away in the boot menu, no stick needed. Nothing else on the drive changes, and
// Take It Off gives the space back. BIOS PCs only ever start one program from a drive, so it
// can't go there.

#include "udisks.h"

#include <QString>

namespace liveonpc {

inline const QString kLabel = QStringLiteral("DISKANVIL");  // its FAT32 label, like on a stick
inline const QString kEntry = QStringLiteral("DiskAnvil Live"); // its name in the boot menu
inline const QString kLoader = QStringLiteral("\\EFI\\BOOT\\BOOTX64.EFI");

// Where it can go on `disk`: the end of its biggest free space, when that's big enough, and why not.
struct Place {
    quint64 offset = 0;
    quint64 size = 0;
    quint64 freeBytes = 0; // the biggest free space there is
    QString why;           // when it can't
    bool ok() const { return size > 0; }
};
Place placeOn(const Disk &disk, quint64 needed);

// A PC's own drives it can go on: inside, GPT.
bool suits(const Disk &disk);
// Where it's been put already (the disk and the partition), or nullptr.
const Volume *installed(const QVector<Disk> &disks, const Disk **disk = nullptr);

// What runs as root on Linux (bash, with the drive and the partition number as $1 and $2): the
// boot menu entry made (an old one of the same name first taken out) and put last in the order.
QString addScript();
QString removeScript(); // the entries of that name taken out
// The order with `added` last.
QVector<int> orderWithLast(QVector<int> order, int added);

} // namespace liveonpc
