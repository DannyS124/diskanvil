// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "surfacescan.h"

#include "blockio.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSet>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unistd.h>

namespace {

constexpr quint64 kChunk = 1024 * 1024;
using blockio::alignedBuffer;
using blockio::logicalSize;
using blockio::physicalSize;
using blockio::readAt;

} // namespace

SurfaceScan::SurfaceScan(int fd, quint64 size)
    : m_fd(fd)
    , m_size(size)
{
    qRegisterMetaType<QVector<ReadSample>>();
}

SurfaceScan::~SurfaceScan()
{
    if (m_fd >= 0)
        blockio::close(m_fd);
}

void SurfaceScan::run()
{
    const int logical = logicalSize(m_fd);
    const int physical = physicalSize(m_fd);
    QVector<quint64> bad;
    blockio::Buffer buf = alignedBuffer(kChunk);
    if (!buf) {
        emit finished(false, bad, logical);
        return;
    }

    QVector<ReadSample> batch;
    QElapsedTimer sinceBatch;
    sinceBatch.start();
    auto flush = [&] {
        if (!batch.isEmpty())
            emit samples(batch);
        batch.clear();
        sinceBatch.restart();
    };
    // Reads one piece and notes how long it took.
    auto timedRead = [&](quint64 len, quint64 off) {
        QElapsedTimer t;
        t.start();
        const bool ok = readAt(m_fd, buf.get(), len, off);
        batch.append({off, quint32(len), quint32(t.elapsed()), ok});
        return ok;
    };

    quint64 lastReport = 0;
    for (quint64 off = 0; off < m_size; off += kChunk) {
        if (m_cancel) {
            flush();
            emit finished(false, bad, logical);
            return;
        }
        const quint64 len = std::min(kChunk, m_size - off);
        QElapsedTimer t;
        t.start();
        if (readAt(m_fd, buf.get(), len, off)) {
            batch.append({off, quint32(len), quint32(t.elapsed()), true});
        } else {
            // Narrow it down: physical blocks first, then the logical sectors inside a bad one.
            for (quint64 p = off; p < off + len; p += quint64(physical)) {
                const quint64 plen = std::min<quint64>(quint64(physical), off + len - p);
                if (readAt(m_fd, buf.get(), plen, p)) {
                    batch.append({p, quint32(plen), 0, true});
                    continue;
                }
                for (quint64 s = p; s < p + plen; s += quint64(logical)) {
                    if (timedRead(quint64(logical), s))
                        continue;
                    bad << s;
                    // A bad sector can take the drive seconds to give up on: counted straight away,
                    // not at the next 64 MiB, or a bad patch shows red with "0 so far" for an hour.
                    flush();
                    emit progress(s, m_size, int(bad.size()));
                }
            }
        }
        if (sinceBatch.elapsed() >= 250)
            flush();
        if (off + len - lastReport >= 64 * kChunk || off + len == m_size) {
            lastReport = off + len;
            emit progress(off + len, m_size, int(bad.size()));
        }
    }
    flush();
    emit finished(true, bad, logical);
}

SectorRepair::SectorRepair(int fd, const QVector<quint64> &badSectors, int physicalSize)
    : m_fd(fd)
    , m_bad(badSectors)
    , m_physical(physicalSize)
{
    qRegisterMetaType<RepairResult>();
}

SectorRepair::~SectorRepair()
{
    if (m_fd >= 0)
        blockio::close(m_fd);
}

