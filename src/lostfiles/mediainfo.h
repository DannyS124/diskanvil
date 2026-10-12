// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What a song or a film says about itself in its own headers: how long it is, its picture size,
// what it's encoded with, its title and artist, and the cover picture it carries. For Find Lost
// Files' previews, so it reads bytes that can be anything: every number is checked before it's
// used and every walk has a limit. MP4 (and MOV, 3GP, M4A), MP3, Ogg (Vorbis, Opus, Theora),
// WAV, AVI, FLAC, Matroska and WebM, and Windows Media (WMV, WMA).

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>

#include <functional>

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
    // What a phone or camera adds to its films (MP4, MOV): where (©xyz, 3GPP's loci, or Apple's
    // QuickTime keys), what made it, and when, in the time zone it was in.
    bool hasLocation = false;
    double latitude = 0, longitude = 0;
    bool hasAltitude = false;
    double altitude = 0;
    QString make, model, software;
    QDateTime made;
    bool found() const { return durationMs >= 0 || width > 0 || !codec.isEmpty() || !title.isEmpty() || !cover.isEmpty(); }
};

// `type` is a filetypes number; `fileSize` is the whole file's (the data may be only its start).
Info read(int type, const QByteArray &data, quint64 fileSize);
// In plain words, a line or two: "3 min 25 s · 1920 × 1080 · H.264", "Artist: Title (Album, 2024)".
QString describe(const Info &info);
// The rest as "label\tvalue" lines: Recorded, Phone or camera, Made with, Where, Coordinates.
QStringList facts(const Info &info);
// "+53.8008-001.5491+100.000/" (ISO 6709, how films write where they were made). False when it isn't one.
bool parseIso6709(const QString &text, double *latitude, double *longitude, double *altitude, bool *hasAltitude);
// An MP4 or MOV's moov box (where everything above is), wherever in the file it is: phones often
// write it at the end. `read` reads the file. Empty when there's none, or it's too big; `where`
// says where it starts.
using Reader = std::function<qint64(quint64 at, char *buf, qint64 len)>;
QByteArray mp4Moov(const Reader &read, quint64 size, quint64 *where = nullptr);

} // namespace mediainfo
