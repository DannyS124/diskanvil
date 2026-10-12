// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "activity.h"

#include "reghive.h"
#include "repair.h"
#include "shelllink.h"
#include "winstartup.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QUrl>
#include <QHash>
#include <QXmlStreamReader>
#include <QVersionNumber>
#include <QtEndian>

#include <algorithm>

namespace activity {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("activity", text);
}

bool stopped(const std::atomic<bool> *cancel)
{
    return cancel && *cancel;
}

// A little-endian field from a registry value's bytes, or 0 past its end.
quint32 le32(const QByteArray &b, int at)
{
    return at >= 0 && at + 4 <= b.size() ? qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(b.constData()) + at) : 0;
}
quint64 le64(const QByteArray &b, int at)
{
    return at >= 0 && at + 8 <= b.size() ? qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(b.constData()) + at) : 0;
}

// A UTF-16 string from a value's bytes, to its first double zero.
QString utf16(const QByteArray &b, int at = 0)
{
    QString out;
    for (int i = at; i + 1 < b.size(); i += 2) {
        const char16_t c = char16_t(quint8(b[i]) | quint8(b[i + 1]) << 8);
        if (c == 0)
            break;
        out += QChar(c);
    }
    return out;
}

// UserAssist writes many paths from a known folder's ID: "{6D809377-...}\Notepad++\notepad++.exe".
QString knownFolders(const QString &path)
{
    static const QVector<std::pair<const char *, const char *>> folders = {
        {"{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}", "C:\\Windows\\System32"},
        {"{D65231B0-B2F1-4857-A4CE-A8E7C6EA7D27}", "C:\\Windows\\SysWOW64"},
        {"{F38BF404-1D43-42F2-9305-67DE0B28FC23}", "C:\\Windows"},
        {"{6D809377-6AF0-444B-8957-A3773F02200E}", "C:\\Program Files"},
        {"{7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E}", "C:\\Program Files (x86)"},
        {"{F7F1ED05-9F6D-47D2-AAAE-29D317C6F066}", "C:\\Program Files\\Common Files"},
        {"{0139D44E-6AFE-49F2-8690-3DAFCAE6FFB8}", "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs"},
        {"{A77F5D77-2E2B-44C3-A6A2-ABA601054A51}", "%AppData%\\Microsoft\\Windows\\Start Menu\\Programs"},
        {"{9E3995AB-1F9C-4F13-B827-48B24B6C7174}", "%AppData%\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned"},
        {"{B4BFCC3A-DB2C-424C-B029-7FE99A87C641}", "%UserProfile%\\Desktop"},
        {"{374DE290-123F-4565-9164-39C4925E467B}", "%UserProfile%\\Downloads"},
        {"{FDD39AD0-238F-46AF-ADB4-6C85480369C7}", "%UserProfile%\\Documents"},
    };
    for (const auto &[id, folder] : folders) {
        if (path.startsWith(QLatin1String(id), Qt::CaseInsensitive))
            return QLatin1String(folder) + path.mid(int(qstrlen(id)));
    }
    return path;
}

// --- The overview.

void readOverview(const startup::Machine &m, Report &report)
{
    Overview &o = report.overview;
    const QString nt = QStringLiteral("Microsoft\\Windows NT\\CurrentVersion");
    o.product = m.software->text(nt, QStringLiteral("ProductName"));
    o.release = m.software->text(nt, QStringLiteral("DisplayVersion"));
    if (o.release.isEmpty())
        o.release = m.software->text(nt, QStringLiteral("ReleaseId"));
    o.build = m.software->text(nt, QStringLiteral("CurrentBuildNumber"));
    // Windows 11 keeps the name "Windows 10" in the registry; the build number tells them apart.
    if (o.product.contains(QLatin1String("Windows 10")) && o.build.toInt() >= 22000)
        o.product.replace(QLatin1String("Windows 10"), QLatin1String("Windows 11"));
    o.owner = m.software->text(nt, QStringLiteral("RegisteredOwner"));
    o.organisation = m.software->text(nt, QStringLiteral("RegisteredOrganization"));
    if (const auto v = m.software->value(nt, QStringLiteral("InstallTime")); v && v->data.size() == 8)
        o.installed = fromFileTime(le64(v->data, 0));
    else if (const auto d = m.software->value(nt, QStringLiteral("InstallDate")); d)
        o.installed = QDateTime::fromSecsSinceEpoch(qint64(d->number()), QTimeZone::UTC);
    const QString control = QStringLiteral("%1\\Control").arg(m.controlSet);
    o.computerName = m.system->text(control + QStringLiteral("\\ComputerName\\ComputerName"), QStringLiteral("ComputerName"));
    o.timeZone = m.system->text(control + QStringLiteral("\\TimeZoneInformation"), QStringLiteral("TimeZoneKeyName"));
    if (const auto v = m.system->value(control + QStringLiteral("\\Windows"), QStringLiteral("ShutdownTime")); v && v->data.size() == 8)
        o.lastShutdown = fromFileTime(le64(v->data, 0));
}

// --- USB sticks and drives (SYSTEM\...\Enum\USBSTOR).

