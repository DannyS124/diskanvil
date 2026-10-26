// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The daily background check, off until it's switched on (Disk Health): `diskanvil
// --health-check` once a day, from a systemd user timer on Linux and a Task Scheduler task on
// Windows. It notes each drive's health in its history and shows a desktop notification when one
// got worse since the last check. It never changes anything on a drive.

#include <QString>
#include <QStringList>

#include "udisks.h"

namespace healthwatch {

inline constexpr char kOption[] = "--health-check";

// Why it can't be switched on here, or empty. On DiskAnvil Live it can't (it only runs while it's
// started from the stick, and has no notifications), but the history itself is still kept: on the
// stick with Keep my changes, until it's turned off without.
QString unavailableReason();
// Said under a drive's history on DiskAnvil Live without Keep my changes: it goes at shutdown.
QString historyGoesReason();
bool enabled();
// Switches it on or off; false (and why in `message`) when that didn't work.
bool setEnabled(bool on, QString *message);

// The check itself: what got worse, a line for each drive, as the notification says it. Notes
// every drive's point for today and remembers what it saw for the next check.
QStringList check(UDisks *udisks, const QVector<Disk> &disks);
// Runs the check and shows the notification. The exit code for main().
int run(UDisks *udisks);
// For the tests: where the timer's files go, and no notification is shown (it's printed).
extern QString unitFolderForTest;
extern bool printInsteadForTest;

} // namespace healthwatch
