// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "savefiles.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QTemporaryDir>

#include <algorithm>

namespace savefiles {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("savefiles", text);
}

constexpr QDir::Filters kFiles = QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks;

// The people on a drive: Windows' and macOS' Users, Linux's home.
QStringList userFolders(const QString &root)
{
    static const QStringList skip = {QStringLiteral("default"), QStringLiteral("default user"), QStringLiteral("all users"),
                                     QStringLiteral("defaultuser0"), QStringLiteral("defaultuser100000"), QStringLiteral("shared"),
                                     QStringLiteral("lost+found")};
    QStringList out;
    for (const QString &base : {QStringLiteral("Users"), QStringLiteral("home")}) {
        const QDir dir(root + QLatin1Char('/') + base);
        for (const QFileInfo &f : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
            if (!skip.contains(f.fileName().toLower()))
                out << f.absoluteFilePath();
        }
    }
    return out;
}

// Linux's own names for the folders, which can be in the person's language
// (~/.config/user-dirs.dirs: XDG_PICTURES_DIR="$HOME/Bilder").
QMap<QString, QString> xdgFolders(const QString &home)
{
    QMap<QString, QString> out;
    QFile f(home + QStringLiteral("/.config/user-dirs.dirs"));
    if (!f.open(QIODevice::ReadOnly))
        return out;
    static const QRegularExpression line(QStringLiteral("^XDG_(DESKTOP|DOCUMENTS|PICTURES|MUSIC|VIDEOS|DOWNLOAD)_DIR=\"\\$HOME/([^\"]+)\""));
    static const QMap<QString, QString> names = {{QStringLiteral("DESKTOP"), QStringLiteral("Desktop")},   {QStringLiteral("DOCUMENTS"), QStringLiteral("Documents")},
                                                 {QStringLiteral("PICTURES"), QStringLiteral("Pictures")}, {QStringLiteral("MUSIC"), QStringLiteral("Music")},
                                                 {QStringLiteral("VIDEOS"), QStringLiteral("Videos")},     {QStringLiteral("DOWNLOAD"), QStringLiteral("Downloads")}};
    for (const QByteArray &l : f.readAll().split('\n')) {
        const QRegularExpressionMatch m = line.match(QString::fromUtf8(l.trimmed()));
        if (m.hasMatch())
            out.insert(names.value(m.captured(1)), m.captured(2));
    }
    return out;
}

void count(Folder &folder, const std::atomic<bool> *cancel)
{
    QDirIterator it(folder.path, kFiles, QDirIterator::Subdirectories);
    while (it.hasNext() && !(cancel && *cancel)) {
        it.next();
        folder.size += quint64(std::max<qint64>(0, it.fileInfo().size()));
        ++folder.files;
    }
}

void chromiumProfiles(const QString &user, const QString &name, const QString &dataDir, QVector<Browser> &out)
{
    const QDir dir(dataDir);
    if (!dir.exists())
        return;
    // Opera keeps its one profile in the folder itself; the others in Default, Profile 1...
    if (QFile::exists(dir.filePath(QStringLiteral("Bookmarks")))) {
        out.push_back({Browser::Kind::Chromium, user, name, QString(), dir.filePath(QStringLiteral("Bookmarks"))});
        return;
    }
    for (const QString &profile : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString file = dir.filePath(profile + QStringLiteral("/Bookmarks"));
        if ((profile == QLatin1String("Default") || profile.startsWith(QLatin1String("Profile "))) && QFile::exists(file))
            out.push_back({Browser::Kind::Chromium, user, name, profile, file});
    }
}

void firefoxProfiles(const QString &user, const QString &name, const QString &profilesDir, QVector<Browser> &out)
{
    const QDir dir(profilesDir);
    for (const QString &profile : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString file = dir.filePath(profile + QStringLiteral("/places.sqlite"));
        if (QFile::exists(file))
            out.push_back({Browser::Kind::Firefox, user, name, profile, file});
    }
}

