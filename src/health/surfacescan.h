// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "blockmapdata.h"

#include <QObject>
#include <QVector>

#include <atomic>

// Reads every sector of a device to find the ones that can't be read. Read-only; needs
// an O_DIRECT fd (UDisks::openDevice with forBenchmark) so nothing comes from the cache.
class SurfaceScan : public QObject
{
    Q_OBJECT
public:
    SurfaceScan(int fd, quint64 size);
    ~SurfaceScan() override;
    void cancel() { m_cancel = true; }

public slots:
    void run();

signals:
    void progress(quint64 done, quint64 total, int badCount);
    // How long each read took, a few times a second, for the block map.
    void samples(const QVector<ReadSample> &batch);
    // badSectors: byte offsets of unreadable logical sectors
    void finished(bool completed, const QVector<quint64> &badSectors, int logicalSize);

private:
    int m_fd;
    quint64 m_size;
    std::atomic<bool> m_cancel{false};
};

struct RepairResult {
    int blocks = 0;     // physical blocks rewritten
    int recovered = 0;  // bad sectors that read fine on a retry; their data was kept
    int zeroed = 0;     // sectors that stayed unreadable and were filled with zeros
    int stillBad = 0;   // sectors that still fail after rewriting: the drive couldn't fix them
    QString error;
};

// Rewrites the physical blocks holding the given bad sectors so the drive either reuses
// them or swaps in a spare. Good sectors in the same block are read and written back
// unchanged; unreadable ones get zeros (they're already lost). Needs an O_DIRECT,
// read-write fd on an unmounted device.
class SectorRepair : public QObject
{
    Q_OBJECT
public:
    // physicalSize 0 = ask the device
    SectorRepair(int fd, const QVector<quint64> &badSectors, int physicalSize = 0);
    ~SectorRepair() override;

public slots:
    void run();

signals:
    void progress(int done, int total);
    void finished(const RepairResult &result);

private:
    int m_fd;
    QVector<quint64> m_bad;
    int m_physical;
};

Q_DECLARE_METATYPE(RepairResult)
