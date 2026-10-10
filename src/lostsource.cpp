// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lostsource.h"

#include "blockio.h"

#include <QCoreApplication>
#include <QFile>

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr quint64 kPiece = 1024 * 1024;     // reads go in pieces this big
constexpr quint64 kSmallPiece = 64 * 1024;  // a piece that failed is tried again this small

quint64 roundDown(quint64 value, quint64 to)
{
    return value / to * to;
}

quint64 roundUp(quint64 value, quint64 to)
{
    return (value + to - 1) / to * to;
}

} // namespace

std::shared_ptr<lost::Source> lost::Source::fromFd(int fd, bool direct)
{
    std::shared_ptr<Source> source(new Source);
    source->m_fd = fd;
    source->m_direct = direct;
    source->m_size = blockio::deviceSize(fd);
    return source;
}

std::shared_ptr<lost::Source> lost::Source::fromImage(const QString &path, QString *error)
{
    const int fd = blockio::openFile(path, blockio::FileMode::Read);
    if (fd < 0) {
        if (error)
            *error = QCoreApplication::translate("lostfiles", "Couldn't open %1: %2").arg(path, QString::fromLocal8Bit(strerror(errno)));
        return {};
    }
    return fromFd(fd, false);
}

std::shared_ptr<lost::Source> lost::Source::fromMemory(QByteArray data)
{
    std::shared_ptr<Source> source(new Source);
    source->m_memory = true;
    source->m_data = std::move(data);
    source->m_size = quint64(source->m_data.size());
    return source;
}

std::shared_ptr<lost::Source> lost::Source::fromLayer(std::unique_ptr<Layer> layer)
{
    std::shared_ptr<Source> source(new Source);
    source->m_size = layer->size();
    source->m_layer = std::move(layer);
    return source;
}

lost::Source::~Source()
{
    if (m_fd >= 0)
        blockio::close(m_fd);
}

bool lost::Source::rawRead(quint64 offset, char *buf, quint64 len)
{
    for (const Extent &e : std::as_const(m_failForTest)) {
        if (offset < e.end() && e.start < offset + len)
            return false;
    }
    if (m_memory) {
        memcpy(buf, m_data.constData() + offset, len);
        return true;
    }
    if (m_layer)
        return m_layer->read(offset, buf, len);
    if (!m_direct)
        return blockio::readAt(m_fd, buf, len, offset);
    // O_DIRECT: whole aligned blocks into an aligned buffer, then the part that was asked for.
    // The last block of a drive can be shorter than kAlign; sectors are always whole.
    const quint64 start = roundDown(offset, blockio::kAlign);
    const quint64 end = std::min(roundUp(offset + len, blockio::kAlign), roundUp(m_size, 512));
    blockio::Buffer aligned = blockio::alignedBuffer(size_t(end - start));
    if (!aligned || !blockio::readAt(m_fd, aligned.get(), end - start, start))
        return false;
    memcpy(buf, aligned.get() + (offset - start), len);
    return true;
}

void lost::Source::markBad(quint64 offset, quint64 len)
{
    QMutexLocker locker(&m_lock);
    Extent add{offset, len};
    QVector<Extent> merged;
    merged.reserve(m_bad.size() + 1);
    bool placed = false;
    for (const Extent &e : std::as_const(m_bad)) {
        if (e.end() < add.start) {
            merged << e;
        } else if (add.end() < e.start) {
            if (!placed) {
                merged << add;
                placed = true;
            }
            merged << e;
        } else {
            const quint64 start = std::min(e.start, add.start);
            add = {start, std::max(e.end(), add.end()) - start};
        }
    }
    if (!placed)
        merged << add;
    m_bad = merged;
}

quint64 lost::Source::read(quint64 offset, char *buf, quint64 len)
{
    if (offset >= m_size)
        return 0;
    len = std::min(len, m_size - offset);
    quint64 done = 0;
    while (done < len) {
        const quint64 at = offset + done;
        const quint64 piece = std::min(kPiece - at % kPiece, len - done);
        if (unreadable(at, piece) || !rawRead(at, buf + done, piece)) {
            // Try again in small pieces, so one bad spot doesn't cost the whole megabyte.
            for (quint64 small = 0; small < piece;) {
                const quint64 n = std::min(kSmallPiece - (at + small) % kSmallPiece, piece - small);
                if (unreadable(at + small, n) || !rawRead(at + small, buf + done + small, n)) {
                    memset(buf + done + small, 0, n);
                    markBad(at + small, n);
                }
                small += n;
            }
        }
        done += piece;
    }
    return len;
}

bool lost::Source::unreadable(quint64 offset, quint64 len) const
{
    QMutexLocker locker(&m_lock);
    // m_bad is sorted: the first one that ends after `offset` is the only one that can overlap.
    const auto it = std::lower_bound(m_bad.cbegin(), m_bad.cend(), offset, [](const Extent &e, quint64 value) { return e.end() <= value; });
    return it != m_bad.cend() && it->start < offset + len;
}

QVector<lost::Extent> lost::Source::unreadableParts() const
{
    QMutexLocker locker(&m_lock);
    return m_bad;
}
