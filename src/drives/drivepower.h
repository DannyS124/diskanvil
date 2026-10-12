// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// An ATA drive's settings, as UDisks keeps them (Drive.Configuration): when to spin down, how
// hard to save power (APM), how quietly to seek (AAM), the write cache and reading ahead.
// UDisks saves them in /etc/udisks2 and sets them again whenever the drive shows up.

#include <QString>
#include <QVariantMap>
#include <QVector>

namespace drivepower {

constexpr int kDriveDefault = -2; // no setting: the drive does what it does by itself
constexpr int kKeep = -1;         // leave this one as it is

struct Choice {
    int value;
    QString label;
};
QVector<Choice> standbyChoices(); // hdparm -S values: 0 never, 1-240 five-second steps, 241-251 half hours
QVector<Choice> apmChoices();     // 1 most saving ... 254 most performance
QVector<Choice> aamChoices();     // 128 quietest ... 254 fastest
QString standbyText(int value);   // "20 minutes", for any value

// What to change. kKeep leaves a setting alone, kDriveDefault removes it; the switches are
// kKeep, 0 or 1.
struct Changes {
    int standby = kKeep;
    int apm = kKeep;
    int aam = kKeep;
    int writeCache = kKeep;
    int lookahead = kKeep;
};
// The full configuration to set: the current one with these changed.
QVariantMap configuration(const QVariantMap &current, const Changes &changes);
QVariantMap configuration(const QVariantMap &current, int standby, int apm, int writeCache);

QString stateText(int state); // from PmGetState: "Spinning", "Asleep (spun down)", ...

// The Host Protected Area: the end of an ATA drive, hidden by the drive's own settings. Only
// hdparm can see it (UDisks can't), so only DiskForge Live shows it. From `hdparm -N`:
//   max sectors   = 976771055/976773168, HPA is enabled
struct HiddenArea {
    bool known = false;
    quint64 visible = 0; // sectors the system sees
    quint64 native = 0;  // sectors the drive really has
    quint64 hidden() const { return native > visible ? native - visible : 0; }
};
HiddenArea parseHdparmN(const QString &output);

// The sector sizes an ATA drive reports to hdparm -I, and the ones it can be switched to (some
// drives can: "Logical  Sector size: 512 bytes [ Supported: 512 4096 ]").
struct SectorSizes {
    int logical = 0;
    int physical = 0;
    QVector<int> supported; // empty: the drive can't change it
    bool canChange() const { return supported.size() > 1; }
};
SectorSizes parseHdparmI(const QString &output);
// hdparm's command to switch, which erases everything on the drive.
QStringList setSectorSizeCommand(const QString &device, int size);

} // namespace drivepower
