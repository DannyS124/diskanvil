// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// zstd's seekable format (zstd's contrib/seekable_format): a backup made of independent zstd
// frames, with a table of their sizes in a skippable frame at the end. Any zstd still reads it
// as one stream; a reader that knows the table can read from anywhere in it, unpacking one
// frame: what lets a backup be opened and looked through without restoring it.

#include <QByteArray>
#include <QPair>
#include <QVector>

#include <functional>
#include <optional>

namespace zstdseek {

inline constexpr quint32 kFrameSize = 8 * 1024 * 1024; // what each frame holds, unpacked

struct Frame {
    quint64 compressedOffset = 0;
    quint32 compressedSize = 0;
    quint64 offset = 0; // where its data is in the whole
    quint32 size = 0;
};

// The table, as the skippable frame that ends the file: each frame's (compressed, unpacked) sizes.
QByteArray table(const QVector<QPair<quint32, quint32>> &frames);

// The frames from a file's table; nothing when the file doesn't end with one. `read` gets
// `len` bytes at `offset`; `size` is the file's.
std::optional<QVector<Frame>> readTable(quint64 size, const std::function<bool(quint64 offset, char *buf, quint64 len)> &read);

} // namespace zstdseek