void readUsb(const startup::Machine &m, Report &report, const std::atomic<bool> *cancel)
{
    const QString enumPath = QStringLiteral("%1\\Enum\\USBSTOR").arg(m.controlSet);
    // The times are kept in a property set, by a GUID, each a filetime.
    const QString firstGuid = QStringLiteral("{83da6326-97a6-4088-9453-a1923f573b29}");
    for (const QString &device : m.system->subkeys(enumPath)) {
        if (stopped(cancel))
            return;
        for (const QString &serial : m.system->subkeys(enumPath + QLatin1Char('\\') + device)) {
            const QString instance = enumPath + QLatin1Char('\\') + device + QLatin1Char('\\') + serial;
            QString name = m.system->text(instance, QStringLiteral("FriendlyName"));
            if (name.isEmpty())
                name = tidyUsbName(device);
            // A serial whose second character is "&" was made up by Windows: not the stick's own.
            const QString realSerial = serial.size() > 1 && serial[1] == QLatin1Char('&') ? QString() : serial.section(QLatin1Char('&'), 0, 0);
            // Windows 8 and later keep it as the key's own value; Windows 7 one level down, as "Data".
            const auto time = [&](const QString &prop) {
                const QString key = instance + QStringLiteral("\\Properties\\") + firstGuid + QLatin1Char('\\') + prop;
                auto v = m.system->value(key, QString());
                if (!v || v->data.size() < 8)
                    v = m.system->value(key + QStringLiteral("\\00000000"), QStringLiteral("Data"));
                return v && v->data.size() >= 8 ? fromFileTime(le64(v->data, 0)) : QDateTime();
            };
            const QDateTime first = time(QStringLiteral("0064")), last = time(QStringLiteral("0066"));
            QStringList detail;
            if (!realSerial.isEmpty())
                detail << tr("serial %1").arg(realSerial);
            if (first.isValid())
                detail << tr("first plugged in %1").arg(QLocale().toString(first.toLocalTime(), QLocale::ShortFormat));
            Event e;
            e.kind = Kind::UsbDevice;
            e.when = last.isValid() ? last : first;
            e.what = tr("USB drive: %1").arg(name);
            e.detail = detail.join(QStringLiteral(", "));
            e.source = tr("the record of USB storage in the registry");
            report.events << e;
        }
    }
}

// --- Programs run (each user's UserAssist).

void readPrograms(const startup::Machine::User &user, Report &report, const std::atomic<bool> *cancel)
{
    const QString userAssist = QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\UserAssist");
    for (const QString &guid : user.keys->subkeys(userAssist)) {
        if (stopped(cancel))
            return;
        const QString count = userAssist + QLatin1Char('\\') + guid + QStringLiteral("\\Count");
        for (const reg::Value &v : user.keys->values(count)) {
            const QString name = knownFolders(rot13(v.name));
            // Programs and the shortcuts that start them (the list has Windows' own bookkeeping too).
            if (!name.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive) && !name.endsWith(QLatin1String(".lnk"), Qt::CaseInsensitive))
                continue;
            const quint32 runs = le32(v.data, 4);
            const QDateTime when = v.data.size() >= 68 ? fromFileTime(le64(v.data, 60)) : QDateTime();
            Event e;
            e.kind = Kind::ProgramRun;
            e.when = when;
            e.what = tr("Ran %1").arg(QFileInfo(name.section(QLatin1Char('\\'), -1)).fileName());
            QStringList detail{name};
            if (runs > 0)
                detail << QCoreApplication::translate("activity", "%n time(s)", nullptr, int(runs));
            e.detail = detail.join(QStringLiteral(" · "));
            e.user = user.name;
            e.source = tr("the list of programs run (UserAssist)");
            report.events << e;
        }
    }
    // The Run box's history, and programs the Start menu's search ran.
    const QString runMru = QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\RunMRU");
    for (const reg::Value &v : user.keys->values(runMru)) {
        if (v.name == QLatin1String("MRUList") || v.text().isEmpty())
            continue;
        Event e;
        e.kind = Kind::Typed;
        e.what = tr("Typed in the Run box: %1").arg(v.text().section(QLatin1Char('\\'), 0, 0).trimmed());
        e.user = user.name;
        e.source = tr("the Run box's history");
        report.events << e;
    }
}

// --- Files, folders, searches and typed paths (each user's Explorer).

void readUserTrail(const startup::Machine::User &user, Report &report, const std::atomic<bool> *cancel)
{
    const QString explorer = QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer");
    // Recently opened files: each is in the whole list and again under its extension, so once each.
    const QString recent = explorer + QStringLiteral("\\RecentDocs");
    QSet<QString> seen;
    for (const QString &ext : QStringList{QString()} + user.keys->subkeys(recent)) {
        if (stopped(cancel))
            return;
        const QString key = ext.isEmpty() ? recent : recent + QLatin1Char('\\') + ext;
        for (const reg::Value &v : user.keys->values(key)) {
            const QString name = utf16(v.data);
            if (v.name == QLatin1String("MRUListEx") || name.isEmpty() || name.size() > 260 || seen.contains(name.toLower()))
                continue;
            seen.insert(name.toLower());
            Event e;
            e.kind = Kind::FileOpened;
            e.what = tr("Opened %1").arg(name);
            e.user = user.name;
            e.source = tr("the list of recent files");
            report.events << e;
        }
    }
    // What was typed into Explorer's address bar.
    for (const reg::Value &v : user.keys->values(explorer + QStringLiteral("\\TypedPaths"))) {
        if (v.text().isEmpty())
            continue;
        Event e;
        e.kind = Kind::Typed;
        e.what = tr("Went to %1").arg(v.text());
        e.user = user.name;
        e.source = tr("the folders typed into Explorer");
        report.events << e;
    }
    // Searches typed into Explorer's search box (WordWheelQuery: each is UTF-16).
    const QString wheel = explorer + QStringLiteral("\\WordWheelQuery");
    for (const reg::Value &v : user.keys->values(wheel)) {
        const QString term = utf16(v.data);
        if (v.name == QLatin1String("MRUListEx") || term.isEmpty())
            continue;
        Event e;
        e.kind = Kind::Search;
        e.what = tr("Searched for \"%1\"").arg(term);
        e.user = user.name;
        e.source = tr("the Explorer search box's history");
        report.events << e;
    }
}

// --- Recent shortcuts: their target and when the file was opened (the shortcut's own time).

