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

void readBrowsers(const startup::Machine &m, const startup::Machine::User &user, Report &report, const std::atomic<bool> *cancel)
{
    constexpr int kMostVisits = 5000;
    const QVector<std::pair<const char *, const char *>> chromium = {
        {"Chrome", "\\AppData\\Local\\Google\\Chrome\\User Data"},
        {"Edge", "\\AppData\\Local\\Microsoft\\Edge\\User Data"},
        {"Brave", "\\AppData\\Local\\BraveSoftware\\Brave-Browser\\User Data"},
        {"Vivaldi", "\\AppData\\Local\\Vivaldi\\User Data"},
        {"Opera", "\\AppData\\Roaming\\Opera Software"},
    };
    for (const auto &[browser, place] : chromium) {
        const QString data = m.local(user.profile + QLatin1String(place));
        if (data.isEmpty())
            continue;
        for (const QFileInfo &profile : QDir(data).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString history = profile.filePath() + QStringLiteral("/History");
            if (stopped(cancel) || !QFileInfo(history).isFile())
                continue;
            const QString name = QLatin1String(browser) + (profile.fileName() == QLatin1String("Default") ? QString() : QStringLiteral(" (%1)").arg(profile.fileName()));
            const QString visits = QStringLiteral("SELECT u.url AS url, u.title AS title, CAST(v.visit_time AS TEXT) AS t FROM visits v "
                                                  "JOIN urls u ON u.id = v.url ORDER BY v.visit_time DESC LIMIT %1;")
                                       .arg(kMostVisits);
            for (const QJsonValue &row : sqlite(history, visits))
                report.events << visit(row.toObject(), chromiumTime(row.toObject().value(QStringLiteral("t"))), user.name, name);
            const QString downloads = QStringLiteral("SELECT target_path AS path, tab_url AS url, CAST(start_time AS TEXT) AS t FROM downloads "
                                                     "ORDER BY start_time DESC LIMIT 2000;");
            for (const QJsonValue &row : sqlite(history, downloads)) {
                const QJsonObject o = row.toObject();
                report.events << download(o.value(QStringLiteral("path")).toString(), o, chromiumTime(o.value(QStringLiteral("t"))), user.name, name);
            }
        }
    }
    const QString firefox = m.local(user.profile + QStringLiteral("\\AppData\\Roaming\\Mozilla\\Firefox\\Profiles"));
    if (firefox.isEmpty())
        return;
    for (const QFileInfo &profile : QDir(firefox).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString places = profile.filePath() + QStringLiteral("/places.sqlite");
        if (stopped(cancel) || !QFileInfo(places).isFile())
            continue;
        const QString name = QStringLiteral("Firefox (%1)").arg(profile.fileName().section(QLatin1Char('.'), 1));
        const QString visits = QStringLiteral("SELECT p.url AS url, p.title AS title, CAST(v.visit_date AS TEXT) AS t FROM moz_historyvisits v "
                                              "JOIN moz_places p ON p.id = v.place_id ORDER BY v.visit_date DESC LIMIT %1;")
                                   .arg(kMostVisits);
        for (const QJsonValue &row : sqlite(places, visits))
            report.events << visit(row.toObject(), firefoxTime(row.toObject().value(QStringLiteral("t"))), user.name, name);
        // Downloads are notes on the page they came from, saying where the file went.
        const QString downloads = QStringLiteral("SELECT a.content AS path, p.url AS url, CAST(a.dateAdded AS TEXT) AS t FROM moz_annos a "
                                                 "JOIN moz_anno_attributes n ON n.id = a.anno_attribute_id JOIN moz_places p ON p.id = a.place_id "
                                                 "WHERE n.name = 'downloads/destinationFileURI' ORDER BY a.dateAdded DESC LIMIT 2000;");
        for (const QJsonValue &row : sqlite(places, downloads)) {
            const QJsonObject o = row.toObject();
            const QUrl file(o.value(QStringLiteral("path")).toString());
            report.events << download(file.isLocalFile() ? file.toLocalFile() : file.toString(), o, firefoxTime(o.value(QStringLiteral("t"))), user.name, name);
        }
    }
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

Report read(const QString &root, const std::atomic<bool> *cancel)
{
    Report report;
#ifdef Q_OS_WIN
    const std::optional<startup::Machine> machine = root.isEmpty() ? std::optional(startup::openRunningMachine()) : startup::openMachine(root);
#else
    const std::optional<startup::Machine> machine = startup::openMachine(root);
#endif
    if (!machine)
        return report;
    report.windows = true;
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
        readBrowsers(m, u, report, cancel);
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
    QString out = QStringLiteral("<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>%1</title><style>"
                                 "body{font-family:sans-serif;margin:24px;color:#111}h1{font-size:20px}table{border-collapse:collapse;font-size:12px}"
                                 "td,th{border:1px solid #bbb;padding:3px 6px;vertical-align:top;text-align:left}th{background:#eee}"
                                 ".about td:first-child{color:#555}</style></head><body>\n")
                      .arg(e(tr("What this PC did: %1").arg(where)));
    out += QStringLiteral("<h1>%1</h1>\n<table class=\"about\">").arg(e(tr("What the Windows on %1 was used for").arg(where)));
    const QVector<std::pair<QString, QString>> facts = {
        {tr("Windows"), QStringList{o.product, o.release, o.build.isEmpty() ? QString() : tr("build %1").arg(o.build)}.join(QLatin1Char(' ')).simplified()},
        {tr("PC's name"), o.computerName},
        {tr("Registered to"), QStringList{o.owner, o.organisation}.join(QStringLiteral(", ")).remove(QRegularExpression(QStringLiteral("^, |, $")))},
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
