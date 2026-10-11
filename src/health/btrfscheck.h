// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Btrfs keeps count of the read, write and checksum errors it has seen on each drive of a
// file system, and the kernel shows them in /sys/fs/btrfs (readable by anyone, only while
// it's mounted). A scrub reads everything back and checks it; it runs as systemd's
// btrfs-scrub@ unit, which comes with btrfs-progs, so DiskForge never runs it as root
// itself.

#include <QString>
#include <QStringList>

namespace btrfscheck {

struct Counts {
    bool known = false; // found in sysfs
    qint64 write = 0;
    qint64 read = 0;
    qint64 flush = 0;
    qint64 corruption = 0; // data or metadata that didn't match its checksum
    qint64 generation = 0; // data older than it should be (a write that never landed)
    int devices = 0;       // drives in the file system (the counts are for all of them)
    bool tempFsid = false; // a clone mounted next to the original, under a made-up ID
    qint64 total() const { return write + read + flush + corruption + generation; }
};

Counts parse(const QByteArray &errorStats); // one drive's error_stats
// The counts for the file system on a block device ("sda1", "dm-0").
Counts read(const QString &deviceName, const QString &sysfs = QStringLiteral("/sys/fs/btrfs"));
QString describe(const Counts &counts); // plain words

// Where to scrub from: one scrub covers the whole file system, whichever subvolume is
// mounted where, so "/" when it's there, otherwise the shortest mount point.
QString scrubMountPoint(const QStringList &mountPoints);
QString scrubUnit(const QString &mountPoint);  // btrfs-scrub@<escaped>.service
QString scrubTimer(const QString &mountPoint); // its monthly timer
// What a finished scrub found, from the unit and the counts before and after.
QString scrubOutcome(bool conditionMet, int exitStatus, const QString &result, qint64 errorsBefore, qint64 errorsAfter);

} // namespace btrfscheck
