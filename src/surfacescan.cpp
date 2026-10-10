// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "surfacescan.h"

#include "blockio.h"

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
                    if (!timedRead(quint64(logical), s))
                        bad << s;
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
