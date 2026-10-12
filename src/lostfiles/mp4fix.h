// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Rebuilding a film's index (the "moov" box of an MP4 or MOV), which a camera or phone writes
// only when the recording stops: a battery that ran out or a card pulled out leaves the frames
// without it, and nothing plays them. The idea is untrunc's (it's GPL-2-only, so none of its
// code is here): a clip that plays, made with the same camera and settings, says how the frames
// are stored (the codec and its settings, how long each frame lasts), and the frames themselves
// are found one by one in the broken file:
// - H.264 and H.265: each frame is NAL units, each with its length in front; a frame starts
//   with the first slice of a picture, keyframes are the IDR (and IRAP) ones.
// - Motion JPEG: each frame is a JPEG.
// - Sound stored as PCM (most cameras that write MOV): the bytes between frames, cut into the
//   working clip's sample size. Packed sound (AAC) can't be cut up without decoding it, so
//   it's left out and the film plays silent; that's said.
// The new file is the working clip's beginning (ftyp), a new index, and the broken file's
// frames as they were.

#include <QIODevice>
#include <QStringList>

#include <functional>

namespace mp4fix {

struct Fixed {
    bool ok = false;
    QStringList done;
    QString why;
};

Fixed rebuild(const uchar *broken, quint64 brokenSize, const uchar *reference, quint64 referenceSize, QIODevice &out,
              const std::function<bool(int percent)> &progress = {});

} // namespace mp4fix