void SectorRepair::run()
{
    RepairResult r;
    const int logical = logicalSize(m_fd);
    const int physical = std::max(m_physical > 0 ? m_physical : physicalSize(m_fd), logical);
    const QSet<quint64> bad(m_bad.cbegin(), m_bad.cend());

    QVector<quint64> blocks;
    for (quint64 s : m_bad) {
        const quint64 block = s / quint64(physical) * quint64(physical);
        if (!blocks.contains(block))
            blocks << block;
    }
    std::sort(blocks.begin(), blocks.end());

    blockio::Buffer buf = alignedBuffer(size_t(std::max<quint64>(quint64(physical), blockio::kAlign)));
    if (!buf) {
        r.error = tr("Out of memory");
        emit finished(r);
        return;
    }

    for (int i = 0; i < blocks.size(); ++i) {
        const quint64 block = blocks[i];
        // Rebuild the block sector by sector: keep whatever still reads, zero the rest.
        for (quint64 s = block; s < block + quint64(physical); s += quint64(logical)) {
            char *dst = buf.get() + (s - block);
            const int attempts = bad.contains(s) ? 3 : 1;
            bool ok = false;
            for (int a = 0; a < attempts && !ok; ++a)
                ok = readAt(m_fd, dst, quint64(logical), s);
            if (ok) {
                if (bad.contains(s))
                    ++r.recovered;
            } else {
                std::memset(dst, 0, size_t(logical));
                ++r.zeroed;
            }
        }
        if (!blockio::writeAt(m_fd, buf.get(), quint64(physical), block)) {
            r.error = tr("Writing sector %1 failed: %2").arg(block / quint64(logical)).arg(QString::fromLocal8Bit(strerror(errno)));
            emit finished(r);
            return;
        }
        blockio::flush(m_fd);
        ++r.blocks;
        for (quint64 s = block; s < block + quint64(physical); s += quint64(logical)) {
            if (!readAt(m_fd, buf.get(), quint64(logical), s))
                ++r.stillBad;
        }
        emit progress(i + 1, int(blocks.size()));
    }
    emit finished(r);
}

QVector<QPair<quint64, quint64>> slowRanges(const QVector<ReadSample> &samples, const BlockMapData &map)
{
    QVector<ReadSample> slow;
    for (const ReadSample &s : samples) {
        if (s.readable && map.isSlow(s.ms))
            slow << s;
    }
    std::sort(slow.begin(), slow.end(), [](const ReadSample &a, const ReadSample &b) { return a.offset < b.offset; });
    QVector<QPair<quint64, quint64>> out;
    for (const ReadSample &s : slow) {
        if (!out.isEmpty() && out.last().first + out.last().second >= s.offset)
            out.last().second = std::max(out.last().second, s.offset + s.length - out.last().first);
        else
            out.append({s.offset, s.length});
    }
    return out;
}

SlowRefresh::SlowRefresh(int fd, const QVector<QPair<quint64, quint64>> &ranges)
    : m_fd(fd)
    , m_ranges(ranges)
{
    qRegisterMetaType<RefreshResult>();
}

SlowRefresh::~SlowRefresh()
{
    if (m_fd >= 0)
        blockio::close(m_fd);
}

void SlowRefresh::run()
{
    RefreshResult r;
    quint64 total = 0;
    for (const auto &range : std::as_const(m_ranges))
        total += range.second;
    blockio::Buffer buf = alignedBuffer(kChunk);
    if (!buf) {
        r.error = tr("Out of memory");
        emit finished(r);
        return;
    }
    QElapsedTimer clock;
    double before = 0, after = 0;
    int timed = 0;
    quint64 done = 0;
    for (const auto &[start, length] : std::as_const(m_ranges)) {
        for (quint64 off = start; off < start + length && !m_cancel; off += kChunk) {
            const quint64 len = std::min(kChunk, start + length - off);
            clock.start();
            if (!readAt(m_fd, buf.get(), len, off)) {
                ++r.unreadable; // not this one's job: Repair handles what can't be read
                done += len;
                continue;
            }
            const double readMs = double(clock.nsecsElapsed()) / 1e6;
            if (!blockio::writeAt(m_fd, buf.get(), len, off) || !blockio::flush(m_fd)) {
                r.error = tr("Writing at %1 failed: %2").arg(off).arg(QString::fromLocal8Bit(strerror(errno)));
                emit finished(r);
                return;
            }
            clock.start();
            if (readAt(m_fd, buf.get(), len, off)) {
                after += double(clock.nsecsElapsed()) / 1e6 * double(kChunk) / double(len);
                before += readMs * double(kChunk) / double(len);
                ++timed;
            }
            r.bytes += len;
            done += len;
            emit progress(done, total);
        }
    }
    if (timed > 0) {
        r.beforeMs = before / timed;
        r.afterMs = after / timed;
    }
    if (m_cancel)
        r.error = tr("Stopped");
    emit finished(r);
}
