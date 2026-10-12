// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Drives that only make sense together and aren't put together yet: the drives of a Linux RAID
// array (a NAS's, taken out and plugged into this PC: Synology, QNAP and most others are Linux
// RAID, often with LVM on top), and Windows dynamic disks (Windows' own way of spanning and
// mirroring). They're opened read-only, as programs of their own: mdadm, LVM's vgchange, and
// ldmtool.

#include "udisks.h"

#include <QString>
#include <QStringList>

namespace closedsets {

struct Set {
    enum class Kind { Raid, Dynamic } kind = Kind::Raid;
    QString name;        // the array's name ("DiskStation:2"); empty for dynamic disks
    QString id;          // the array's UUID
    QStringList members; // devices, /dev/sdb5...
};

// What's here but not opened. `dynamicOpen`: Windows' dynamic volumes are open already (some
// /dev/mapper/ldm_vol_ exist).
QVector<Set> find(const QVector<Disk> &disks, bool dynamicOpen);
QString describe(const Set &set);
// The script that opens them, run as root: each part only when its program is there.
QString openScript(const QVector<Set> &sets);
// The programs it needs that aren't here (mdadm, ldmtool), from `has`.
QStringList missing(const QVector<Set> &sets, const std::function<bool(const QString &)> &has);
// Whether these partition types are Windows dynamic disks' (GPT's LDM partitions, MBR's 0x42).
bool isDynamicType(const QString &partType);

} // namespace closedsets
