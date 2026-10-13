// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Windows' event logs (.evtx), read without Windows: our own reader of the file, its 64 KiB
// chunks and the binary XML each event is kept in (templates, and the values put into them).
// Only what's needed to tell what happened: who logged it, its number, its level, when, and its
// data. Written for files that can be anything: a log left half-written by a crash (its last
// events are the ones wanted, so a chunk with a bad checksum is still read, and events past
// where the chunk says it ends are picked up too), or a damaged or made-up one (every offset is
// checked; a broken event is skipped, never trusted).

#include <QByteArray>
#include <QDateTime>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>

namespace evtx {

struct Event {
    QString provider;   // "Microsoft-Windows-Kernel-Power", "EventLog"...
    quint32 id = 0;     // its event number (41)
    int level = 0;      // 1 critical, 2 error, 3 warning, 4 information
    QDateTime time;     // when it was logged (UTC)
    quint64 record = 0; // its number in the log
    // Its data: by name for newer providers ("BugcheckCode"), unnamed for older ones (in order).
    QVector<QPair<QString, QString>> data;
    QString binary;      // its binary part, as hex, when it has one
    bool recovered = false; // read from past where its chunk said it ends (a log left half-written)

    QString value(const QString &name) const; // by name; empty when it hasn't one
    QString at(int index) const;              // the index-th, named or not
};

struct Log {
    QVector<Event> events; // in the order they were logged (by record number)
    int chunks = 0;        // chunks read
    int broken = 0;        // events that couldn't be read
    QString problem;       // why nothing could be read, when nothing could
};

Log read(const QByteArray &file, const std::atomic<bool> *cancel = nullptr);

} // namespace evtx
