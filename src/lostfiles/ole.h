// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Office 97-2003 documents (.doc, .xls, .ppt) and Outlook emails (.msg) are compound files: a
// little FAT file system of their own, with folders ("storages") and files ("streams") inside.
// Our own reader of them, for Find Lost Files' previews and viewer: what's in one, the words of a
// Word document, an Excel sheet's cells, a PowerPoint's slides, an email, and what each says
// about itself. Written from Microsoft's published specifications ([MS-CFB], [MS-OLEPS], [MS-DOC],
// [MS-XLS], [MS-PPT], [MS-OXMSG]); found files can hold anything, so every number is checked
// before it's used and every chain and walk has a limit.

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ole {

class File
{
public:
    explicit File(const QByteArray &data);
    bool valid() const { return m_valid; }

    struct Entry {
        QString path; // "Folder/Name", without the root
        bool storage = false;
        quint32 start = 0;
        quint64 size = 0;
    };
    const QVector<Entry> &entries() const { return m_entries; }
    bool has(const QString &path) const;
    // A stream's bytes, up to `most`; empty when it isn't there.
    QByteArray stream(const QString &path, qint64 most = 64 * 1024 * 1024) const;

private:
    QByteArray chain(quint32 start, quint64 size, bool mini, qint64 most) const;

    QByteArray m_data;
    bool m_valid = false;
    int m_shift = 9;
    quint32 m_miniCutoff = 4096;
    QVector<quint32> m_fat, m_miniFat;
    QByteArray m_miniStream;
    QVector<Entry> m_entries;
};

// What kind of compound file it is, by what's inside: "doc", "xls", "ppt", "msg" or "" (other).
QString kind(const File &file);
// Its words, up to `most` characters: a document's text, a workbook's cells sheet by sheet, the
// slides' text, an email with its headers. Empty when there are none or it's locked.
QString text(const File &file, int most = 4000);
// What it says about itself, a "label\tvalue" per line (title, author, created, the email's
// sender, recipients, subject...). Only what's there.
QStringList properties(const File &file);
// Whether it carries macros (VBA, or Excel 4.0 macro sheets): such files aren't opened in their
// own program, since opening them could run them.
bool hasMacros(const File &file);
// Whether it's password-protected (its words can't be read then).
bool locked(const File &file);

// What an Office file's summary says, as values (the same as properties() shows), and for an
// Outlook email, its subject and when it was sent.
struct Summary {
    QString title, subject, author;
    QDateTime created, changed;
};
Summary summary(const File &file);

} // namespace ole