void readRecentLinks(const startup::Machine &m, const startup::Machine::User &user, Report &report, const std::atomic<bool> *cancel)
{
    const QString folder = m.local(user.profile + QStringLiteral("\\AppData\\Roaming\\Microsoft\\Windows\\Recent"));
    if (folder.isEmpty())
        return;
    const QFileInfoList links = QDir(folder).entryInfoList({QStringLiteral("*.lnk")}, QDir::Files, QDir::Time);
    int done = 0;
    for (const QFileInfo &fi : links) {
        if (stopped(cancel) || ++done > 400)
            return;
        QFile f(fi.filePath());
        shelllink::Link link;
        if (!f.open(QIODevice::ReadOnly) || !shelllink::parse(f.read(256 * 1024), &link) || link.target.isEmpty())
            continue;
        Event e;
        e.kind = link.target.contains(QLatin1Char('.')) ? Kind::FileOpened : Kind::FolderOpened;
        e.when = fi.lastModified();
        e.what = (e.kind == Kind::FileOpened ? tr("Opened %1") : tr("Opened the folder %1")).arg(fi.completeBaseName());
        e.detail = link.target;
        e.user = user.name;
        e.source = tr("a shortcut in the Recent folder");
        report.events << e;
    }
}

// --- Browsers: what was visited and downloaded. Their history is in SQLite files, read by the
// sqlite3 program from a copy (the drive isn't touched, and a browser's -wal file is read too).

QJsonArray sqlite(const QString &file, const QString &query)
{
    const QString program = repair::sqlite3();
    QTemporaryDir dir;
    if (program.isEmpty() || !dir.isValid())
        return {};
    const QString copy = dir.filePath(QStringLiteral("history.db"));
    for (const char *suffix : {"", "-wal", "-journal"}) {
        if (QFileInfo::exists(file + QLatin1String(suffix)))
            QFile::copy(file + QLatin1String(suffix), copy + QLatin1String(suffix));
    }
    QProcess p;
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(program, {QStringLiteral("-json"), copy, query});
    if (!p.waitForFinished(120000)) {
        p.kill();
        p.waitForFinished(5000);
        return {};
    }
    return QJsonDocument::fromJson(p.readAllStandardOutput()).array();
}

// A Chromium browser's times: microseconds since 1601. Firefox's: since 1970.
QDateTime chromiumTime(const QJsonValue &v)
{
    return fromFileTime(v.toString().toULongLong() * 10);
}

QDateTime firefoxTime(const QJsonValue &v)
{
    const qint64 us = v.toString().toLongLong();
    return us > 0 ? QDateTime::fromMSecsSinceEpoch(us / 1000, QTimeZone::UTC) : QDateTime();
}

Event visit(const QJsonObject &o, QDateTime when, const QString &user, const QString &browser)
{
    Event e;
    e.kind = Kind::Website;
    e.when = when;
    const QString title = o.value(QStringLiteral("title")).toString().simplified().left(200);
    const QString url = o.value(QStringLiteral("url")).toString().left(2000);
    e.what = tr("Visited %1").arg(title.isEmpty() ? QUrl(url).host() : title);
    e.detail = url;
    e.user = user;
    e.source = tr("%1's history").arg(browser);
    return e;
}

Event download(const QString &path, const QJsonObject &o, QDateTime when, const QString &user, const QString &browser)
{
    Event e;
    e.kind = Kind::Download;
    e.when = when;
    e.what = tr("Downloaded %1").arg(path.section(QLatin1Char('\\'), -1).section(QLatin1Char('/'), -1));
    e.detail = QStringList{path, o.value(QStringLiteral("url")).toString().left(2000)}.join(QStringLiteral(" · "));
    e.user = user;
    e.source = tr("%1's downloads").arg(browser);
    return e;
}

// The browsers' folders: Chromium ones by name and where their profiles are, and Firefox's
// profile folders.
struct BrowserFolders {
    QVector<std::pair<QString, QString>> chromium;
    QStringList firefox;
};

void readBrowsers(const BrowserFolders &folders, const QString &userName, Report &report, const std::atomic<bool> *cancel)
{
    constexpr int kMostVisits = 5000;
    const QString user = userName;
    for (const auto &[browser, data] : folders.chromium) {
        if (data.isEmpty())
            continue;
        for (const QFileInfo &profile : QDir(data).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString history = profile.filePath() + QStringLiteral("/History");
            if (stopped(cancel) || !QFileInfo(history).isFile())
                continue;
            const QString name = browser + (profile.fileName() == QLatin1String("Default") ? QString() : QStringLiteral(" (%1)").arg(profile.fileName()));
            const QString visits = QStringLiteral("SELECT u.url AS url, u.title AS title, CAST(v.visit_time AS TEXT) AS t FROM visits v "
                                                  "JOIN urls u ON u.id = v.url ORDER BY v.visit_time DESC LIMIT %1;")
                                       .arg(kMostVisits);
            for (const QJsonValue &row : sqlite(history, visits))
                report.events << visit(row.toObject(), chromiumTime(row.toObject().value(QStringLiteral("t"))), user, name);
            const QString downloads = QStringLiteral("SELECT target_path AS path, tab_url AS url, CAST(start_time AS TEXT) AS t FROM downloads "
                                                     "ORDER BY start_time DESC LIMIT 2000;");
            for (const QJsonValue &row : sqlite(history, downloads)) {
                const QJsonObject o = row.toObject();
                report.events << download(o.value(QStringLiteral("path")).toString(), o, chromiumTime(o.value(QStringLiteral("t"))), user, name);
            }
        }
    }
    for (const QString &firefox : folders.firefox) {
    if (firefox.isEmpty())
        continue;
    for (const QFileInfo &profile : QDir(firefox).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString places = profile.filePath() + QStringLiteral("/places.sqlite");
        if (stopped(cancel) || !QFileInfo(places).isFile())
            continue;
        const QString name = QStringLiteral("Firefox (%1)").arg(profile.fileName().section(QLatin1Char('.'), 1));
        const QString visits = QStringLiteral("SELECT p.url AS url, p.title AS title, CAST(v.visit_date AS TEXT) AS t FROM moz_historyvisits v "
                                              "JOIN moz_places p ON p.id = v.place_id ORDER BY v.visit_date DESC LIMIT %1;")
                                   .arg(kMostVisits);
        for (const QJsonValue &row : sqlite(places, visits))
            report.events << visit(row.toObject(), firefoxTime(row.toObject().value(QStringLiteral("t"))), user, name);
        // Downloads are notes on the page they came from, saying where the file went.
        const QString downloads = QStringLiteral("SELECT a.content AS path, p.url AS url, CAST(a.dateAdded AS TEXT) AS t FROM moz_annos a "
                                                 "JOIN moz_anno_attributes n ON n.id = a.anno_attribute_id JOIN moz_places p ON p.id = a.place_id "
                                                 "WHERE n.name = 'downloads/destinationFileURI' ORDER BY a.dateAdded DESC LIMIT 2000;");
        for (const QJsonValue &row : sqlite(places, downloads)) {
            const QJsonObject o = row.toObject();
            const QUrl file(o.value(QStringLiteral("path")).toString());
            report.events << download(file.isLocalFile() ? file.toLocalFile() : file.toString(), o, firefoxTime(o.value(QStringLiteral("t"))), user, name);
        }
    }
    }
}

