// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// "What This PC Did": what a Windows on a drive was used for, read from its own records without
// starting it. Our own readers of its registry (reghive.h) and its shortcut files
// (shelllink.h): which USB sticks were plugged in and when, which programs were run and how
// often, which files and folders were opened, what was searched for and typed, which networks it
// joined, and which accounts are on it. Everything here is read-only and says where it came from,
// so it can be checked.
//
// It works best on DiskAnvil Live, with the Windows not running, and on any Windows drive from a
// Linux PC or the Windows version. Each piece is written for files that can be anything (a drive
// may be damaged, or made to mislead) and is bounds-checked and fuzzed.

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>

namespace activity {

// What the PC is: read from its registry, for the top of the report.
struct Overview {
    QString product;      // "Windows 11 Pro"
    QString release;      // "23H2"
    QString build;        // "22631"
    QString computerName;
    QString owner;        // who it was registered to
    QString organisation;
    QDateTime installed;
    QDateTime lastShutdown;
    QString timeZone;
    bool found() const { return !product.isEmpty() || !computerName.isEmpty(); }
};

enum class Kind {
    UsbDevice,   // a USB stick or drive plugged in
    ProgramRun,  // a program that was run
    FileOpened,  // a file opened
    FolderOpened,// a folder opened
    Search,      // something searched for
    Typed,       // a path or address typed in
    Website,     // a page visited in a browser
    Download,    // a file downloaded
    Network,     // a Wi-Fi or wired network joined
    Account,     // a user account on the PC
};

struct Event {
    Kind kind = Kind::ProgramRun;
    QDateTime when;    // when it happened; invalid when only that it did is known
    QString what;      // in plain words: "Plugged in SanDisk Cruzer", "Ran Notepad"
    QString detail;    // the file, the serial number, how many times, and so on
    QString user;      // whose it was; empty when it's the whole PC's
    QString source;    // where in Windows it was read from, so it can be checked
};

QString kindName(Kind kind);

struct Report {
    bool windows = false;
    Overview overview;
    QVector<Event> events; // newest first (events without a time last)
    QStringList notes;     // what couldn't be read, or may be out of date
};

// Reads it all from the Windows on the drive mounted at `root` (empty, in the Windows version: the
// Windows that's running). Run it on a worker thread; set `*cancel` to stop it.
Report read(const QString &root, const std::atomic<bool> *cancel = nullptr);

// The report to keep: a web page (also turned into a PDF) and CSV for a spreadsheet. `events`:
// the ones shown, in the order they're shown.
QString html(const Report &report, const QVector<Event> &events, const QString &where);
QByteArray csv(const QVector<Event> &events);

// Pieces exposed for the tests.
QString rot13(const QString &text);           // UserAssist hides its names with it
QString tidyUsbName(const QString &deviceId); // "Disk&Ven_SanDisk&Prod_Cruzer_Blade" -> "SanDisk Cruzer Blade"
QDateTime fromSystemTime(const QByteArray &bytes); // a 16-byte Windows SYSTEMTIME (the network list's)
QDateTime fromFileTime(quint64 ticks);        // 100 ns steps since 1601

} // namespace activity
