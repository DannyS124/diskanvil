// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What's worth knowing about a drive beyond its health record: how worn an eMMC drive is (the
// kind soldered into laptops, tablets and small boards), partitions that don't start on a 4 KiB
// boundary (slower, and more wear on an SSD), and facts about some models that their makers
// have published: SMR recording (slow for big writes, and not for RAID), and firmware with a
// known bug that an update fixes.

#include "udisks.h"

#include <QString>
#include <QStringList>

namespace drivefacts {

struct EmmcLife {
    bool known = false;
    int usedFrom = -1, usedTo = -1; // percent of its life used: 0 to 10, 10 to 20... (the worse of its two estimates)
    int reserve = 0;                // its spare blocks: 1 normal, 2 a warning (80% used), 3 urgent (90%)
};
// From /sys/block/<name>/device (life_time and pre_eol_info). `sysfs`: where that is, for the tests.
EmmcLife emmcLife(const QString &device, const QString &sysfs = QStringLiteral("/sys/block"));
QString describe(const EmmcLife &life);

// Partitions that don't start on a 4 KiB boundary, in plain words ("Partition 1 starts 31.5 KiB in").
QStringList misaligned(const Disk &disk);

struct Fact {
    enum class Kind { Smr, Firmware, Note } kind = Kind::Note;
    QString text;
};
// What's known about this model (and firmware version).
QVector<Fact> aboutModel(const QString &model, const QString &firmware);

} // namespace drivefacts