BrowserFolders windowsBrowsers(const startup::Machine &m, const startup::Machine::User &user)
{
    BrowserFolders f;
    for (const auto &[browser, place] : {std::pair{"Chrome", "\\AppData\\Local\\Google\\Chrome\\User Data"},
                                         std::pair{"Edge", "\\AppData\\Local\\Microsoft\\Edge\\User Data"},
                                         std::pair{"Brave", "\\AppData\\Local\\BraveSoftware\\Brave-Browser\\User Data"},
                                         std::pair{"Vivaldi", "\\AppData\\Local\\Vivaldi\\User Data"},
                                         std::pair{"Opera", "\\AppData\\Roaming\\Opera Software"}})
        f.chromium.push_back({QLatin1String(browser), m.local(user.profile + QLatin1String(place))});
    f.firefox << m.local(user.profile + QStringLiteral("\\AppData\\Roaming\\Mozilla\\Firefox\\Profiles"));
    return f;
}

// --- Linux and macOS.

// A file on the mounted system, following its links the way that system would: a link to
// "/usr/lib/os-release" means the one on that drive, not this PC's.
QString onDrive(const QString &root, QString path)
{
    for (int hops = 0; hops < 8; ++hops) {
        const QFileInfo fi(root + path);
        if (!fi.isSymLink())
            return root + path;
        const QString target = fi.readSymLink();
        path = QDir::cleanPath(target.startsWith(QLatin1Char('/')) ? target : QFileInfo(path).path() + QLatin1Char('/') + target);
        if (path.startsWith(QLatin1String("..")))
            return {};
    }
    return {};
}

QByteArray readOn(const QString &root, const QString &path, qint64 most = 8 * 1024 * 1024, bool start = false)
{
    QFile f(onDrive(root, path));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    // A log's newest part is at its end.
    if (!start && f.size() > most)
        f.seek(f.size() - most);
    return f.read(most);
}

QHash<QString, QString> osRelease(const QString &root)
{
    QHash<QString, QString> v;
    QByteArray text = readOn(root, QStringLiteral("/etc/os-release"), 64 * 1024);
    if (text.isEmpty())
        text = readOn(root, QStringLiteral("/usr/lib/os-release"), 64 * 1024);
    for (const QByteArray &line : text.split('\n')) {
        const qsizetype eq = line.indexOf('=');
        if (eq <= 0)
            continue;
        QString value = QString::fromUtf8(line.mid(eq + 1)).trimmed();
        if (value.size() >= 2 && (value.startsWith(QLatin1Char('"')) || value.startsWith(QLatin1Char('\''))))
            value = value.mid(1, value.size() - 2);
        v.insert(QString::fromUtf8(line.left(eq)).trimmed(), value);
    }
    return v;
}

// macOS's SystemVersion.plist: <key>ProductVersion</key><string>14.5</string>.
QHash<QString, QString> plistStrings(const QByteArray &xml)
{
    QHash<QString, QString> v;
    static const QRegularExpression pair(QStringLiteral("<key>([^<]{1,80})</key>\\s*<string>([^<]{0,200})</string>"));
    for (auto it = pair.globalMatch(QString::fromUtf8(xml)); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        v.insert(m.captured(1), m.captured(2));
    }
    return v;
}

struct LinuxUser {
    QString name, home;
};

QVector<LinuxUser> linuxUsers(const QString &root)
{
    QVector<LinuxUser> users;
    for (const QByteArray &line : readOn(root, QStringLiteral("/etc/passwd"), 1024 * 1024).split('\n')) {
        const QList<QByteArray> f = line.split(':');
        if (f.size() < 7)
            continue;
        const int uid = f[2].toInt();
        if ((uid >= 1000 && uid < 60000) || uid == 0)
            users.push_back({QString::fromUtf8(f[0]), QString::fromUtf8(f[5])});
    }
    return users;
}

