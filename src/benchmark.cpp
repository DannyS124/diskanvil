// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "benchmark.h"

#include "blockio.h"

#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QTemporaryFile>

#include <cerrno>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <unistd.h>

namespace {

constexpr quint64 kAlign = 4096;   // O_DIRECT wants aligned buffers, offsets and sizes
constexpr int kReadSamples = 32;
constexpr quint64 kReadChunk = 8 * 1024 * 1024;
constexpr int kAccessSamples = 50;
constexpr qint64 kWriteSize = 256 * 1024 * 1024;

} // namespace

Benchmark::Benchmark(int fd, quint64 deviceSize, const QString &writeDir)
    : m_fd(fd)
    , m_size(deviceSize)
    , m_writeDir(writeDir)
{
    qRegisterMetaType<BenchmarkResult>();
}

Benchmark::~Benchmark()
{
    if (m_fd >= 0)
        ::close(m_fd);
}

void Benchmark::run()
{
    BenchmarkResult r;
    const quint64 chunk = std::min<quint64>(kReadChunk, m_size / kAlign * kAlign);
    blockio::Buffer buffer = blockio::alignedBuffer(chunk);
    if (chunk == 0 || !buffer) {
        emit finished(false, r, tr("The drive is too small to test"));
        return;
    }

    // Sequential reads spread over the whole disk: outer and inner parts of a hard
    // drive differ a lot, so one spot would be misleading.
    double seconds = 0;
    for (int i = 0; i < kReadSamples; ++i) {
        if (m_cancel)
            return emit finished(false, r, tr("Cancelled"));
        const quint64 offset = (m_size - chunk) / (kReadSamples - 1) * i / kAlign * kAlign;
        QElapsedTimer t;
        t.start();
        const bool ok = blockio::readAt(m_fd, buffer.get(), chunk, offset);
        const double s = t.nsecsElapsed() / 1e9;
        if (!ok)
            return emit finished(false, r, tr("Reading the drive failed: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        const double rate = chunk / s / 1e6;
        r.minReadMBps = r.minReadMBps < 0 ? rate : std::min(r.minReadMBps, rate);
        r.maxReadMBps = std::max(r.maxReadMBps, rate);
        seconds += s;
        emit progress(tr("Read speed"), (i + 1) * 60 / kReadSamples);
    }
    r.readMBps = double(chunk) * kReadSamples / seconds / 1e6;

    // Small reads at random places: how long the drive takes to get to data.
    double accessTotal = 0;
    for (int i = 0; i < kAccessSamples; ++i) {
        if (m_cancel)
            return emit finished(false, r, tr("Cancelled"));
        const quint64 offset = QRandomGenerator::global()->bounded(m_size / kAlign) * kAlign;
        QElapsedTimer t;
        t.start();
        if (!blockio::readAt(m_fd, buffer.get(), kAlign, offset))
            return emit finished(false, r, tr("Reading the drive failed: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        accessTotal += t.nsecsElapsed() / 1e6;
        emit progress(tr("Access time"), 60 + (i + 1) * 15 / kAccessSamples);
    }
    r.accessMs = accessTotal / kAccessSamples;

    if (!m_writeDir.isEmpty()) {
        QTemporaryFile file(m_writeDir + QStringLiteral("/.diskforge-benchmark-XXXXXX"));
        if (!file.open())
            return emit finished(false, r, tr("Couldn't create a test file: %1").arg(file.errorString()));
        QByteArray data(qint64(4 * 1024 * 1024), Qt::Uninitialized);
        QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(data.data()), data.size() / 4);
        QElapsedTimer t;
        t.start();
        for (qint64 done = 0; done < kWriteSize; done += data.size()) {
            if (m_cancel)
                return emit finished(false, r, tr("Cancelled"));
            if (file.write(data) != data.size())
                return emit finished(false, r, tr("Writing the test file failed: %1").arg(file.errorString()));
            emit progress(tr("Write speed"), 75 + int(done * 25 / kWriteSize));
        }
        blockio::flushFile(file);
        r.writeMBps = kWriteSize / (t.nsecsElapsed() / 1e9) / 1e6;
    }

    emit progress(tr("Done"), 100);
    emit finished(true, r, {});
}
