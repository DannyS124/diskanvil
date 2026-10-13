// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Which files did the bad spots hit? After a Rescue Copy, the copy's file systems (FAT, exFAT,
// NTFS: Find Lost Files' readers) are read, and each file's place is laid over the rescue map: the
// files with parts that couldn't be read, and how much of each. The rest came off whole.

#include "lostsource.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>

class RescueMap;

namespace hitfiles {

struct Hit {
    QString path;           // "Photos/2024/beach.jpg", under its volume's label
    quint64 size = 0;
    quint64 missing = 0;    // bytes of it that weren't read (blank in the copy)
    bool deleted = false;   // a deleted file the file system still lists
    bool guessed = false;   // where its pieces are is worked out, not known (a deleted FAT file)
    bool packed = false;    // Windows keeps it compressed: what's missing is of the packed bytes
};

struct Report {
    QVector<Hit> hits;        // most missing first
    int files = 0;            // files looked at (not deleted ones)
    QStringList volumes;      // the file systems read: "STICK (FAT)"
    QStringList unread;       // the parts with bad spots no file system was read on: "partition 2 (3.2 MB missing)"
    quint64 missing = 0;      // every byte not read
    quint64 inFiles = 0;      // of those, in files listed
};

// The copy (an image of the drive or of one partition) and its map. `progress` gets a line now
// and then; `cancel` stops it.
Report find(lost::Source &copy, const RescueMap &map, const std::atomic<bool> &cancel,
            const std::function<void(const QString &)> &progress = {});

// As plain text, to save next to the copy.
QString asText(const Report &report, const QString &copyName);

} // namespace hitfiles