void readLinux(const QString &root, Report &report, const std::atomic<bool> *cancel)
{
    report.system = QStringLiteral("Linux");
    Overview &o = report.overview;
    const QHash<QString, QString> release = osRelease(root);
    o.product = release.value(QStringLiteral("PRETTY_NAME"), release.value(QStringLiteral("NAME"), QStringLiteral("Linux")));
    o.release = release.value(QStringLiteral("VERSION_ID"), release.value(QStringLiteral("BUILD_ID")));
    // Its newest kernel.
    QStringList kernels;
    for (const QString &modules : {QStringLiteral("/usr/lib/modules"), QStringLiteral("/lib/modules")})
        kernels += QDir(onDrive(root, modules)).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    kernels.removeDuplicates();
    std::sort(kernels.begin(), kernels.end(), [](const QString &a, const QString &b) { return QVersionNumber::fromString(a) < QVersionNumber::fromString(b); });
    // Its kernels, newest first (it may start any of them).
    std::reverse(kernels.begin(), kernels.end());
    if (kernels.size() == 1)
        o.build = tr("kernel %1").arg(kernels.first());
    else if (!kernels.isEmpty())
        o.build = tr("kernels %1").arg(kernels.mid(0, 4).join(QStringLiteral(", ")));
    o.computerName = QString::fromUtf8(readOn(root, QStringLiteral("/etc/hostname"), 256)).trimmed();
    const QString zone = QFileInfo(root + QStringLiteral("/etc/localtime")).readSymLink();
    o.timeZone = zone.contains(QLatin1String("zoneinfo/")) ? zone.section(QStringLiteral("zoneinfo/"), 1)
                                                            : QString::fromUtf8(readOn(root, QStringLiteral("/etc/timezone"), 256)).trimmed();

    for (const LinuxUser &u : linuxUsers(root)) {
        Event e;
        e.kind = Kind::Account;
        e.what = tr("Account: %1").arg(u.name);
        e.detail = u.home;
        e.source = QStringLiteral("/etc/passwd");
        report.events << e;
    }
    // Sign-ins, start-ups and shut-downs: wtmp, or wtmpdb's database (Debian 13 and later).
    // wtmp's newest 20000 records (they're 384 bytes each, from the file's start).
    QByteArray wtmp;
    if (QFile f(onDrive(root, QStringLiteral("/var/log/wtmp"))); f.open(QIODevice::ReadOnly)) {
        f.seek(std::max<qint64>(0, f.size() - 384LL * 20000) / 384 * 384);
        wtmp = f.read(384LL * 20000);
    }
    QVector<Session> sessions = readWtmp(wtmp);
    const QString wtmpdb = onDrive(root, QStringLiteral("/var/lib/wtmpdb/wtmp.db"));
    if (sessions.isEmpty() && QFileInfo(wtmpdb).isFile()) {
        for (const QJsonValue &row : sqlite(wtmpdb, QStringLiteral("SELECT Type AS type, User AS user, TTY AS line, RemoteHost AS host, "
                                                                   "CAST(Login AS TEXT) AS login, CAST(Logout AS TEXT) AS logout FROM wtmp "
                                                                   "ORDER BY Login DESC LIMIT 20000;"))) {
            const QJsonObject r = row.toObject();
            Session in;
            in.type = r.value(QStringLiteral("type")).toInt() == 2 ? 2 : 7;
            in.user = r.value(QStringLiteral("user")).toString();
            in.line = r.value(QStringLiteral("line")).toString();
            in.host = r.value(QStringLiteral("host")).toString();
            in.when = QDateTime::fromMSecsSinceEpoch(r.value(QStringLiteral("login")).toString().toLongLong() / 1000, QTimeZone::UTC);
            sessions << in;
            const qint64 logout = r.value(QStringLiteral("logout")).toString().toLongLong();
            if (in.type == 2 && logout > 0)
                sessions << Session{1, QStringLiteral("shutdown"), QString(), QString(), QDateTime::fromMSecsSinceEpoch(logout / 1000, QTimeZone::UTC)};
        }
    }
    for (const Session &s : std::as_const(sessions)) {
        if (stopped(cancel))
            return;
        Event e;
        e.kind = Kind::Login;
        e.when = s.when;
        e.source = QFileInfo::exists(wtmpdb) && !QFileInfo::exists(onDrive(root, QStringLiteral("/var/log/wtmp"))) ? tr("the sign-in database (wtmpdb)")
                                                                                                                     : tr("the sign-in log (wtmp)");
        if (s.type == 7) {
            e.what = tr("Signed in: %1").arg(s.user);
            e.user = s.user;
            e.detail = s.host.isEmpty() ? tr("on %1").arg(s.line) : tr("on %1, from %2").arg(s.line, s.host);
        } else if (s.type == 2) {
            e.what = tr("Started up");
            e.detail = s.host; // the kernel it started
        } else if (s.type == 1 && s.user == QLatin1String("shutdown")) {
            e.what = tr("Shut down");
            if (!o.lastShutdown.isValid() || s.when > o.lastShutdown)
                o.lastShutdown = s.when;
        } else {
            continue;
        }
        report.events << e;
    }
    // Programs installed and removed, from the package manager's own log.
    for (const QString &log : {QStringLiteral("/var/log/pacman.log"), QStringLiteral("/var/log/dpkg.log"), QStringLiteral("/var/log/dpkg.log.1")}) {
        const QVector<Event> packages = readPackageLog(readOn(root, log));
        for (Event e : packages) {
            e.source = log;
            report.events << e;
        }
    }
    // When it was put on the drive: the installer's own log, or the package log's first line.
    const QFileInfo installer(onDrive(root, QStringLiteral("/var/log/installer")));
    if (installer.exists())
        o.installed = installer.lastModified();
    else if (const QByteArray head = readOn(root, QStringLiteral("/var/log/pacman.log"), 64, true); head.startsWith('['))
        o.installed = QDateTime::fromString(QString::fromLatin1(head.mid(1, head.indexOf(']') - 1)).left(24), Qt::ISODate);
    // Each person's recent files and browsers.
    for (const LinuxUser &u : linuxUsers(root)) {
        if (stopped(cancel))
            return;
        const QString home = onDrive(root, u.home);
        if (home.isEmpty() || !QFileInfo(home).isDir())
            continue;
        report.events += readRecentlyUsed(readOn(root, u.home + QStringLiteral("/.local/share/recently-used.xbel")), u.name);
        BrowserFolders f;
        for (const auto &[browser, place] : {std::pair{"Chrome", "/.config/google-chrome"}, std::pair{"Chromium", "/.config/chromium"},
                                             std::pair{"Edge", "/.config/microsoft-edge"}, std::pair{"Brave", "/.config/BraveSoftware/Brave-Browser"},
                                             std::pair{"Vivaldi", "/.config/vivaldi"}, std::pair{"Opera", "/.config/opera"},
                                             std::pair{"Chromium (Flatpak)", "/.var/app/org.chromium.Chromium/config/chromium"}})
            f.chromium.push_back({QLatin1String(browser), onDrive(root, u.home + QLatin1String(place))});
        for (const char *place : {"/.mozilla/firefox", "/.var/app/org.mozilla.firefox/.mozilla/firefox", "/snap/firefox/common/.mozilla/firefox"})
            f.firefox << onDrive(root, u.home + QLatin1String(place));
        readBrowsers(f, u.name, report, cancel);
    }
    report.notes << tr("Linux keeps no list of USB drives outside its journal, so none are listed.");
}

