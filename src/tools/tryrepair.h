// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Try a Repair First (DiskForge Live): a file system's repair run on a stand-in for the
// partition (a device-mapper snapshot: reads come from the partition, writes go to a file in
// memory), so what it would do can be looked at before anything on the drive changes. Then the
// real repair, or nothing.

#include <QString>
#include <QStringList>

namespace tryrepair {

inline constexpr char kName[] = "diskforge-try";
inline constexpr char kMount[] = "/mnt/diskforge-try";

// The repair for a file system, on `device`; empty when there's none to try.
QStringList repairCommand(const QString &fsType, const QString &device);
// As root: makes the stand-in for `device` (its changes kept up to `changeRoom` bytes), runs
// the repair on it, and leaves it there to look at.
QString startScript(const QString &device, const QString &fsType, quint64 changeRoom);
// Mounts the stand-in read-only at kMount, to look.
QString lookScript(const QString &fsType);
// Unmounts it and takes the stand-in away; the drive is as it was.
QString stopScript();

} // namespace tryrepair
