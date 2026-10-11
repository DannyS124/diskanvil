// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QVector>

// Lists the files in an ISO image (ISO 9660 with Joliet names, which xorriso and the other
// common ISO makers write) straight from the image file, so nothing gets mounted. Make a
// DiskForge Live USB uses it to copy the DiskForge Live ISO onto a FAT32 stick.
//
// Images come from anywhere, so everything read is checked: extents stay inside the image,
// names can't climb out of the folder ("..", "/"), and folders can't loop or nest forever.
namespace isofs {

struct Entry {
    QString path;        // "live/vmlinuz": no leading slash, '/' between folders
    bool isDir = false;
    quint64 offset = 0;  // where the file's data starts in the image, in bytes
    quint64 size = 0;
};

struct Listing {
    QString volumeId; // the label, as blkid reads it
    QVector<Entry> entries; // a folder comes before what's in it
    QString error;          // empty when it worked
    bool ok() const { return error.isEmpty(); }
};

Listing list(int fd);

// The entry for `path`, or nullptr.
const Entry *find(const Listing &listing, const QString &path);

} // namespace isofs