void readMac(const QString &root, Report &report)
{
    report.system = QStringLiteral("macOS");
    const QHash<QString, QString> v = plistStrings(readOn(root, QStringLiteral("/System/Library/CoreServices/SystemVersion.plist"), 64 * 1024));
    report.overview.product = v.value(QStringLiteral("ProductName"), QStringLiteral("macOS"));
    report.overview.release = v.value(QStringLiteral("ProductVersion"));
    report.overview.build = v.value(QStringLiteral("ProductBuildVersion"));
    for (const QString &name : QDir(root + QStringLiteral("/Users")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (name == QLatin1String("Shared") || name.startsWith(QLatin1Char('.')))
            continue;
        Event e;
        e.kind = Kind::Account;
        e.what = tr("Account: %1").arg(name);
        e.detail = QStringLiteral("/Users/") + name;
        e.source = tr("the Users folder");
        report.events << e;
    }
    report.notes << tr("For a Mac, only what it is and its accounts are read.");
}

// --- Networks joined (SOFTWARE\...\NetworkList\Profiles).

void readNetworks(const startup::Machine &m, Report &report)
{
    const QString profiles = QStringLiteral("Microsoft\\Windows NT\\CurrentVersion\\NetworkList\\Profiles");
    for (const QString &guid : m.software->subkeys(profiles)) {
        const QString key = profiles + QLatin1Char('\\') + guid;
        const QString name = m.software->text(key, QStringLiteral("ProfileName"));
        if (name.isEmpty())
            continue;
        const auto created = m.software->value(key, QStringLiteral("DateCreated"));
        const auto connected = m.software->value(key, QStringLiteral("DateLastConnected"));
        Event e;
        e.kind = Kind::Network;
        e.when = connected ? fromSystemTime(connected->data) : QDateTime();
        e.what = tr("Joined the network \"%1\"").arg(name);
        if (created && fromSystemTime(created->data).isValid())
            e.detail = tr("first joined %1").arg(QLocale().toString(fromSystemTime(created->data).toLocalTime(), QLocale::ShortFormat));
        e.source = tr("the list of networks joined");
        report.events << e;
    }
}

} // namespace

QString kindName(Kind kind)
{
    switch (kind) {
    case Kind::UsbDevice:
        return tr("USB drive");
    case Kind::ProgramRun:
        return tr("Program run");
    case Kind::FileOpened:
        return tr("File opened");
    case Kind::FolderOpened:
        return tr("Folder opened");
    case Kind::Search:
        return tr("Search");
    case Kind::Typed:
        return tr("Typed in");
    case Kind::Website:
        return tr("Website");
    case Kind::Download:
        return tr("Download");
    case Kind::Network:
        return tr("Network");
    case Kind::Account:
        return tr("Account");
    case Kind::Login:
        return tr("Sign-in or start-up");
    case Kind::Software:
        return tr("Program installed");
    }
    return QString();
}

