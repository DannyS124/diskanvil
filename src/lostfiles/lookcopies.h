// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Copies of found files made only to look at them: in the viewer, in their own program, or
// dragged out to a folder. They never go on the drive the files are being found on, since
// writing there can overwrite the very files being looked for: they're kept in the computer's
// memory when they fit, otherwise in the cache folder when that's on another drive, or in a
// folder picked for them. They're deleted when Find Lost Files closes.

#include "lostscan.h"

#include <QHash>
#include <QString>

#include <functional>
#include <memory>

class QTemporaryDir;

namespace lost {

class LookCopies
{
public:
    // sourceDisk: the drive the files are on ("/dev/sdb"), empty for a disk image file.
    explicit LookCopies(QString sourceDisk);
    ~LookCopies();
    LookCopies(const LookCopies &) = delete;
    LookCopies &operator=(const LookCopies &) = delete;

    // Copies of this size go here, or nowhere (empty) and `why` says so.
    QString folderFor(quint64 size, QString *why);
    // A folder picked for them, when neither memory nor the cache folder will do. False, with
    // why, when it's on the drive the files are on.
    bool setFolder(const QString &folder, QString *why);
    // A copy of the file, under its own name (the same copy again when it was made before).
    // Empty, with why, when there's nowhere for it or it couldn't be read. `progress` is
    // called as it goes, and returning false from it stops it.
    QString make(const std::shared_ptr<Source> &source, const Found &f, int index, QString *why,
                 const std::function<bool(quint64 done, quint64 total)> &progress = {});
    // The copy made of that file, if there is one.
    QString made(int index) const { return m_made.value(index); }

    // Whether it's safe to open in the program the computer has for it: documents, pictures,
    // films, music and archives are; programs, scripts and anything unknown aren't, since
    // opening them could run them.
    static bool safeToOpen(const QString &name);

private:
    QString m_disk;
    std::unique_ptr<QTemporaryDir> m_memory, m_cache, m_picked;
    QHash<int, QString> m_made;
};

} // namespace lost
