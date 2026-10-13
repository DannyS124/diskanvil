// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Why Did It Crash?: a Windows that keeps restarting or blue-screening, explained from its own
// records without starting it. The System event log (evtx.h) says when it stopped without
// shutting down and how (a blue screen and its stop code, the power button held, the power
// cut or a hard freeze, dying on the way to sleep); the blue-screen dumps (crashdump.h) say
// which driver's code was running; Windows Error Reporting's notes fill in what the log has
// lost. Disk, file system and hardware errors logged just before each stop are put next to it.
// Then, across all of them, what they have in common: the same driver every time, different
// stop codes every time (hardware), the disk.
//
// A driver is only named with evidence, and how it's known is said ("its code was running",
// "seen on the stack"). Everything here is read-only.

#include "crashdump.h"
#include "evtx.h"

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>

namespace whycrash {

// A stop code: its name, what it means and what usually causes it, in plain words.
struct StopCode {
    quint32 code;
    const char *name;
    const char *meaning;
    const char *cause;
    int addressParameter; // 1 to 4: the one that's where in the code it happened; 0 none
    bool hardware;        // a hardware error: no driver is blamed
};
// The entry for a code (0x1000007E is 0x7E); nullptr when it isn't in the table.
const StopCode *stopCode(quint32 code);

enum class Kind {
    BlueScreen,  // stopped with a stop code
    PowerButton, // someone held the power button (usually because it froze)
    PowerLost,   // it just went off: the power was cut, or it froze so hard nothing was written
    Sleep,       // it died going to sleep or waking up
    Recovered,   // the screen froze and came back (the graphics driver was restarted)
};

struct Incident {
    Kind kind = Kind::PowerLost;
    QDateTime when;      // when it stopped (as near as is known), UTC
    QString title;       // "Blue screen: DRIVER_IRQL_NOT_LESS_OR_EQUAL (0xD1)"
    QString explanation; // what it means, in plain words
    QString driver;      // the driver its code was running in, when known: "nvlddmkm.sys"
    QString blame;       // how it's known, or what else it points at
    QStringList before;  // errors logged in the half hour before it
    QStringList sources; // where each part came from (event numbers, dump files), so it can be checked
    quint32 code = 0;
};

struct Report {
    QVector<Incident> incidents; // newest first
    QStringList summary;         // what they have in common, and what to do
    QStringList notes;           // what couldn't be read, settings that hide crashes
    QDateTime from;              // how far back the log goes
};

// The pieces, read: for the tests, and what read() puts together.
struct Records {
    evtx::Log system;
    QVector<QPair<QString, crashdump::Dump>> dumps; // file name, dump
    struct Wer {
        QString file;
        QString type;  // "BlueScreen", "LiveKernelEvent"
        QDateTime when;
        quint32 code = 0;
        std::array<quint64, 4> parameters {};
    };
    QVector<Wer> wer;
    int crashDumpEnabled = -1; // the CrashControl setting; -1 not known
    bool autoReboot = true;
    // What a driver says it is ("NVIDIA Kernel Mode Driver, NVIDIA Corporation"), by file name.
    std::function<QString(const QString &driverPath)> describeDriver;
    // Whether a driver is Windows' own (its file says Microsoft made it).
    std::function<bool(const QString &driverPath)> microsoftDriver;
};

Report explain(const Records &records);

// Everything read from the Windows mounted at `root` (empty: the running one, in the Windows version).
Report read(const QString &root, const std::atomic<bool> *cancel = nullptr);

// A Report.wer: UTF-16 "Key=Value" lines.
Records::Wer parseWer(const QByteArray &file);

// The report as plain text, to save or copy into a forum post.
QString asText(const Report &report);

QString kindName(Kind kind);

} // namespace whycrash
