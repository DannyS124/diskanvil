// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Getting files back from btrfs with btrfs-progs' own tools, run as programs of their own:
// btrfs writes changes to new places and leaves the older copies of its file tree behind until
// the space is needed again. btrfs-find-root lists those older trees, and btrfs restore copies
// files out of any of them, including files deleted since. Nothing is written to the drive.

#include "lostsource.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace btrfsrestore {

// Whether btrfs-progs is there to run.
bool available();

// Where btrfs file systems are in the source: on their own, or in its partitions.
QVector<lost::Extent> volumes(lost::Source &source);

struct Tree {
    quint64 at = 0;         // where its root is, for btrfs restore -t; 0 for the one in use now
    quint64 generation = 0; // the higher, the newer
};
// The trees btrfs-find-root found, the one in use first, then the older ones, newest first.
QVector<Tree> parseTrees(const QString &findRootOutput);
// The files and folders btrfs restore -vv says it restores, as paths inside the file system ("/Notes/a.txt").
QStringList parseRestored(const QString &output, const QString &target);

// The commands: `device` is the file system's file or device.
QStringList findTreesCommand(const QString &device);
// `tree` 0: the one in use. `dryRun`: only says what it would restore. `only`: just these paths (and their folders).
QStringList restoreCommand(const QString &device, quint64 tree, const QString &target, bool dryRun, const QStringList &only = {});
// The --path-regex for just these paths: each folder on the way to them, and them.
QString pathRegex(const QStringList &paths);

} // namespace btrfsrestore
