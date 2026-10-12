// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Names for files found by their contents, from what's inside them, instead of "Picture
// 000042.jpg": a photo's date and camera ("2024-07-14 13.22.05 Canon EOS 80D"), a song's artist
// and title, a film's title or recording date, a document's title (or its first words), an
// email's subject. And the date the file carries, for putting found files in date order.
//
// This runs in DiskAnvil itself, not in the preview helper, so it only uses our own readers
// (EXIF, the media headers, compound files, emails, and a PDF's or Office file's own summary),
// which are written for damaged and hostile files and fuzzed; never another program's decoders.

#include <QByteArray>
#include <QString>

#include <functional>

namespace naming {

struct Inside {
    QString name;    // without the extension; empty when nothing inside says one
    qint64 when = 0; // ms since 1970: when it was taken, recorded, written or sent; 0 when not known
};

// `read(at, len)`: the file's bytes from `at` (fewer at its end).
using Read = std::function<QByteArray(quint64 at, qint64 len)>;
Inside fromContents(int type, quint64 size, const Read &read);

} // namespace naming
