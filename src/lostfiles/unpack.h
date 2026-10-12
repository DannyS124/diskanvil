// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Windows' ways of keeping a file compressed, unpacked by our own code (written from Microsoft's
// published descriptions of the formats: [MS-XCA] for XPRESS and LZNT1, the LZX one for LZX):
// - NTFS compression ("Compress contents to save disk space", compact.exe /c): LZNT1, in units of
//   16 clusters, each kept packed only when that saves at least a cluster. A packed unit's
//   clusters are followed by a hole for the rest of it; a unit kept as it is has no hole.
// - compact.exe /exe and CompactOS (Windows' "WOF"): the file's own data is left empty, and the
//   packed bytes are in a stream of their own, WofCompressedData. It starts with a table of where
//   each chunk starts, then the chunks, each packed on its own: XPRESS in 4, 8 or 16 KB chunks, or
//   LZX in 32 KB ones. A chunk that packing didn't make smaller is kept as it is.

#include "lostsource.h"

#include <QByteArray>
#include <QVector>

#include <memory>

namespace unpack {

enum class Format : quint8 { None, Lznt1, Xpress4k, Lzx, Xpress8k, Xpress16k };

// WOF's number for a format (FILE_PROVIDER_COMPRESSION_*: 0 XPRESS 4K, 1 LZX, 2 XPRESS 8K,
// 3 XPRESS 16K), None for any other.
Format wofFormat(quint32 algorithm);
// How much of the file each of a WOF format's chunks holds.
quint32 chunkSize(Format format);
// "LZX", "XPRESS (4 KB)"... for the logs and the notes.
QString name(Format format);

struct Packing {
    Format format = Format::None;
    quint32 unit = 0;   // NTFS compression: the bytes in a compression unit
    quint64 stored = 0; // WOF: the size of the WofCompressedData stream
    bool packed() const { return format != Format::None; }
};

// A packed file's bytes, unpacked as they're read (safe from several threads at once). Where the
// packed bytes are: `stored`, on `source`. For NTFS compression, that's the file's own pieces,
// its holes included, as far as it has clusters for (whole units); for WOF, the
// WofCompressedData stream's pieces, or `inRecord` when the stream is small enough to be kept in
// the file's record. `size` is the file's. A part that can't be unpacked reads as failed.
std::unique_ptr<lost::Layer> layer(std::shared_ptr<lost::Source> source, QVector<lost::Extent> stored, QByteArray inRecord,
                                   const Packing &packing, quint64 size);

// The formats themselves. Each unpacks `in` into exactly `outSize` bytes; false when it can't
// (damaged, or not that format at all).
bool xpress(const uchar *in, qsizetype inSize, uchar *out, qsizetype outSize);
bool lzx(const uchar *in, qsizetype inSize, uchar *out, qsizetype outSize);
// One NTFS compression unit (what its clusters hold, up to the hole): zeros after what it says.
bool lznt1(const uchar *in, qsizetype inSize, uchar *out, qsizetype outSize);

} // namespace unpack
