// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Copies regions of one device or file to another, optionally through zstd, hashing
// everything on the way. Used by Clone Drive, Back Up and Restore. Blocking: run it
// on a worker thread.

#include <QByteArray>
#include <QString>
#include <QVector>

#include <atomic>
#include <functional>

namespace blockcopy {

// One region: `length` bytes at `source` go to `target`. For a zstd source, `source`
// counts bytes of the uncompressed stream; for a zstd target, `target` is ignored and
// the regions are written one after another.
struct Extent {
    quint64 source = 0;
    quint64 length = 0;
    quint64 target = 0;
};

enum class Format { Raw, Zstd };

struct Options {
    int sourceFd = -1;
    Format sourceFormat = Format::Raw;
    int targetFd = -1; // -1: only read and hash (checking a backup before restoring it)
    Format targetFormat = Format::Raw;
    QVector<Extent> extents;
    int level = 3; // zstd level
};

struct Result {
    bool ok = false;
    bool cancelled = false;
    QString error;
    quint64 bytes = 0;      // uncompressed bytes copied
    QByteArray dataSha256;  // of the uncompressed data, region after region
    QByteArray fileSha256;  // of what was written; for a zstd target, the .zst file itself
};

using Progress = std::function<void(quint64 done, quint64 total)>;

quint64 totalLength(const QVector<Extent> &extents);

Result copy(const Options &options, const std::atomic<bool> &cancel, const Progress &progress);

// Reads `extents` back from `fd` (at their target offsets) after dropping the cache, and
// hashes them the same way copy() does; compare with Result::dataSha256.
Result readBack(int fd, const QVector<Extent> &extents, const std::atomic<bool> &cancel, const Progress &progress);

} // namespace blockcopy