void browsers(const QString &home, const QString &user, QVector<Browser> &out)
{
    // Windows, Linux (and its Flatpaks), macOS.
    const struct {
        const char *name;
        const char *path;
    } chromium[] = {
        {"Chrome", "AppData/Local/Google/Chrome/User Data"},
        {"Edge", "AppData/Local/Microsoft/Edge/User Data"},
        {"Brave", "AppData/Local/BraveSoftware/Brave-Browser/User Data"},
        {"Vivaldi", "AppData/Local/Vivaldi/User Data"},
        {"Chromium", "AppData/Local/Chromium/User Data"},
        {"Opera", "AppData/Roaming/Opera Software/Opera Stable"},
        {"Chrome", ".config/google-chrome"},
        {"Chromium", ".config/chromium"},
        {"Edge", ".config/microsoft-edge"},
        {"Brave", ".config/BraveSoftware/Brave-Browser"},
        {"Vivaldi", ".config/vivaldi"},
        {"Opera", ".config/opera"},
        {"Chrome", ".var/app/com.google.Chrome/config/google-chrome"},
        {"Chromium", ".var/app/org.chromium.Chromium/config/chromium"},
        {"Brave", ".var/app/com.brave.Browser/config/BraveSoftware/Brave-Browser"},
        {"Edge", ".var/app/com.microsoft.Edge/config/microsoft-edge"},
        {"Chrome", "Library/Application Support/Google/Chrome"},
        {"Edge", "Library/Application Support/Microsoft Edge"},
        {"Brave", "Library/Application Support/BraveSoftware/Brave-Browser"},
    };
    for (const auto &c : chromium)
        chromiumProfiles(user, QLatin1String(c.name), home + QLatin1Char('/') + QLatin1String(c.path), out);
    for (const char *path : {"AppData/Roaming/Mozilla/Firefox/Profiles", ".mozilla/firefox", "snap/firefox/common/.mozilla/firefox",
                             ".var/app/org.mozilla.firefox/.mozilla/firefox", "Library/Application Support/Firefox/Profiles"})
        firefoxProfiles(user, QStringLiteral("Firefox"), home + QLatin1Char('/') + QLatin1String(path), out);
}

QString escaped(const QString &text)
{
    return text.toHtmlEscaped();
}

QString htmlStart()
{
    return QStringLiteral("<!DOCTYPE NETSCAPE-Bookmark-file-1>\n<!-- Saved by DiskForge's Save My Files. Import it in any browser. -->\n"
                          "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n<TITLE>Bookmarks</TITLE>\n<H1>Bookmarks</H1>\n<DL><p>\n");
}

void chromiumNode(const QJsonObject &node, int depth, QString &out, bool toolbar = false)
{
    const QString indent(4 * depth, QLatin1Char(' '));
    // Its date: microseconds since 1601, as Chrome counts.
    const qint64 added = node.value(QLatin1String("date_added")).toString().toLongLong() / 1000000 - 11644473600LL;
    const QString date = added > 0 ? QStringLiteral(" ADD_DATE=\"%1\"").arg(added) : QString();
    if (node.value(QLatin1String("type")).toString() == QLatin1String("url")) {
        out += indent + QStringLiteral("<DT><A HREF=\"%1\"%2>%3</A>\n").arg(escaped(node.value(QLatin1String("url")).toString()), date,
                                                                               escaped(node.value(QLatin1String("name")).toString()));
        return;
    }
    out += indent + QStringLiteral("<DT><H3%1%2>%3</H3>\n").arg(date, toolbar ? QStringLiteral(" PERSONAL_TOOLBAR_FOLDER=\"true\"") : QString(),
                                                                escaped(node.value(QLatin1String("name")).toString()));
    out += indent + QStringLiteral("<DL><p>\n");
    for (const QJsonValue &child : node.value(QLatin1String("children")).toArray())
        chromiumNode(child.toObject(), depth + 1, out);
    out += indent + QStringLiteral("</DL><p>\n");
}

} // namespace

