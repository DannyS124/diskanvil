// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Disk Cleanup's pieces that run as the user: sizing folders, emptying the user's own
// caches and trash, and asking paccache what it would free. The system's parts
// (package cache, journal) are cleaned by fixed systemd units, never from here.

#include <QString>
#include <QStringList>

#include <atomic>

namespace cleanup {

struct Result {
    quint64 freed = 0;
    int failed = 0;     // entries that couldn't be removed
    QString firstError;
};

// Space used on disk, like `du -sx`: stays on the folder's file system.
quint64 folderSize(const QString &folder);

// Deletes everything inside `folder` but keeps the folder. Never follows symlinks (a link
// is removed, what it points to is not) and never goes into another file system.
Result removeContents(const QString &folder, const std::atomic<bool> *cancel = nullptr);

// The user's trash folders: ~/.local/share/Trash and .Trash-<uid> on the given mounts.
QStringList trashFolders(const QStringList &mountPoints);
Result emptyTrash(const QString &trashFolder);

// What `paccache -r` would remove, with the same options the paccache service uses, or
// with `uninstalled`, what `paccache -ruk0` would (packages that aren't installed anymore).
// -1 when paccache isn't installed.
qint64 packageCacheReclaimable(bool uninstalled = false);
// Parses "disk space saved: 1.23 GiB" from paccache's dry run.
qint64 parsePaccacheSaved(const QString &output);

constexpr quint64 kJournalKeep = 200ull * 1024 * 1024; // what diskforge-journal-vacuum.service keeps

} // namespace cleanup
