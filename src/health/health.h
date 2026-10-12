// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What a drive's SMART data means, in plain words, and the overall verdict. The attributes
// that count are the ones Backblaze's drive statistics show going with failures (5, 187,
// 188, 197, 198), plus a few that point at the hardware (10, 184) or the cable (199).
// Counters that never go down are only warned about when they've grown since DiskAnvil
// last saw them (or since the warning was dismissed).

#include "udisks.h"

#include <QDateTime>
#include <QMap>
#include <QString>
#include <QVector>

namespace health {

struct AttributeInfo {
    QString name;    // plain name, like "Replaced sectors"
    QString meaning; // a sentence or two for the Health window
    bool counts = false; // goes into the verdict
};
AttributeInfo describe(int id);

struct AtaInput {
    bool driveSaysFailing = false;
    int failingNow = 0;   // attributes at or under their threshold
    int failedBefore = 0; // attributes that were, at some point
    QVector<SmartAttribute> attributes;
    QMap<int, qint64> seen; // raw values DiskAnvil saw before (counters that never go down)
    double temperatureC = -1;
    bool ssd = false;
};

struct NvmeInput {
    QStringList criticalWarnings; // UDisks' names: spare, temperature, degraded, readonly, ...
    int availableSpare = -1;
    int spareThreshold = -1;
    int percentUsed = -1;
    qint64 mediaErrors = 0;
    double temperatureC = -1;
    double warningTempC = -1;
};

struct Verdict {
    Health::State state = Health::State::Healthy;
    QString summary;
    QVector<HealthReason> reasons; // worst first
    int lifeLeft = -1;             // SATA SSDs that report it
};

Verdict ata(const AtaInput &in);
Verdict nvme(const NvmeInput &in);

// Adds a reason found outside SMART (Btrfs errors), keeping the worst first and the verdict
// in step with it.
void add(Health &h, const HealthReason &reason);

// Where a drive's remembered values and dismissals are kept: its UDisks Drive.Id (the same
// drive keeps it on any port), or its object name when it has none.
QString keyFor(const QString &driveId, const QString &drivePath);

// A dismissed warning banner: "code=number" for each warning or failure. The banner comes
// back when there's one that wasn't there then, or a number that went up.
QStringList signature(const Health &h);
bool worseThan(const Health &h, const QStringList &dismissed);
// Whether the drive has spots it can't read (or is failing): then a plain copy stops at them,
// and Rescue Copy, which works around them, is the way to save what's on it.
bool hasUnreadableSpots(const Health &h);

// The counters whose growth matters (184, 188, 199), with the values DiskAnvil remembers
// for a drive. remember() stores values it hasn't seen yet; acknowledge() takes the
// current ones as the new starting point (a dismissed warning).
QList<int> watchedCounters();
QMap<int, qint64> seen(const QString &driveKey);
void remember(const QString &driveKey, const QVector<SmartAttribute> &attributes);
void acknowledge(const QString &driveKey, const QVector<SmartAttribute> &attributes);

// Self-tests, kept with the drive: a test DiskAnvil saw finish goes into its history (the
// newest 20), as drives keep only their last few results and NVMe drives show none of them.
struct SelftestRecord {
    QDateTime finished;
    QString kind;   // "short", "extended", "conveyance"; empty when DiskAnvil didn't start it
    QString status; // as the drive put it: "success", "aborted", "error_read"...
};
void noteSelftestStarted(const QString &key, const QString &kind);
// Every time the drive's self-test status is seen: a test that was running and isn't now is
// recorded.
void noteSelftestStatus(const QString &key, const QString &status);
QVector<SelftestRecord> selftestHistory(const QString &key);
QString selftestResult(const QString &status); // in plain words: "passed", "stopped", "failed (read error)"
QString selftestKind(const QString &kind);     // "short", "long", "conveyance"

} // namespace health
