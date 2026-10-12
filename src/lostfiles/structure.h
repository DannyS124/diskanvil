// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What a file is made of, part by part: a JPEG's markers, a PNG's chunks, an MP4's boxes, a
// ZIP's entries, a PDF's objects, and so on, each with where it is, how big, and whether it
// checks out (a checksum that adds up, a size that fits). For Find Lost Files' viewer, next to
// the bytes, so a damaged file shows where it's damaged. The file can be anything a drive held:
// every number is checked before it's used, and the list has a limit.

#include <QByteArray>
#include <QString>
#include <QVector>

namespace structure {

struct Part {
    quint64 at = 0;   // where it starts in the file
    quint64 size = 0; // how many bytes
    int depth = 0;    // inside the part before it at depth - 1
    QString name;     // "APP1 (EXIF)", "IDAT", "moov"
    QString detail;   // what's known about it, in a few words
    QString problem;  // empty when it checks out; what's wrong otherwise
};

constexpr int kMostParts = 20000;

// `type` is a filetypes number (-1: not known). `data` and `size`: the whole file.
QVector<Part> parts(int type, const uchar *data, quint64 size);
// The same for a file in memory.
inline QVector<Part> parts(int type, const QByteArray &file)
{
    return parts(type, reinterpret_cast<const uchar *>(file.constData()), quint64(file.size()));
}

} // namespace structure
