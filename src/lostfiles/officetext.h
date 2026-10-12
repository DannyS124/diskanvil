// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The words in an office document, and what's in an archive, for Find Lost Files' previews.
// Word, Excel and PowerPoint files, and OpenDocument ones, are ZIP archives of XML: libarchive
// opens them in memory and the XML is read with Qt's reader (no external entities, a limit on
// internal ones). Nothing is unpacked to disk, and every read has a size limit.

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace officetext {

// The text of a .docx, .xlsx, .pptx, .odt, .ods or .odp (a filetypes number), up to `most`
// characters. Empty when it has none, or isn't one of those.
QString text(int type, const QByteArray &data, int most = 4000);
// What a document says about itself (docProps in Word, Excel and PowerPoint files, meta.xml in
// OpenDocument ones), a "label\tvalue" per line: Title, Author, Last changed by, Created,
// Changed, Made with, Pages, Words, Slides. Only what's there.
QStringList properties(int type, const QByteArray &data);

struct Entry {
    QString path;
    quint64 size = 0;
    bool folder = false;
};
// What a ZIP, 7z, RAR, jar, apk, epub or .tar.gz holds (or the one file a .gz packs), up to `most` entries. `complete` says whether it got to
// the end without trouble.
QVector<Entry> list(const QByteArray &data, int most, bool *complete = nullptr);

} // namespace officetext
