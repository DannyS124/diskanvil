// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Repairing found files, always on a copy: what a damaged file's own structure says is wrong
// is put right where that can be done without guessing at its contents.
// - PNG: checksums put right, a missing end added, what's after the end left out.
// - JPEG: a missing end added; a lost start (its size and tables) borrowed from a photo that
//   opens, made by the same camera.
// - ZIP and the files built on it (Word, Excel and PowerPoint files, OpenDocument, EPUB): the
//   list of what's inside rebuilt from the entries themselves.
// - PDF: the table that says where each object is rebuilt from the objects themselves.
// - gzip: what still unpacks, packed again whole.
// - WAV and AVI: their sizes put right to what's there (a recording that was cut off).
// - SQLite: the rows sqlite3's .recover still finds, in a new database (sqlite3 runs as a
//   program of its own).
// - MP4 and MOV: the index that says where each frame is (lost when the recording stopped
//   before it was written), rebuilt from the frames themselves and a clip that plays, made with
//   the same camera and settings (mp4fix.h).

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace repair {

struct Result {
    bool ok = false;
    QStringList done;            // what was put right, in plain words
    QString why;                 // when it couldn't be repaired: why
    bool needsReference = false; // it can be, with a working file from the same device (referenceWanted)
};

// Whether Repair knows this kind (a filetypes number), and what it does for it, in a few words.
bool knows(int type);
QString what(int type);
// What to pick as the working file to borrow from, when one's needed; empty when none ever is.
QString referenceWanted(int type);

// Repairs the file at `in` into a new file at `out`. `reference`: a working file, when asked for.
// `progress` is told how far it is (0 to 100), and returning false from it stops it.
Result run(int type, const QString &in, const QString &out, const QString &reference = {},
           const std::function<bool(int percent)> &progress = {});

// The sqlite3 program (it reads SQLite files: browsers' history, Firefox's bookmarks), or empty
// when it isn't there. The one where DiskAnvil runs: in the Flatpak, the runtime's.
QString sqlite3();

// A PDF saved again after it was made keeps each earlier version whole inside it: a save adds
// only the changes, at the end. Where each version ends, oldest first (the last is the PDF as it
// is); one for a PDF saved once, none for what isn't a PDF.
QVector<qint64> pdfVersions(const QByteArray &pdf);

} // namespace repair
