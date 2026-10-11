// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

#include <limits>

inline const QString kService = QStringLiteral("org.freedesktop.UDisks2");
inline const QString kRoot = QStringLiteral("/org/freedesktop/UDisks2");
inline const QString kManagerPath = QStringLiteral("/org/freedesktop/UDisks2/Manager");
inline const QString kManager = QStringLiteral("org.freedesktop.UDisks2.Manager");
inline const QString kDrive = QStringLiteral("org.freedesktop.UDisks2.Drive");
inline const QString kAta = QStringLiteral("org.freedesktop.UDisks2.Drive.Ata");
inline const QString kNvme = QStringLiteral("org.freedesktop.UDisks2.NVMe.Controller");
inline const QString kBlock = QStringLiteral("org.freedesktop.UDisks2.Block");
inline const QString kPartition = QStringLiteral("org.freedesktop.UDisks2.Partition");
inline const QString kPartitionTable = QStringLiteral("org.freedesktop.UDisks2.PartitionTable");
inline const QString kFilesystem = QStringLiteral("org.freedesktop.UDisks2.Filesystem");
inline const QString kSwapspace = QStringLiteral("org.freedesktop.UDisks2.Swapspace");
inline const QString kEncrypted = QStringLiteral("org.freedesktop.UDisks2.Encrypted");
inline const QString kLoop = QStringLiteral("org.freedesktop.UDisks2.Loop");
inline const QString kJob = QStringLiteral("org.freedesktop.UDisks2.Job");
inline const QString kNvmeNamespace = QStringLiteral("org.freedesktop.UDisks2.NVMe.Namespace");
inline const QString kMDRaid = QStringLiteral("org.freedesktop.UDisks2.MDRaid");
// LVM, from UDisks' lvm2 module
inline const QString kVolumeGroup = QStringLiteral("org.freedesktop.UDisks2.VolumeGroup");
inline const QString kLogicalVolume = QStringLiteral("org.freedesktop.UDisks2.LogicalVolume");
inline const QString kPhysicalVolume = QStringLiteral("org.freedesktop.UDisks2.PhysicalVolume");
inline const QString kManagerLvm = QStringLiteral("org.freedesktop.UDisks2.Manager.LVM2");

// Wipes, checks and image writes can take hours, and polkit may wait for a password:
// never give up on a reply (INT_MAX is libdbus's "no timeout").
inline constexpr int kNoTimeout = std::numeric_limits<int>::max();
