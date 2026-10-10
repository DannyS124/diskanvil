// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lostscan.h"

#include <QString>
#include <QVector>

#include <atomic>
#include <functional>

// Find Lost Files' own readers for the file systems of USB sticks, memory cards and Windows:
// FAT (12, 16 and 32), exFAT and NTFS (lostntfs.cpp). They list what a file system still knows about, deleted files and
// folders included, with their names. Our own code: The Sleuth Kit's licence doesn't mix with
// GPL-3. Everything read from the drive is checked before it's used, every walk has a limit,
// and nothing is ever written.
//
// What deleting leaves behind:
// - FAT marks the folder entry deleted (its first byte becomes 0xE5) and empties the file's
//   chain in the table. The name stays (the long name whole, the short one minus its first
//   letter, which the long name's checksum gives back) and so do the size, the dates and
//   where it started. Where the rest was isn't kept: it's taken to follow on from the start,
//   skipping what's in use now, which is right for most files on a stick.
// - exFAT clears one bit in each of the file's entries. Name, size and start stay, and a file
//   in one piece says so, so for those nothing is guessed at all.
namespace lost {

// Where file systems may start on the source: its start (a partition, an image of one, or a
// stick with no partition table), and on a drive, every partition in its MBR (logical ones
// too) or GPT.
QVector<Extent> partitionsOf(Source &source);

using Batch = std::function<void(QVector<Found> &&files)>;
// Lists what a FAT or exFAT file system at `where` knows about, in batches. Folders start with
// the volume's label (or its type and place, when it has none). False when there's neither
// there; `volume` says what was.
bool readFileSystem(Source &source, const Extent &where, const Batch &out, const std::atomic<bool> &cancel, Volume *volume);
// The NTFS part of that (lostntfs.cpp).
bool readNtfs(Source &source, const Extent &where, const Batch &out, const std::atomic<bool> &cancel, Volume *volume);

// How whole a deleted file looks from what's in it: the start against what its name says it
// is, and when `full`, the whole structure (the deep scan's checks). Sets condition and checked.
void checkContents(const std::shared_ptr<Source> &source, Found &file, bool full);

// Up to this size a deleted file's whole structure is checked.
constexpr quint64 kFullCheckLimit = 32 * 1024 * 1024;

} // namespace lost
