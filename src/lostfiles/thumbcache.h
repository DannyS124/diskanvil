// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The small pictures Windows keeps of photos and videos it has shown in Explorer: in
// thumbcache_*.db files (Vista to 11, in %LocalAppData%\Microsoft\Windows\Explorer) and in
// Thumbs.db files next to the photos (XP, and some network folders since). They often outlive
// the photos themselves, so a deleted photo can at least be seen again. Thumbs.db keeps each
// photo's name too; thumbcache keeps only a number for it.
//
// Our own reader: each entry of a thumbcache file ("CMMM", its size) is looked through for a
// picture's start (JPEG, PNG or BMP), which works whatever Windows version wrote it; Thumbs.db is
// a compound file (ole.h), a stream per picture, named in its Catalog stream.

#include <QByteArray>
#include <QString>
#include <QVector>

namespace thumbcache {

struct Thumb {
    QString name;       // the photo's own name, when the cache keeps it (Thumbs.db)
    QString kind;       // "jpg", "png" or "bmp"
    quint64 offset = 0; // where the picture starts in the cache file (thumbcache; contiguous there)
    QByteArray data;    // the picture itself (Thumbs.db, whose streams are in pieces)
    quint64 size = 0;
};

// Whether a file of this name is one of Windows' thumbnail caches.
bool isCache(const QString &name);
// The pictures in one, `most` at most.
QVector<Thumb> read(const QByteArray &file, int most = 200000);

} // namespace thumbcache
