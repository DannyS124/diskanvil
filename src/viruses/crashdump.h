// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Windows' blue-screen dumps (Minidump\*.dmp, MEMORY.DMP), read without Windows: the stop code
// and its four numbers, when it happened, and in a small dump the drivers that were loaded, so
// a number that's an address in a driver's code can be named ("in nvlddmkm.sys"). Our own reader;
// written for files that can be anything (every offset checked).

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

#include <array>

namespace crashdump {

struct Driver {
    QString name; // "nvlddmkm.sys"
    QString path; // as Windows had it: "\SystemRoot\System32\DriverStore\...\nvlddmkm.sys"
    quint64 base = 0;
    quint64 size = 0;
};

struct Dump {
    bool ok = false;
    QString problem;      // why it couldn't be read
    quint32 code = 0;     // the stop code (0x9F)
    std::array<quint64, 4> parameters {};
    QDateTime time;       // when it crashed (UTC)
    quint32 build = 0;    // Windows' build (22631)
    QString machine;      // "x64", "ARM64", "x86"
    int type = 0;         // 4 small (minidump), 1 complete, 5 or 6 kernel or bitmap...
    bool complete = true; // a small dump that says it was written to the end
    quint64 instruction = 0; // where the processor was (its saved context), when known
    QVector<Driver> drivers;  // small dumps only
    QVector<Driver> unloaded; // drivers unloaded before the crash (names cut to 12 letters by Windows)
    QStringList onStack;      // drivers seen on the crashing thread's stack, nearest first (a guess)
};

// From the file's first bytes (a big MEMORY.DMP needs only its first 8 KiB; a small dump all of it).
Dump read(const QByteArray &file);

// The driver an address falls in: "nvlddmkm.sys+0x1a2b3"; empty when none. `unloaded`: it was in
// one that had been unloaded already.
QString where(const Dump &dump, quint64 address, bool *unloaded = nullptr);

} // namespace crashdump
