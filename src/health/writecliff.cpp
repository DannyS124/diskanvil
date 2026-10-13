// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "writecliff.h"

#include "blockio.h"
#include "format.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstring>

namespace writecliff {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("writecliff", text);
}

constexpr quint64 kMiB = 1024 * 1024;
constexpr quint64 kChunk = 8 * kMiB; // one write

double median(QVector<double> v)
{
    if (v.isEmpty())
        return 0;
    std::sort(v.begin(), v.end());
    return v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2;
}

} // namespace

quint64 sliceFor(quint64 total)
{
    const quint64 slice = std::max<quint64>(64 * kMiB, total / 200);
    return (slice + kChunk - 1) / kChunk * kChunk;
}

Finding analyse(const QVector<Point> &points)
{
    Finding f;
    if (points.isEmpty())
        return f;
    QVector<double> all;
    for (const Point &p : points)
        all << p.mbps;
    f.slowMbps = median(all);
    f.fastMbps = f.slowMbps;
    if (points.size() < 6)
        return f;
    // Where it fell: the split into a before and an after that fits the speeds best (each side
    // close to its own median). Then the after has to be under half the before.
    int best = -1;
    double bestCost = 0;
    for (int i = 2; i <= points.size() - 3; ++i) {
        const double before = median(all.mid(0, i)), after = median(all.mid(i));
        double cost = 0;
        for (int j = 0; j < all.size(); ++j)
            cost += std::abs(all[j] - (j < i ? before : after));
        if (best < 0 || cost < bestCost) {
            bestCost = cost;
            best = i;
        }
    }
    if (best >= 0 && median(all.mid(best)) >= median(all.mid(0, best)) * 0.5)
        best = -1;
    if (best < 0)
        return f;
    // Most of what comes after has to be slow, not a few slow slices among fast ones.
    const double before = median(all.mid(0, best));
    int slow = 0;
    for (int i = best; i < all.size(); ++i)
        slow += all[i] < before * 0.6;
    if (slow < (all.size() - best) * 3 / 4)
        return f;
    f.cliff = true;
    f.cacheBytes = points[best - 1].written;
    f.fastMbps = before;
    f.slowMbps = median(all.mid(best));
    return f;
}

QString describe(const Finding &f)
{
    if (!f.cliff)
        return tr("It kept a steady %1 MB/s all the way: no fast cache that ran out (or one bigger than what was written).").arg(qRound(f.slowMbps));
    QString text = tr("It wrote at %1 MB/s for the first %2, then dropped to %3 MB/s: its fast cache is about %2. Copies bigger than "
                      "that slow down to the second speed.")
                       .arg(qRound(f.fastMbps))
                       .arg(formatSize(f.cacheBytes))
                       .arg(qRound(f.slowMbps));
    if (f.slowMbps < 60)
        text += QStringLiteral(" ") + tr("That's very slow for an SSD after its cache: it's a cheap one (QLC flash, no DRAM), or not what it says it is.");
    return text;
}

Writer::Writer(QString file, int fd, quint64 total)
    : m_file(std::move(file))
    , m_fd(fd)
    , m_total(total / kChunk * kChunk)
{
}

Writer::~Writer()
{
    if (m_fd >= 0)
        blockio::close(m_fd);
}

void Writer::run()
{
    bool direct = true;
    if (m_fd < 0) {
        m_fd = blockio::openFile(m_file, blockio::FileMode::WriteDirect);
        if (m_fd < 0 && errno == EINVAL) {
            // A file system that can't skip the cache: written normally, flushed every slice.
            direct = false;
            m_fd = blockio::openFile(m_file, blockio::FileMode::CreateNew);
        }
        if (m_fd < 0) {
            Q_EMIT finished(false, tr("Couldn't make the test file: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
            return;
        }
    }
    // Random, and each chunk made different, so nothing can be compressed or deduplicated.
    blockio::Buffer buffer = blockio::alignedBuffer(kChunk);
    QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(buffer.get()), kChunk / 4);
    const quint64 slice = sliceFor(m_total);
    QElapsedTimer timer;
    timer.start();
    qint64 sliceStart = 0;
    quint64 inSlice = 0;
    QString error;
    quint64 done = 0;
    for (; done < m_total && !m_cancel; done += kChunk) {
        for (quint64 at = 0; at < kChunk; at += 4096)
            std::memcpy(buffer.get() + at, &done, sizeof done);
        if (!blockio::writeAt(m_fd, buffer.get(), kChunk, done)) {
            error = errno == ENOSPC ? tr("The drive got full.") : tr("Writing failed: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
            break;
        }
        inSlice += kChunk;
        if (inSlice >= slice || done + kChunk >= m_total) {
            if (!direct)
                blockio::flush(m_fd);
            const qint64 now = timer.nsecsElapsed();
            const double seconds = std::max(1e-6, (now - sliceStart) / 1e9);
            Q_EMIT point({done + kChunk, inSlice / seconds / 1e6});
            sliceStart = now;
            inSlice = 0;
        }
    }
    blockio::flush(m_fd);
    blockio::close(m_fd);
    m_fd = -1;
    if (!m_file.isEmpty())
        QFile::remove(m_file);
    if (!error.isEmpty() && done > 0 && error == tr("The drive got full."))
        error.clear(); // as far as it could go: what it wrote is still worth showing
    Q_EMIT finished(error.isEmpty() && (!m_cancel || done >= m_total), m_cancel ? tr("Stopped.") : error);
}

} // namespace writecliff
