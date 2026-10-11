// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What a song or a film says about itself in its own headers: how long it is, its picture size,
// what it's encoded with, its title and artist, and the cover picture it carries. For Find Lost
// Files' previews, so it reads bytes that can be anything: every number is checked before it's
// used and every walk has a limit. MP4 (and MOV, 3GP, M4A), MP3, Ogg (Vorbis, Opus, Theora),
// WAV and AVI.

#include <QByteArray>
#include <QDateTime>
#include <QString>

namespace mediainfo {

struct Info {
    qint64 durationMs = -1;
    int width = 0;
    int height = 0;
    QString codec;      // "H.264", "AAC", "MP3"...
    int sampleRate = 0; // Hz
    int channels = 0;
    QString title, artist, album, year;
    QDateTime recorded; // when a film was made, from its own header (MP4, MOV: in UTC)
    QByteArray cover;   // a JPEG or PNG, when it carries one
    bool found() const { return durationMs >= 0 || width > 0 || !codec.isEmpty() || !title.isEmpty() || !cover.isEmpty(); }
};

// `type` is a filetypes number; `fileSize` is the whole file's (the data may be only its start).
Info read(int type, const QByteArray &data, quint64 fileSize);
// In plain words, a line or two: "3 min 25 s · 1920 × 1080 · H.264", "Artist: Title (Album, 2024)".
QString describe(const Info &info);

} // namespace mediainfo
