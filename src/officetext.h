// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The words in an office document, and what's in an archive, for Find Lost Files' previews.
// Word, Excel and PowerPoint files, and OpenDocument ones, are ZIP archives of XML: libarchive
// opens them in memory and the XML is read with Qt's reader (no external entities, a limit on
// internal ones). Nothing is unpacked to disk, and every read has a size limit.

#include <QByteArray>
#include <QString>
#include <QVector>

namespace officetext {

// The text of a .docx, .xlsx, .pptx, .odt, .ods or .odp (a filetypes number), up to `most`
// characters. Empty when it has none, or isn't one of those.
QString text(int type, const QByteArray &data, int most = 4000);

struct Entry {
    QString path;
    quint64 size = 0;
    bool folder = false;
};
// What a ZIP, 7z, jar, apk or epub holds, up to `most` entries. `complete` says whether it got to
// the end without trouble.
QVector<Entry> list(const QByteArray &data, int most, bool *complete = nullptr);

} // namespace officetext
