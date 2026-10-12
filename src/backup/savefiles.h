// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Save My Files: the folders people keep things in (Desktop, Documents, Pictures, Music,
// Videos, Downloads), found for each person on a drive (Windows' and macOS' Users, Linux'
// home), and their browsers' bookmarks, made into a bookmarks.html any browser imports. Copied
// to another drive, going on past what can't be read and listing it at the end: the drive is
// often failing, or the PC won't start.

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>

namespace savefiles {

struct Folder {
    QString user;
    QString name;  // "Documents"
    QString path;  // where it is
    quint64 size = 0;
    int files = 0;
};

struct Browser {
    enum class Kind { Chromium, Firefox } kind = Kind::Chromium;
    QString user;
    QString name;     // "Chrome", "Edge", "Firefox"...
    QString profile;  // "Default", "Profile 1", "abcd1234.default-release"
    QString file;     // its Bookmarks (Chromium) or places.sqlite (Firefox)
};

struct Found {
    QStringList users;
    QVector<Folder> folders;
    QVector<Browser> browsers;
};

// What's on the drive mounted at `root` (sizes counted too: run it on a worker thread).
Found look(const QString &root, const std::atomic<bool> *cancel = nullptr);

// Bookmarks as a bookmarks.html (the Netscape format every browser imports).
QString chromiumToHtml(const QByteArray &bookmarksJson, QString *error);
// Firefox's, from its places.sqlite through the sqlite3 program (a copy of it, with its -wal).
QString firefoxToHtml(const QString &placesSqlite, QString *error);
// The same from what sqlite3 -json says about moz_bookmarks joined with moz_places (the tests).
QString firefoxJsonToHtml(const QByteArray &json, QString *error);

struct Result {
    int files = 0;
    quint64 bytes = 0;
    QStringList failed;  // "Documents/tax.pdf: it couldn't be read"
    QStringList bookmarks; // the bookmarks.html made
    QString folder;      // where it all went
    bool stopped = false;
};

// Copies what's picked to `to`/"Saved files <date>"/<user>/..., with a "What was saved.txt".
class Saver : public QObject
{
    Q_OBJECT
public:
    Saver(const QVector<Folder> &folders, const QVector<Browser> &browsers, const QString &to);
    void cancel() { m_cancel = true; }

public slots:
    void run();

signals:
    void progress(const QString &file, quint64 done, quint64 total);
    void finished(const savefiles::Result &result);

private:
    enum class Copied { Done, CantRead, CantWrite };
    Copied copyFile(const QString &from, const QString &to, QString *error);

    QVector<Folder> m_folders;
    QVector<Browser> m_browsers;
    QString m_to;
    std::atomic<bool> m_cancel{false};
};

} // namespace savefiles

Q_DECLARE_METATYPE(savefiles::Result)
