// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Putting fragmented files back together. A file system writes a big file wherever it has room,
// so after a format or a delete its pieces can be spread over the drive, and a file found by its
// start has someone else's data after its first piece. For kinds whose contents say what has to
// come where, the rest is found: films (MP4, MOV) by their index, which lists every frame's place
// and size, and JPEG photos by decoding their picture data until it ends exactly where it should.
// Our own code, like the rest of Find Lost Files; it only ever reads.

#include "lostscan.h"

#include <atomic>
#include <functional>

namespace lost::fragments {

struct Context {
    Source &source;
    quint64 clusterSize = 4096;
    // Whether the cluster at `at` belongs to another file that was found whole (or that a file
    // system lists): not one of this file's pieces.
    std::function<bool(quint64 at)> taken;
    // Where MP4 indexes ("moov" boxes) start on the source, from noteIndexes().
    QVector<quint64> indexes;
    const std::atomic<bool> &cancel;
};

// Notes where MP4 indexes start in `data` (which is at `at` on the source).
void noteIndexes(const uchar *data, qsizetype len, quint64 at, QVector<quint64> *out);

// A film found by its start (MP4, MOV, 3GP, M4A): checked frame by frame against its index and,
// where the frames stop being where the index says, carried on from the piece where they are.
// True when it changed `file` (its pieces, and whether it's whole).
bool mp4(Context &c, Found &file);

// A JPEG found by its start whose picture data breaks off: carried on from the piece where it
// decodes on to its end. True when it changed `file`.
bool jpeg(Context &c, Found &file);

} // namespace lost::fragments
