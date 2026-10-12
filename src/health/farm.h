// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Seagate drives keep a second record of their use, FARM (Field Accessible Reliability
// Metrics), which the usual SMART reset doesn't touch. When FARM's power-on hours are far more
// than SMART's, the SMART record was wiped: the drive has been used, whatever it was sold as.
// smartctl (smartmontools 7.4 and later) reads it, as a program of its own; it needs admin
// rights to talk to the drive.

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace farm {

struct Result {
    bool read = false;            // smartctl gave a FARM record
    qint64 smartHours = -1;       // what SMART says
    qint64 farmHours = -1;        // what FARM says
    qint64 spindleHours = -1;     // the motor's own hours (FARM)
    qint64 headFlightHours = -1;  // hours the heads were over the platters (FARM)
    qint64 powerCycles = -1;      // FARM
    QString problem;              // why it couldn't be read
    // The SMART hours were reset (FARM says at least a day more).
    bool reset() const { return read && smartHours >= 0 && farmHours > smartHours + 24; }
};

// Whether the drive is one that keeps FARM: Seagate's hard drives.
bool keeps(const QString &model);
// The command: smartctl's JSON for everything, FARM included. On DiskAnvil Live it's run with sudo.
QStringList command(const QString &device);
Result parse(const QByteArray &smartctlJson);
// In plain words.
QString describe(const Result &result);

} // namespace farm
