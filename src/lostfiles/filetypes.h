// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QVector>

#include <functional>

// The kinds of file Find Lost Files can find by their contents alone, when there's no file
// system left to say where they are. Each one starts with something recognisable, and each
// one says (somewhere in itself) how long it is or where it ends: a PNG's chunks end with IEND,
// a ZIP ends with its central directory, an MP4 is a row of boxes that each give their size.
//
// Everything here reads data from a drive that may be damaged or may hold anything at all, so
// every number read is checked before it's used, and every walk has a limit.
namespace filetypes {

enum class Category : quint8 { Picture, Document, Video, Music, Archive, Other };

struct Type {
    QString id;        // also the extension: "jpg", "docx"
    Category category;
    QString name;      // "JPEG picture"
};

// Every type there is. A type's number is its place in this list.
const QVector<Type> &types();
int typeNumber(const QString &id); // -1 for an unknown one
// The type a file's name says it is, from its extension (".jpeg" too). -1 when it's none of these.
int typeForName(const QString &fileName);
// What candidates() calls a file of this type by its start: ZIP for a .docx, MP4 for a .heic.
int startType(int type);
// Whether a file of this type always starts in a way candidates() knows. An MP3 without a
// tag or an old QuickTime film doesn't, so a start that's not recognised proves nothing.
bool alwaysRecognised(int type);
QString categoryName(Category category);

// Reads a candidate file's bytes: `at` counts from its first byte. Returns how many it got,
// fewer at the end of the drive.
using Reader = std::function<qint64(quint64 at, char *buf, qint64 len)>;

// The types whose start this could be: `head` is the first bytes of a sector (at least 32).
QVector<int> candidates(const uchar *head, int len);
// Whether a sector that's none of those could start a plain text file (then measure() it as
// typeNumber("txt")): text written by a person to its end, or to zeros that fill the rest.
bool plainTextStart(const uchar *head, int len);
// What's wrong in data/filetypes.json, if anything (the tests): bad bytes, unknown types or checks.
QStringList dataProblems();

struct Measured {
    int type = -1;         // can be more exact than what was asked (a .zip that's a .docx)
    quint64 size = 0;      // 0: not one after all
    bool complete = false; // its structure checks out from start to end
};
// Works out where a file of this type that starts at `read`'s byte 0 ends.
Measured measure(int type, const Reader &read);

// Whether measure() checks a file of this type all the way through (JPEG's markers, PNG's and
// Ogg's checksums, ZIP's directory...), not just the lengths its header gives. The deep scan
// still looks inside the others: a fragmented one measured whole can hide other files.
bool checksInside(int type);

// The largest file of this type looked for.
quint64 sizeLimit(int type);

// CRC-32 as PNG, ZIP and 7z use it.
quint32 crc32(const uchar *data, qint64 len, quint32 crc = 0);

} // namespace filetypes
