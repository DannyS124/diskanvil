// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Write Speed Over Time: how fast a drive keeps writing when it's given a lot at once. Most SSDs
// (and many USB sticks and SMR hard drives) write into a fast cache first: a few GB to tens of GB
// of their flash used one bit per cell. When that's full they drop to what the flash really
// manages, often a tenth. That's why a big copy starts fast and then crawls, and it's how cheap
// and fake SSDs give themselves away. This writes and times it, a slice at a time, and finds
// where the speed fell and what it fell to.
//
// It writes into a file in the drive's free space (deleted afterwards: nothing on the drive is
// touched), or, on a drive with nothing on it, over the whole drive. Data that can't be
// compressed, so drives that compress what they're given can't cheat.

#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>

namespace writecliff {

struct Point {
    quint64 written = 0; // bytes written when this slice ended
    double mbps = 0;     // the slice's speed, MB/s
};

struct Finding {
    bool cliff = false;   // it slowed down for good part way
    quint64 cacheBytes = 0; // how much it wrote fast before that
    double fastMbps = 0;  // the speed before
    double slowMbps = 0;  // and after (all of it, when there's no cliff)
};
// Where a run of points shows a cliff: the speed falling below half of what it was and staying
// there (the median of what's left), not one slow slice.
Finding analyse(const QVector<Point> &points);
QString describe(const Finding &finding);

// How big each slice is for a test of `total` bytes: about 200 of them, at least 64 MiB.
quint64 sliceFor(quint64 total);

class Writer : public QObject
{
    Q_OBJECT
public:
    // Into a new file `file` (deleted at the end), or over the device `fd` (fd >= 0: taken over).
    Writer(QString file, int fd, quint64 total);
    ~Writer() override;
    void cancel() { m_cancel = true; }

public slots:
    void run();

signals:
    void point(const writecliff::Point &p);
    void finished(bool completed, const QString &error);

private:
    QString m_file;
    int m_fd;
    quint64 m_total;
    std::atomic<bool> m_cancel{false};
};

} // namespace writecliff

Q_DECLARE_METATYPE(writecliff::Point)