Found look(const QString &root, const std::atomic<bool> *cancel)
{
    Found found;
    static const QStringList standard = {QStringLiteral("Desktop"), QStringLiteral("Documents"), QStringLiteral("Pictures"), QStringLiteral("Music"),
                                         QStringLiteral("Videos"),  QStringLiteral("Downloads"), QStringLiteral("Movies"),   QStringLiteral("OneDrive")};
    for (const QString &home : userFolders(root)) {
        const QString user = QFileInfo(home).fileName();
        const QMap<QString, QString> xdg = xdgFolders(home);
        int had = 0;
        for (const QString &name : standard) {
            Folder f;
            f.user = user;
            f.name = name;
            f.path = home + QLatin1Char('/') + xdg.value(name, name);
            if (!QFileInfo(f.path).isDir() || QFileInfo(f.path).isSymLink())
                continue;
            count(f, cancel);
            if (f.files > 0) {
                found.folders << f;
                ++had;
            }
        }
        const qsizetype before = found.browsers.size();
        browsers(home, user, found.browsers);
        if (had > 0 || found.browsers.size() > before)
            found.users << user;
    }
    return found;
}

QString chromiumToHtml(const QByteArray &json, QString *error)
{
    const QJsonObject roots = QJsonDocument::fromJson(json).object().value(QLatin1String("roots")).toObject();
    if (roots.isEmpty()) {
        *error = tr("the bookmarks file couldn't be read");
        return {};
    }
    QString out = htmlStart();
    chromiumNode(roots.value(QLatin1String("bookmark_bar")).toObject(), 1, out, true);
    for (const char *other : {"other", "synced"}) {
        const QJsonObject node = roots.value(QLatin1String(other)).toObject();
        if (!node.value(QLatin1String("children")).toArray().isEmpty())
            chromiumNode(node, 1, out);
    }
    return out + QStringLiteral("</DL><p>\n");
}

QString firefoxJsonToHtml(const QByteArray &json, QString *error)
{
    struct Item {
        qint64 id, parent, position, added;
        int type;
        QString title, url, guid;
    };
    QMap<qint64, QVector<Item>> children;
    qint64 root = -1;
    for (const QJsonValue &v : QJsonDocument::fromJson(json).array()) {
        const QJsonObject o = v.toObject();
        Item i{qint64(o.value(QLatin1String("id")).toDouble()),   qint64(o.value(QLatin1String("parent")).toDouble()),
               qint64(o.value(QLatin1String("position")).toDouble()), qint64(o.value(QLatin1String("dateAdded")).toDouble()) / 1000000,
               o.value(QLatin1String("type")).toInt(),           o.value(QLatin1String("title")).toString(),
               o.value(QLatin1String("url")).toString(),         o.value(QLatin1String("guid")).toString()};
        if (i.guid == QLatin1String("root________"))
            root = i.id;
        children[i.parent] << i;
    }
    if (root < 0) {
        *error = tr("Firefox's bookmarks couldn't be read");
        return {};
    }
    for (auto &list : children)
        std::sort(list.begin(), list.end(), [](const Item &a, const Item &b) { return a.position < b.position; });
    // Its own folders' names, as Firefox shows them.
    static const QMap<QString, QString> named = {{QStringLiteral("menu________"), QStringLiteral("Bookmarks Menu")},
                                                 {QStringLiteral("toolbar_____"), QStringLiteral("Bookmarks Toolbar")},
                                                 {QStringLiteral("unfiled_____"), QStringLiteral("Other Bookmarks")},
                                                 {QStringLiteral("mobile______"), QStringLiteral("Mobile Bookmarks")}};
    QString out = htmlStart();
    std::function<void(qint64, int)> folder = [&](qint64 id, int depth) {
        const QString indent(4 * depth, QLatin1Char(' '));
        for (const Item &i : children.value(id)) {
            const QString date = i.added > 0 ? QStringLiteral(" ADD_DATE=\"%1\"").arg(i.added) : QString();
            if (i.type == 1 && !i.url.isEmpty() && !i.url.startsWith(QLatin1String("place:"))) {
                out += indent + QStringLiteral("<DT><A HREF=\"%1\"%2>%3</A>\n").arg(escaped(i.url), date, escaped(i.title.isEmpty() ? i.url : i.title));
            } else if (i.type == 2 && i.guid != QLatin1String("tags________")) {
                const bool toolbar = i.guid == QLatin1String("toolbar_____");
                out += indent + QStringLiteral("<DT><H3%1%2>%3</H3>\n").arg(date, toolbar ? QStringLiteral(" PERSONAL_TOOLBAR_FOLDER=\"true\"") : QString(),
                                                                            escaped(named.value(i.guid, i.title)));
                out += indent + QStringLiteral("<DL><p>\n");
                folder(i.id, depth + 1);
                out += indent + QStringLiteral("</DL><p>\n");
            } else if (i.type == 3) {
                out += indent + QStringLiteral("<HR>\n");
            }
        }
    };
    folder(root, 1);
    return out + QStringLiteral("</DL><p>\n");
}

