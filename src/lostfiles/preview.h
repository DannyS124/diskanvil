// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Find Lost Files' previews: a found file's picture (a photo, a PDF's first page, a song's cover,
// a film's first frame) and its words (a document's text, what an archive holds, a song's or
// film's details).
//
// They're made in a helper process, `diskforge --preview-helper`, never in DiskForge itself:
// what's decoded is whatever a damaged drive holds, with other people's decoders. The helper is
// only ever given the one file's bytes, never the drive. On Linux it runs in a bubblewrap
// sandbox when bwrap is there (no network, no home folder, nothing it can write to but its own
// /tmp), with limits on its memory. Whatever system it's on, the window gives up on it after a
// few seconds and starts a new one, so a file that hangs or crashes a decoder only costs its
// own preview.

#include <QByteArray>
#include <QImage>
#include <QString>

class QIODevice;

namespace preview {

struct Request {
    int type = -1;     // filetypes number, -1 when its kind isn't known
    QString name;
    quint64 size = 0;  // the whole file's; `data` can be just its start
    int pixels = 0;    // the picture's longest side
    QByteArray data;
};

struct Result {
    QImage image;
    QString text;
    QStringList facts; // what the file says about itself, a "label\tvalue" per line
};

// What the helper does with one request.
Result make(const Request &request);
// The helper itself: answers requests on its input until that closes.
int runHelper();

// How requests and answers travel: a length, then the rest. False when there's nothing (more)
// to read, or it doesn't make sense.
// `msecs`: how long to wait for it, -1 for as long as it takes.
void write(QIODevice &to, const Request &request);
bool read(QIODevice &from, Request &request, int msecs = -1);
void write(QIODevice &to, const Result &result);
bool read(QIODevice &from, Result &result, int msecs = -1);

// How much of a file is handed to the helper: all of it up to this, or the start of a bigger one.
constexpr qint64 kMostData = 64 * 1024 * 1024;
// A PDF says where its pages are at its very end, so a bigger one (a scanned book) is handed
// over whole, up to this.
constexpr qint64 kMostPdfData = 512 * 1024 * 1024;

} // namespace preview
