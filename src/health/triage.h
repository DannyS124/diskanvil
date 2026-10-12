// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// "What happened?": advice for someone who's lost files, from what happened and what kind of
// drive it is. Honest odds (an SSD that trims erases deleted files within minutes; a hard drive
// or memory card keeps them until they're written over), what not to do (format, repair, keep
// using it), and which of DiskForge's tools to use, in order.

#include "udisks.h"

#include <QString>
#include <QStringList>

namespace triage {

enum class Symptom { Deleted, Formatted, WontOpen, Failing, PartitionGone, Ransomware, Damaged, WindowsWontStart };

// Whether a drive erases what's deleted (TRIM): all the time, once a week, not at all.
enum class Trim { NotSsd, Unsupported, Continuous, Weekly, Off, Unknown };

struct Drive {
    bool known = false;     // a drive was picked
    QString name;
    bool ssd = false;
    bool removable = false; // a USB stick, a memory card
    bool failing = false;   // spots it can't read
    bool system = false;    // the running system is on it
    Trim trim = Trim::Unknown;
};

struct Advice {
    enum class Odds { Good, Fair, Poor, Unknown } odds = Odds::Unknown;
    QString odds_why;   // why, in a sentence or two
    QStringList dont;   // what not to do
    QStringList steps;  // what to do, in order, in plain words
    QStringList tools;  // the tools for it, in that order: "rescue-copy", "lost-files", "health",
                        // "recover-partitions", "rescue-usb", "virus-scan", "drivers", "save-files"
};

Advice advise(Symptom symptom, const Drive &drive);

// The drive's facts, from what's known of it (and, for TRIM, how its file systems are mounted
// and whether the weekly trim is switched on).
Drive facts(const Disk &disk, bool runningFromStick);
Trim trimOf(const Disk &disk);

QString oddsWords(Advice::Odds odds);

} // namespace triage