QString firefoxToHtml(const QString &places, QString *error)
{
    // A copy, with its write-ahead log: what was bookmarked last may only be in that.
    QTemporaryDir dir;
    const QString copy = dir.filePath(QStringLiteral("places.sqlite"));
    if (!dir.isValid() || !QFile::copy(places, copy)) {
        *error = tr("it couldn't be copied to read it");
        return {};
    }
    if (QFile::exists(places + QStringLiteral("-wal")))
        QFile::copy(places + QStringLiteral("-wal"), copy + QStringLiteral("-wal"));
    QProcess sqlite;
    sqlite.start(QStringLiteral("sqlite3"), {QStringLiteral("-json"), copy,
                                             QStringLiteral("SELECT b.id, b.parent, b.type, b.position, b.title, b.dateAdded, b.guid, p.url "
                                                            "FROM moz_bookmarks b LEFT JOIN moz_places p ON p.id = b.fk;")});
    if (!sqlite.waitForStarted(10000)) {
        *error = tr("reading Firefox's bookmarks takes the sqlite3 program, which isn't here (its places.sqlite is copied as it is)");
        return {};
    }
    sqlite.waitForFinished(120000);
    if (sqlite.exitStatus() != QProcess::NormalExit || sqlite.exitCode() != 0) {
        *error = QString::fromLocal8Bit(sqlite.readAllStandardError()).trimmed();
        return {};
    }
    return firefoxJsonToHtml(sqlite.readAllStandardOutput(), error);
}

Saver::Saver(const QVector<Folder> &folders, const QVector<Browser> &browsers, const QString &to)
    : m_folders(folders)
    , m_browsers(browsers)
    , m_to(to)
{
    qRegisterMetaType<savefiles::Result>();
}

Saver::Copied Saver::copyFile(const QString &from, const QString &to, QString *error)
{
    QFile in(from);
    if (!in.open(QIODevice::ReadOnly)) {
        *error = tr("it couldn't be opened");
        return Copied::CantRead;
    }
    QFile out(to);
    if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        *error = tr("it couldn't be written there: %1").arg(out.errorString());
        return Copied::CantWrite;
    }
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    while (true) {
        const qint64 n = in.read(buffer.data(), buffer.size());
        if (n < 0) {
            out.remove();
            *error = tr("it couldn't be read (a bad spot on the drive?)");
            return Copied::CantRead;
        }
        if (n == 0)
            break;
        if (out.write(buffer.constData(), n) != n) {
            *error = tr("writing it failed: %1").arg(out.errorString());
            out.remove();
            return Copied::CantWrite;
        }
    }
    out.setFileTime(QFileInfo(in).lastModified(), QFileDevice::FileModificationTime);
    return Copied::Done;
}