QString rot13(const QString &text)
{
    QString out = text;
    for (QChar &c : out) {
        if (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
            c = QChar(u'a' + (c.unicode() - u'a' + 13) % 26);
        else if (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
            c = QChar(u'A' + (c.unicode() - u'A' + 13) % 26);
    }
    return out;
}

QString tidyUsbName(const QString &deviceId)
{
    // "Disk&Ven_SanDisk&Prod_Cruzer_Blade&Rev_1.00" -> "SanDisk Cruzer Blade".
    QString vendor, product;
    for (const QString &part : deviceId.split(QLatin1Char('&'))) {
        if (part.startsWith(QLatin1String("Ven_")))
            vendor = part.mid(4);
        else if (part.startsWith(QLatin1String("Prod_")))
            product = part.mid(5);
    }
    const QString name = (vendor + QLatin1Char(' ') + product).replace(QLatin1Char('_'), QLatin1Char(' ')).simplified();
    return name.isEmpty() ? deviceId : name;
}

QDateTime fromFileTime(quint64 ticks)
{
    if (ticks < 0x01C00000ull << 32 || ticks > 0x0200000000000000ull) // ~1996 to ~2057: anything else is nonsense
        return {};
    return QDateTime::fromMSecsSinceEpoch(qint64(ticks / 10000) - 11644473600000LL, QTimeZone::UTC);
}

QDateTime fromSystemTime(const QByteArray &b)
{
    if (b.size() < 16)
        return {};
    const auto n = [&b](int i) { return int(quint16(quint8(b[i]) | quint8(b[i + 1]) << 8)); };
    const int year = n(0), month = n(2), day = n(6), hour = n(8), minute = n(10), second = n(12);
    const QDate date(year, month, day);
    const QTime time(hour, minute, second);
    if (!date.isValid() || !time.isValid() || year < 1996 || year > 2057)
        return {};
    return QDateTime(date, time, QTimeZone::UTC);
}

QString systemOn(const QString &root)
{
    if (root.isEmpty())
        return {};
    if (startup::hasWindows(root))
        return QStringLiteral("Windows");
    if (!osRelease(root).isEmpty())
        return QStringLiteral("Linux");
    if (QFileInfo::exists(root + QStringLiteral("/System/Library/CoreServices/SystemVersion.plist")))
        return QStringLiteral("macOS");
    return {};
}

QVector<Session> readWtmp(const QByteArray &bytes)
{
    QVector<Session> out;
    constexpr int kRecord = 384;
    const auto text = [&bytes](qsizetype at, int len) {
        const QByteArray field = bytes.mid(at, len);
        const qsizetype zero = field.indexOf('\0');
        return QString::fromUtf8(zero >= 0 ? field.left(zero) : field).simplified();
    };
    for (qsizetype at = 0; at + kRecord <= bytes.size(); at += kRecord) {
        const auto *r = reinterpret_cast<const uchar *>(bytes.constData()) + at;
        const int type = qFromLittleEndian<qint16>(r);
        const qint64 seconds = qFromLittleEndian<qint32>(r + 340);
        if (type < 1 || type > 9 || seconds < 315532800 || seconds > 4102444800)
            continue;
        Session s;
        s.type = type;
        s.line = text(at + 8, 32);
        s.user = text(at + 44, 32);
        s.host = text(at + 76, 256);
        s.when = QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::UTC);
        out << s;
    }
    return out;
}

QVector<Event> readPackageLog(const QByteArray &log)
{
    QVector<Event> out;
    // pacman: "[2024-06-01T10:00:00+0100] [ALPM] installed foo (1.2-3)" (older: "[2019-01-01 10:00]").
    // dpkg: "2024-06-01 10:00:00 install foo:amd64 <none> 1.2-3".
    static const QRegularExpression pacman(QStringLiteral("^\\[([0-9T:+ -]{16,24})\\] \\[ALPM\\] (installed|removed) (\\S+) \\(([^)]*)\\)"));
    static const QRegularExpression dpkg(QStringLiteral("^(\\d{4}-\\d\\d-\\d\\d \\d\\d:\\d\\d:\\d\\d) (install|remove|purge) (\\S+) (\\S+) (\\S+)"));
    for (const QByteArray &raw : log.split('\n')) {
        const QString line = QString::fromUtf8(raw);
        Event e;
        e.kind = Kind::Software;
        if (const QRegularExpressionMatch m = pacman.match(line); m.hasMatch()) {
            QString when = m.captured(1).trimmed();
            if (when.size() == 24 && (when[19] == QLatin1Char('+') || when[19] == QLatin1Char('-')))
                when.insert(22, QLatin1Char(':'));
            e.when = QDateTime::fromString(when.replace(QLatin1Char(' '), QLatin1Char('T')), Qt::ISODate);
            e.what = (m.captured(2) == QLatin1String("installed") ? tr("Installed %1") : tr("Removed %1")).arg(m.captured(3));
            e.detail = m.captured(4);
        } else if (const QRegularExpressionMatch d = dpkg.match(line); d.hasMatch()) {
            e.when = QDateTime::fromString(d.captured(1), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            const QString package = d.captured(3).section(QLatin1Char(':'), 0, 0);
            e.what = (d.captured(2) == QLatin1String("install") ? tr("Installed %1") : tr("Removed %1")).arg(package);
            e.detail = d.captured(2) == QLatin1String("install") ? d.captured(5) : d.captured(4);
        } else {
            continue;
        }
        out << e;
    }
    return out;
}

QVector<Event> readRecentlyUsed(const QByteArray &xbel, const QString &user)
{
    QVector<Event> out;
    QXmlStreamReader r(xbel);
    Event *current = nullptr;
    while (!r.atEnd() && out.size() < 5000) {
        if (r.readNext() != QXmlStreamReader::StartElement)
            continue;
        if (r.name() == QLatin1String("bookmark")) {
            const QUrl href(r.attributes().value(QStringLiteral("href")).toString());
            QDateTime when = QDateTime::fromString(r.attributes().value(QStringLiteral("visited")).toString(), Qt::ISODateWithMs);
            const QDateTime changed = QDateTime::fromString(r.attributes().value(QStringLiteral("modified")).toString(), Qt::ISODateWithMs);
            if (!when.isValid() || (changed.isValid() && changed > when))
                when = changed;
            const QString path = href.isLocalFile() ? href.toLocalFile() : href.toString();
            Event e;
            e.kind = Kind::FileOpened;
            e.when = when;
            e.what = tr("Opened %1").arg(path.section(QLatin1Char('/'), -1));
            e.detail = path;
            e.user = user;
            e.source = tr("the list of recent files");
            out << e;
            current = &out.last();
        } else if (r.name() == QLatin1String("application") && current) {
            const QString app = r.attributes().value(QStringLiteral("name")).toString();
            if (!app.isEmpty() && !current->detail.contains(QLatin1String(" · ")))
                current->detail += QStringLiteral(" · ") + tr("with %1").arg(app);
        }
    }
    return out;
}

Report read(const QString &root, const std::atomic<bool> *cancel)
{
    Report report;
    if (!root.isEmpty() && !startup::hasWindows(root)) {
        const QString system = systemOn(root);
        if (system == QLatin1String("Linux"))
            readLinux(root, report, cancel);
        else if (system == QLatin1String("macOS"))
            readMac(root, report);
        std::stable_sort(report.events.begin(), report.events.end(), [](const Event &a, const Event &b) {
            if (a.when.isValid() != b.when.isValid())
                return a.when.isValid();
            return a.when.isValid() ? a.when > b.when : false;
        });
        return report;
    }
#ifdef Q_OS_WIN
    const std::optional<startup::Machine> machine = root.isEmpty() ? std::optional(startup::openRunningMachine()) : startup::openMachine(root);
#else
    const std::optional<startup::Machine> machine = startup::openMachine(root);
#endif
    if (!machine)
        return report;
    report.windows = true;
    report.system = QStringLiteral("Windows");
    report.notes = machine->notes;
    const startup::Machine &m = *machine;

    readOverview(m, report);
    for (const startup::Machine::User &u : m.users) {
        Event e;
        e.kind = Kind::Account;
        e.what = tr("Account: %1").arg(u.name);
        e.detail = u.profile;
        e.source = tr("the list of accounts");
        report.events << e;
    }
    if (!stopped(cancel))
        readUsb(m, report, cancel);
    if (!stopped(cancel))
        readNetworks(m, report);
    for (const startup::Machine::User &u : m.users) {
        if (stopped(cancel) || !u.keys)
            continue;
        readPrograms(u, report, cancel);
        readUserTrail(u, report, cancel);
        readRecentLinks(m, u, report, cancel);
        readBrowsers(windowsBrowsers(m, u), u.name, report, cancel);
    }

    // Newest first; the ones with no time after, in the order they were read.
    std::stable_sort(report.events.begin(), report.events.end(), [](const Event &a, const Event &b) {
        if (a.when.isValid() != b.when.isValid())
            return a.when.isValid();
        return a.when.isValid() ? a.when > b.when : false;
    });
    return report;
}

QString html(const Report &report, const QVector<Event> &events, const QString &where)
{
    const auto e = [](const QString &s) { return s.toHtmlEscaped(); };
    const auto when = [](const QDateTime &t) { return t.isValid() ? t.toLocalTime().toString(Qt::ISODate) : QString(); };
    const Overview &o = report.overview;
    QStringList owner{o.owner.trimmed(), o.organisation.trimmed()};
    owner.removeAll(QString());
    QString out = QStringLiteral("<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>%1</title><style>"
                                 "body{font-family:sans-serif;margin:24px;color:#111}h1{font-size:20px}table{border-collapse:collapse;font-size:12px}"
                                 "td,th{border:1px solid #bbb;padding:3px 6px;vertical-align:top;text-align:left}th{background:#eee}"
                                 ".about td:first-child{color:#555}</style></head><body>\n")
                      .arg(e(tr("What this PC did: %1").arg(where)));
    out += QStringLiteral("<h1>%1</h1>\n<table class=\"about\">").arg(e(tr("What %1 was used for").arg(where)));
    const QVector<std::pair<QString, QString>> facts = {
        {tr("System"), QStringList{o.product, o.product.contains(o.release) ? QString() : o.release, o.build}.join(QLatin1Char(' ')).simplified()},
        {tr("PC's name"), o.computerName},
        {tr("Registered to"), owner.join(QStringLiteral(", "))},
        {tr("Installed"), when(o.installed)},
        {tr("Last shut down"), when(o.lastShutdown)},
        {tr("Time zone"), o.timeZone},
        {tr("Report made"), QDateTime::currentDateTime().toString(Qt::ISODate)},
        {tr("Made by"), QStringLiteral("DiskAnvil ") + QCoreApplication::applicationVersion()},
    };
    for (const auto &[label, value] : facts) {
        if (!value.isEmpty())
            out += QStringLiteral("<tr><td>%1</td><td>%2</td></tr>").arg(e(label), e(value));
    }
    out += QStringLiteral("</table>\n");
    if (!report.notes.isEmpty())
        out += QStringLiteral("<p>%1</p>\n").arg(e(report.notes.join(QLatin1Char(' '))));
    out += QStringLiteral("<p></p><table><tr><th>%1</th><th>%2</th><th>%3</th><th>%4</th><th>%5</th><th>%6</th></tr>\n")
               .arg(e(tr("When")), e(tr("Kind")), e(tr("What")), e(tr("More")), e(tr("Whose")), e(tr("Read from")));
    for (const Event &ev : events)
        out += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td><td>%5</td><td>%6</td></tr>\n")
                   .arg(e(when(ev.when)), e(kindName(ev.kind)), e(ev.what), e(ev.detail), e(ev.user), e(ev.source));
    out += QStringLiteral("</table>\n<p>%1</p>\n</body></html>\n")
               .arg(e(tr("Times are this PC's. A time Windows didn't keep is left blank: that it happened is known, when isn't.")));
    return out;
}

QByteArray csv(const QVector<Event> &events)
{
    // Cells a spreadsheet would run as a formula get a ' first.
    const auto quote = [](QString s) {
        s.replace(QLatin1Char('\n'), QLatin1Char(' ')).replace(QLatin1Char('\r'), QLatin1Char(' '));
        if (s.startsWith(QLatin1Char('=')) || s.startsWith(QLatin1Char('+')) || s.startsWith(QLatin1Char('@')))
            s.prepend(QLatin1Char('\''));
        if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"')))
            s = QLatin1Char('"') + s.replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"');
        return s;
    };
    QStringList lines{QStringList{tr("When"), tr("Kind"), tr("What"), tr("More"), tr("Whose"), tr("Read from")}.join(QLatin1Char(','))};
    for (const Event &ev : events) {
        lines << QStringList{quote(ev.when.isValid() ? ev.when.toLocalTime().toString(Qt::ISODate) : QString()), quote(kindName(ev.kind)),
                             quote(ev.what), quote(ev.detail), quote(ev.user), quote(ev.source)}
                     .join(QLatin1Char(','));
    }
    return "\xEF\xBB\xBF" + lines.join(QStringLiteral("\r\n")).toUtf8() + "\r\n";
}

} // namespace activity
