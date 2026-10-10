// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// SMART data the way drives hand it over, decoded: ATA's attribute and threshold pages, and
// NVMe's health log page. On Windows these come straight from the drive; on Linux UDisks
// decodes them, and what's decoded here matches what it shows (sector counts, hours, °C).

#include "udisks.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace smart {

// ATA: the 512-byte SMART READ DATA page, and the READ THRESHOLDS one (may be empty). rawValue
// holds sector counts, power-on hours (9) and the temperature now (190, 194).
QVector<SmartAttribute> ataAttributes(const QByteArray &data, const QByteArray &thresholds);

// The self-test byte of the data page, in UDisks' words: "success", "aborted", "interrupted",
// "fatal", "error_unknown", "error_electrical", "error_servo", "error_read", "error_handling" or
// "inprogress"; and, while one runs, how much of it is left.
struct AtaSelftest {
    QString status;
    int percentRemaining = -1;
};
AtaSelftest ataSelftest(const QByteArray &data);

// NVMe's SMART / Health Information log page (512 bytes).
struct NvmeHealth {
    bool valid = false;
    QStringList criticalWarnings; // UDisks' names: spare, temperature, degraded, readonly, volatile_mem, pmr_readonly
    double temperatureC = -1;
    int availableSpare = -1;
    int spareThreshold = -1;
    int percentUsed = -1;
    quint64 bytesRead = 0;
    quint64 bytesWritten = 0;
    quint64 powerCycles = 0;
    quint64 powerOnHours = 0;
    quint64 unsafeShutdowns = 0;
    quint64 mediaErrors = 0;
    quint64 errorLogEntries = 0;
    quint32 warningTempMinutes = 0;
    quint32 criticalTempMinutes = 0;
    QVector<double> sensorsC; // the ones the drive has
};
NvmeHealth nvmeHealth(const QByteArray &page);

// NVMe's Identify Controller page: the drive's own warning and critical temperatures (-1 when
// it doesn't say).
void nvmeTemperatureLimits(const QByteArray &identify, double *warningC, double *criticalC);

} // namespace smart