void Saver::run()
{
    Result r;
    r.folder = m_to + QStringLiteral("/Saved files ") + QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH.mm"));
    QDir().mkpath(r.folder);
    const QString fs = QStorageInfo(m_to).fileSystemType().toLower();
    const bool fat32 = fs == QLatin1String("vfat") || fs == QLatin1String("msdos") || fs == QLatin1String("fat32");
    quint64 total = 0;
    for (const Folder &f : std::as_const(m_folders))
        total += f.size;
    QElapsedTimer clock;
    clock.start();
    bool full = false;
    for (const Folder &folder : std::as_const(m_folders)) {
        const QString target = r.folder + QLatin1Char('/') + folder.user + QLatin1Char('/') + folder.name;
        QDirIterator it(folder.path, kFiles, QDirIterator::Subdirectories);
        while (it.hasNext() && !m_cancel && !full) {
            const QString file = it.next();
            const QString relative = QDir(folder.path).relativeFilePath(file);
            const QString to = target + QLatin1Char('/') + relative;
            const QString shown = folder.user + QLatin1Char('/') + folder.name + QLatin1Char('/') + relative;
            const quint64 size = quint64(std::max<qint64>(0, it.fileInfo().size()));
            QDir().mkpath(QFileInfo(to).absolutePath());
            QString error;
            if (fat32 && size >= 4ull * 1024 * 1024 * 1024) {
                r.failed << shown + QStringLiteral(": ") + tr("too big for a FAT32 drive (4 GB is the most)");
                continue;
            }
            const Copied copied = copyFile(file, to, &error);
            if (copied == Copied::Done) {
                ++r.files;
                r.bytes += size;
            } else {
                r.failed << shown + QStringLiteral(": ") + error;
                // A full drive stops it all: everything after would fail the same way.
                full = copied == Copied::CantWrite && QStorageInfo(m_to).bytesAvailable() < qint64(size) + 1024 * 1024;
            }
            if (clock.elapsed() > 150) {
                emit progress(shown, r.bytes, total);
                clock.restart();
            }
        }
    }
    // Bookmarks: as bookmarks.html, and the browser's own file next to it.
    for (const Browser &b : std::as_const(m_browsers)) {
        if (m_cancel || full)
            break;
        const QString base = b.name + (b.profile.isEmpty() ? QString() : QStringLiteral(" (") + b.profile + QLatin1Char(')'));
        const QString folder = r.folder + QLatin1Char('/') + b.user + QStringLiteral("/Bookmarks");
        QDir().mkpath(folder + QLatin1Char('/') + base);
        QString error;
        copyFile(b.file, folder + QLatin1Char('/') + base + QLatin1Char('/') + QFileInfo(b.file).fileName(), &error);
        QString html;
        if (b.kind == Browser::Kind::Chromium) {
            QFile f(b.file);
            html = f.open(QIODevice::ReadOnly) ? chromiumToHtml(f.readAll(), &error) : QString();
            if (html.isEmpty() && error.isEmpty())
                error = tr("it couldn't be read");
        } else {
            html = firefoxToHtml(b.file, &error);
        }
        QFile out(folder + QLatin1Char('/') + base + QStringLiteral(".html"));
        if (!html.isEmpty() && out.open(QIODevice::WriteOnly) && out.write(html.toUtf8()) > 0)
            r.bookmarks << out.fileName();
        else
            r.failed << b.user + QStringLiteral("/Bookmarks/") + base + QStringLiteral(": ") + (error.isEmpty() ? out.errorString() : error);
    }
    r.stopped = m_cancel;
    // What was saved, in a note next to it.
    QFile note(r.folder + QStringLiteral("/What was saved.txt"));
    if (note.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QStringList lines{tr("Saved by DiskForge's Save My Files, %1.").arg(QDateTime::currentDateTime().toString(Qt::TextDate)), QString()};
        for (const Folder &f : std::as_const(m_folders))
            lines << tr("%1's %2, from %3").arg(f.user, f.name, QDir::toNativeSeparators(f.path));
        lines << QString() << tr("%n file(s) copied.", nullptr, r.files);
        if (!r.bookmarks.isEmpty())
            lines << QString() << tr("Bookmarks: the .html files in each person's Bookmarks folder. In any browser: Import bookmarks, "
                                     "from an HTML file.");
        if (!r.failed.isEmpty())
            lines << QString() << tr("Not saved:") << r.failed;
        if (r.stopped)
            lines << QString() << tr("Stopped before the end.");
        if (full)
            lines << QString() << tr("The drive it was copying to filled up.");
        note.write(lines.join(QLatin1Char('\n')).toUtf8() + '\n');
    }
    emit finished(r);
}

} // namespace savefiles
