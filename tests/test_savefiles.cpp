// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Save My Files on a pretend drive: a Windows person and a Linux one (with their folders in
// German), Chrome's and Firefox's bookmarks, and a file that can't be read.

#include "testkit.h"

#include "../src/backup/savefiles.h"
#include "../src/tools/fingerprints.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <unistd.h>

namespace {

bool put(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

QByteArray read(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

const char *kChrome = R"({"roots": {
  "bookmark_bar": {"type": "folder", "name": "Bookmarks bar", "date_added": "13300000000000000", "children": [
    {"type": "url", "name": "Fish & Chips <shop>", "url": "https://example.com/?a=1&b=2", "date_added": "13300000000000000"},
    {"type": "folder", "name": "Recipes", "children": [{"type": "url", "name": "Soup", "url": "https://example.org/soup"}]}]},
  "other": {"type": "folder", "name": "Other bookmarks", "children": [{"type": "url", "name": "Elsewhere", "url": "https://example.net/"}]},
  "synced": {"type": "folder", "name": "Mobile bookmarks", "children": []}}, "version": 1})";

} // namespace

void fingerprintTests();

void saveFilesTests()
{
    fingerprintTests();
    QTemporaryDir dir;
    const QString root = dir.filePath(QStringLiteral("drive"));
    put(root + QStringLiteral("/Users/alice/Desktop/note.txt"), "on the desktop\n");
    put(root + QStringLiteral("/Users/alice/Documents/taxes/2025.txt"), QByteArray(300000, 't'));
    put(root + QStringLiteral("/Users/alice/Documents/stuck.txt"), "can't be read\n");
    put(root + QStringLiteral("/Users/alice/Pictures/beach.jpg"), QByteArray(5000, 'j'));
    put(root + QStringLiteral("/Users/alice/AppData/Local/Google/Chrome/User Data/Default/Bookmarks"), kChrome);
    put(root + QStringLiteral("/Users/Default/Documents/template.txt"), "Windows' own\n");
    put(root + QStringLiteral("/Users/Public/Documents/shared.txt"), "for everyone\n");
    put(root + QStringLiteral("/home/bob/Bilder/cat.png"), QByteArray(2000, 'p'));
    put(root + QStringLiteral("/home/bob/.config/user-dirs.dirs"), "XDG_PICTURES_DIR=\"$HOME/Bilder\"\nXDG_MUSIC_DIR=\"$HOME/Musik\"\n");
    QFile::setPermissions(root + QStringLiteral("/Users/alice/Documents/stuck.txt"), QFileDevice::Permissions());

    // Firefox's places.sqlite, made with sqlite3 the way Firefox lays it out (its own folders
    // by their GUIDs).
    const bool sqlite = !QStandardPaths::findExecutable(QStringLiteral("sqlite3")).isEmpty();
    const QString places = root + QStringLiteral("/home/bob/.mozilla/firefox/ab12.default-release/places.sqlite");
    if (sqlite) {
        QDir().mkpath(QFileInfo(places).absolutePath());
        QProcess p;
        p.start(QStringLiteral("sqlite3"), {places});
        p.write("CREATE TABLE moz_places (id INTEGER PRIMARY KEY, url LONGVARCHAR, title LONGVARCHAR);"
                "CREATE TABLE moz_bookmarks (id INTEGER PRIMARY KEY, type INTEGER, fk INTEGER DEFAULT NULL, parent INTEGER, position INTEGER,"
                " title LONGVARCHAR, dateAdded INTEGER, lastModified INTEGER, guid TEXT);"
                "INSERT INTO moz_places VALUES (1, 'https://example.com/firefox', 'x'), (2, 'https://example.org/inner', 'y'), (3, 'place:sort=8', 'z');"
                "INSERT INTO moz_bookmarks VALUES (1, 2, NULL, 0, 0, '', 0, 0, 'root________'), (2, 2, NULL, 1, 0, 'menu', 0, 0, 'menu________'),"
                " (3, 2, NULL, 1, 1, 'toolbar', 0, 0, 'toolbar_____'), (4, 2, NULL, 1, 2, 'tags', 0, 0, 'tags________'),"
                " (5, 2, NULL, 1, 3, 'unfiled', 0, 0, 'unfiled_____'), (6, 1, 1, 3, 0, 'Fox & Hounds', 1700000000000000, 0, 'aaaaaaaaaaaa'),"
                " (7, 2, NULL, 2, 0, 'Recipes', 0, 0, 'bbbbbbbbbbbb'), (8, 1, 2, 7, 0, 'Inner', 0, 0, 'cccccccccccc'),"
                " (9, 3, NULL, 2, 1, '', 0, 0, 'dddddddddddd'), (10, 1, 3, 2, 2, 'Recent', 0, 0, 'eeeeeeeeeeee');\n");
        p.closeWriteChannel();
        p.waitForFinished(30000);
    }

    const savefiles::Found found = savefiles::look(root);
    QStringList folders;
    for (const savefiles::Folder &f : found.folders)
        folders << f.user + QLatin1Char('/') + f.name + QLatin1Char('=') + QString::number(f.files);
    report(found.users == QStringList({QStringLiteral("alice"), QStringLiteral("Public"), QStringLiteral("bob")})
               && folders.contains(QLatin1String("alice/Documents=2")) && folders.contains(QLatin1String("bob/Pictures=1"))
               && folders.contains(QLatin1String("Public/Documents=1")) && !folders.join(QLatin1Char(',')).contains(QLatin1String("Default")),
           QStringLiteral("save files: each person's folders, Linux's in their own language, Windows' Default left out"), folders.join(QStringLiteral(", ")));
    QStringList browsers;
    for (const savefiles::Browser &b : found.browsers)
        browsers << b.user + QLatin1Char('/') + b.name + QLatin1Char('/') + b.profile;
    report(browsers.contains(QLatin1String("alice/Chrome/Default")) && (!sqlite || browsers.contains(QLatin1String("bob/Firefox/ab12.default-release"))),
           QStringLiteral("save files: Chrome's and Firefox's bookmarks are found"), browsers.join(QStringLiteral(", ")));

    QString error;
    const QString chrome = savefiles::chromiumToHtml(kChrome, &error);
    report(chrome.contains(QLatin1String("<DT><A HREF=\"https://example.com/?a=1&amp;b=2\" ADD_DATE=\"1655526400\">Fish &amp; Chips &lt;shop&gt;</A>"))
               && chrome.contains(QLatin1String("PERSONAL_TOOLBAR_FOLDER=\"true\">Bookmarks bar</H3>")) && chrome.contains(QLatin1String(">Recipes</H3>"))
               && chrome.contains(QLatin1String(">Other bookmarks</H3>")) && !chrome.contains(QLatin1String("Mobile bookmarks")),
           QStringLiteral("save files: Chrome's bookmarks become a bookmarks.html, escaped, its folders and dates kept"), chrome.left(600));

    const QString to = dir.filePath(QStringLiteral("usb"));
    QDir().mkpath(to);
    savefiles::Result result;
    {
        savefiles::Saver saver(found.folders, found.browsers, to);
        QObject::connect(&saver, &savefiles::Saver::finished, [&result](const savefiles::Result &r) { result = r; });
        saver.run();
    }
    const QString saved = result.folder;
    // As root (a build container) every file can be read, the "unreadable" one too.
    const bool asRoot = ::geteuid() == 0;
    if (asRoot)
        out << "SKIP  save files: the file that can't be read (root reads everything)" << Qt::endl;
    report(read(saved + QStringLiteral("/alice/Documents/taxes/2025.txt")) == QByteArray(300000, 't') && read(saved + QStringLiteral("/bob/Pictures/cat.png")).size() == 2000
               && (asRoot ? result.files == 6 && result.failed.isEmpty()
                        : result.files == 5 && result.failed.size() == 1 && result.failed.first().startsWith(QLatin1String("alice/Documents/stuck.txt"))),
           QStringLiteral("save files: everything readable is copied, and the file that can't be read is listed, not a stop"),
           QStringLiteral("%1 files, failed: %2").arg(result.files).arg(result.failed.join(QStringLiteral("; "))));
    const QString note = QString::fromUtf8(read(saved + QStringLiteral("/What was saved.txt")));
    const QString firefox = QString::fromUtf8(read(saved + QStringLiteral("/bob/Bookmarks/Firefox (ab12.default-release).html")));
    report((asRoot || (note.contains(QLatin1String("Not saved:")) && note.contains(QLatin1String("stuck.txt")))) && QFile::exists(saved + QStringLiteral("/alice/Bookmarks/Chrome (Default).html"))
               && QFile::exists(saved + QStringLiteral("/alice/Bookmarks/Chrome (Default)/Bookmarks")),
           QStringLiteral("save files: a note says what was saved and what wasn't, and the bookmarks are there twice (html, and the browser's own)"));
    if (sqlite)
        report(firefox.contains(QLatin1String("PERSONAL_TOOLBAR_FOLDER=\"true\">Bookmarks Toolbar</H3>"))
                   && firefox.contains(QLatin1String("<A HREF=\"https://example.com/firefox\" ADD_DATE=\"1700000000\">Fox &amp; Hounds</A>"))
                   && firefox.contains(QLatin1String(">Recipes</H3>")) && firefox.contains(QLatin1String("https://example.org/inner")) && firefox.contains(QLatin1String("<HR>"))
                   && !firefox.contains(QLatin1String("place:")) && !firefox.contains(QLatin1String(">tags<")),
               QStringLiteral("save files: Firefox's bookmarks through sqlite3, its own folders named, queries and tags left out"), firefox.left(700));
    QFile::setPermissions(root + QStringLiteral("/Users/alice/Documents/stuck.txt"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

namespace {

// Runs a fingerprints job to the end, on this thread.
QVector<fingerprints::Result> runJob(const QString &folder, fingerprints::Job::Mode mode, bool *ok, QString *book)
{
    fingerprints::Job job(folder, mode);
    QVector<fingerprints::Result> out;
    QObject::connect(&job, &fingerprints::Job::finished, [&](bool good, const QVector<fingerprints::Result> &results, const QString &where, const QString &) {
        *ok = good;
        out = results;
        *book = where;
    });
    job.run();
    return out;
}

} // namespace

// Are My Files Still OK?: fingerprints recorded for a folder, then each kind of change found:
// damaged (changed, its date not), edited, missing, new, unreadable; the file sha256sum reads.
void fingerprintTests()
{
    QTemporaryDir dir;
    const QString top = dir.filePath(QStringLiteral("Photos"));
    auto put = [&](const QString &name, const QByteArray &bytes) {
        QDir().mkpath(QFileInfo(top + QLatin1Char('/') + name).absolutePath());
        QFile f(top + QLatin1Char('/') + name);
        return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
    };
    const QDateTime old = QDateTime::currentDateTime().addDays(-400);
    auto age = [&](const QString &name) {
        QFile f(top + QLatin1Char('/') + name);
        f.open(QIODevice::ReadWrite);
        f.setFileTime(old, QFileDevice::FileModificationTime);
    };
    for (const char *name : {"2019/beach.jpg", "2019/cake.jpg", "letter.pdf", "notes.txt", "gone.jpg", "stuck.jpg", "odd\\name.txt"}) {
        put(QString::fromLatin1(name), QByteArray(name).repeated(3000));
        age(QString::fromLatin1(name));
    }
    bool ok = false;
    QString book;
    runJob(top, fingerprints::Job::Mode::Record, &ok, &book);
    fingerprints::Book b;
    QString error;
    report(ok && book == top + QStringLiteral("/DiskAnvil fingerprints.sha256") && fingerprints::read(book, &b, &error) && b.files.size() == 7
               && b.made.secsTo(QDateTime::currentDateTimeUtc()) < 60,
           QStringLiteral("fingerprints: recorded for every file, in the folder, with when"), QStringLiteral("%1 %2").arg(book).arg(b.files.size()));
    if (!QStandardPaths::findExecutable(QStringLiteral("sha256sum")).isEmpty()) {
        QProcess sum;
        sum.setWorkingDirectory(top);
        sum.start(QStringLiteral("sha256sum"), {QStringLiteral("--quiet"), QStringLiteral("-c"), QStringLiteral("DiskAnvil fingerprints.sha256")});
        sum.waitForFinished(30000);
        report(sum.exitCode() == 0, QStringLiteral("fingerprints: sha256sum -c checks the same file"), QString::fromLocal8Bit(sum.readAll()).left(200));
    }
    // A bit flipped with the date kept (what a failing drive does), one saved again, one deleted,
    // one added, one that can't be read.
    {
        QFile f(top + QStringLiteral("/2019/beach.jpg"));
        f.open(QIODevice::ReadWrite);
        f.seek(4000);
        f.write("X");
        f.close();
        age(QStringLiteral("2019/beach.jpg"));
    }
    put(QStringLiteral("letter.pdf"), "a newer letter");
    QFile::remove(top + QStringLiteral("/gone.jpg"));
    put(QStringLiteral("2020/new.jpg"), "new");
    QFile::setPermissions(top + QStringLiteral("/stuck.jpg"), QFileDevice::Permissions());
    const bool asRoot = ::geteuid() == 0;
    const QVector<fingerprints::Result> results = runJob(top, fingerprints::Job::Mode::Check, &ok, &book);
    QStringList seen;
    for (const fingerprints::Result &r : results)
        if (r.state != fingerprints::State::Same)
            seen << r.path + QLatin1Char('=') + fingerprints::stateName(r.state);
    seen.sort();
    QStringList want = {QStringLiteral("2019/beach.jpg=Damaged"), QStringLiteral("2020/new.jpg=New since"), QStringLiteral("gone.jpg=Missing"),
                        QStringLiteral("letter.pdf=Changed (saved since)")};
    if (!asRoot)
        want << QStringLiteral("stuck.jpg=Can't be read");
    want.sort();
    report(ok && seen == want, QStringLiteral("fingerprints: damaged, saved since, missing, new and unreadable told apart, the rest the same"),
           seen.join(QStringLiteral(", ")));
    QFile::setPermissions(top + QStringLiteral("/stuck.jpg"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    // A folder that can't be written to: they're kept in DiskAnvil's own folder.
    const QString locked = dir.filePath(QStringLiteral("Locked"));
    QDir().mkpath(locked);
    QFile f(locked + QStringLiteral("/a.txt"));
    f.open(QIODevice::WriteOnly);
    f.write("a");
    f.close();
    QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    runJob(locked, fingerprints::Job::Mode::Record, &ok, &book);
    if (!asRoot)
        report(ok && book == fingerprints::ownCopyFor(locked) && QFile::exists(book), QStringLiteral("fingerprints: a read-only folder's are kept in DiskAnvil's own folder"), book);
    QFile::remove(fingerprints::ownCopyFor(locked));
    QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    const fingerprints::Book round = fingerprints::fromText(fingerprints::toText(b));
    report(round.files.size() == b.files.size() && round.made == b.made.toUTC().addMSecs(-b.made.time().msec()),
           QStringLiteral("fingerprints: written and read back the same"));
}

