// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Make Room for Windows Recovery (Windows): Windows' recovery partition (WinRE) is often too
// small for its updates, which then fail with error 0x80070643 over and over. Microsoft's own
// fix (KB5028997), done for you: the Windows partition gives 250 MB to the recovery partition
// right after it, which is made again that much bigger.

#include "udisks.h"

#include <QString>

namespace winre {

inline constexpr quint64 kRoom = 250ull * 1024 * 1024; // what its updates need free, and what's moved

// Where reagentc /info says WinRE is (\\?\GLOBALROOT\device\harddisk0\partition4\Recovery\WindowsRE);
// in any language, by that path. -1 when it's switched off.
struct Location {
    int disk = -1, partition = -1;
};
Location parse(const QString &reagentcInfo);

struct Plan {
    bool needed = false;  // its partition has less than 250 MB free
    QString problem;      // why it can't be done this way
    QString disk;         // the drive (\\.\PhysicalDrive0)
    int diskNumber = -1;
    int osPartition = -1, rePartition = -1;
    bool gpt = true;
    quint64 reSize = 0, reFree = 0, osFree = 0;
};
// From where WinRE is, the drives, and which volume Windows runs from (its mount, "C:/").
Plan plan(const Location &at, const QVector<Disk> &disks, const QString &systemMount);
// Microsoft's diskpart steps for it.
QString diskpartScript(const Plan &plan);

} // namespace winre
