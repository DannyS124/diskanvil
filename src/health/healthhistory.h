// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A drive's health over time: one point a day for each drive DiskAnvil sees (the newest two
// years), so Disk Health can show how its temperature and the counters that matter went, and
// the daily background check can tell when one got worse. Kept in a small file for each drive in
// DiskAnvil's data folder (health/), on every system it runs on.

#include "udisks.h"

#include <QDate>
#include <QStringList>
#include <QVector>

namespace healthhistory {

struct Point {
    QDate day;
    double temperatureC = -1;
    qint64 hours = -1;
    qint64 replaced = -1;   // ATA 5
    qint64 pending = -1;    // ATA 197
    qint64 unreadable = -1; // ATA 198
    qint64 cable = -1;      // ATA 199
    int wear = -1;          // percent of its life used (NVMe percent_used, or 100 - an SSD's life left)
    qint64 mediaErrors = -1; // NVMe
    qint64 written = -1;    // bytes, NVMe
    int state = 0;          // Health::State
    bool operator==(const Point &o) const = default;
};

struct History {
    QString key, model, serial;
    QVector<Point> points;  // oldest first
    Point checked;          // what the background check last saw (no day = never)
};

// What a drive's health says now.
Point pointFor(const Disk &disk, const QVector<SmartAttribute> &attributes);

QString folder();
History load(const QString &key);
bool save(const History &history);
bool hasToday(const QString &key);
// Today's point for the drive (replacing an earlier one from today).
void note(const Disk &disk, const Point &point);
// For a drive with health data that has no point today yet: reads its attributes (a D-Bus call
// or an ioctl) and notes one. True when it did.
bool noteIfDue(UDisks *udisks, const Disk &disk);

// What got worse from `before` to `now`, in plain words, a line each. Empty when nothing did.
QStringList worse(const Point &before, const Point &now);

// One line of the graphs: a name, its unit, and its values by day.
struct Series {
    QString name;
    QString unit;      // "°C", "%", "" for a count, "bytes"
    bool badWhenUp = true;
    QVector<QPair<QDate, double>> values;
};
// The lines worth drawing for this history: the ones the drive reports.
QVector<Series> series(const History &history);

} // namespace healthhistory
