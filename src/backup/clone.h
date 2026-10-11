// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Copies one drive onto another: the partition table, anything boot loaders keep before
// the first partition, and every partition. Empty space isn't copied. On a bigger
// target the GPT backup is moved to the real end so the space can be used.

#include "blockcopy.h"
#include "udisks.h"

#include <QObject>

#include <atomic>

namespace diskclone {

struct Plan {
    QVector<blockcopy::Extent> extents;
    quint64 bytes = 0;
    bool gpt = false;
    int sectorSize = 512;
    QString error; // set when this pair can't be cloned
};

// Pure: decides what to copy and refuses anything unsafe (system drives, a target
// that is the source, too small, or with a different sector size).
Plan plan(const Disk &source, const Disk &target);

// A fresh file system ID in the form the file system uses, or empty when changing it
// isn't supported.
QString newUuid(const QString &fsType);

} // namespace diskclone

class CloneJob : public QObject
{
    Q_OBJECT
public:
    // Takes ownership of both fds. newIds: the clone gets its own disk and partition IDs.
    CloneJob(int sourceFd, int targetFd, const diskclone::Plan &plan, bool newIds, bool verify);
    ~CloneJob() override;
    void cancel() { m_cancel = true; }

public slots:
    void run();

signals:
    void progress(const QString &phase, quint64 done, quint64 total);
    void finished(bool ok, const QString &message);

private:
    int m_source;
    int m_target;
    diskclone::Plan m_plan;
    bool m_newIds;
    bool m_verify;
    std::atomic<bool> m_cancel{false};
};
