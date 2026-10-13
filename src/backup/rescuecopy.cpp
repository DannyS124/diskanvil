// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "rescuecopy.h"

#include "blockio.h"
#include "format.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QThread>
#include <QSaveFile>
#include <QTextStream>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

RescueMap::RescueMap(quint64 size)
    : m_size(size)
{
    if (size > 0)
        m_blocks.append({0, size, NonTried});
}

int RescueMap::indexAt(quint64 pos) const
{
    // Last block that starts at or before pos.
    auto it = std::upper_bound(m_blocks.cbegin(), m_blocks.cend(), pos, [](quint64 p, const Block &b) { return p < b.pos; });
    return int(it - m_blocks.cbegin()) - 1;
}

quint64 RescueMap::missing(quint64 pos, quint64 length) const
{
    const quint64 end = std::min(pos + length, m_size);
    quint64 out = 0;
    for (int i = std::max(0, indexAt(pos)); i < m_blocks.size() && m_blocks[i].pos < end; ++i) {
        const Block &b = m_blocks[i];
        if (b.status == Finished)
            continue;
        const quint64 from = std::max(pos, b.pos), to = std::min(end, b.end());
        if (to > from)
            out += to - from;
    }
    return out;
}

char RescueMap::statusAt(quint64 pos) const
{
    const int i = indexAt(pos);
    return i >= 0 && pos < m_blocks[i].end() ? m_blocks[i].status : char(NonTried);
}

void RescueMap::set(quint64 pos, quint64 size, char status)
{
    if (size == 0 || pos >= m_size)
        return;
    size = std::min(size, m_size - pos);
    const quint64 end = pos + size;
    int first = indexAt(pos);
    int last = indexAt(end - 1);
    const Block head = m_blocks[first];
    const Block tail = m_blocks[last];

    QVector<Block> replacement;
    if (head.pos < pos)
        replacement.append({head.pos, pos - head.pos, head.status});
    replacement.append({pos, size, status});
    if (tail.end() > end)
        replacement.append({end, tail.end() - end, tail.status});
    m_blocks.remove(first, last - first + 1);
    m_blocks.insert(first, replacement.size(), Block());
    std::copy(replacement.cbegin(), replacement.cend(), m_blocks.begin() + first);

    // Merge with neighbours of the same status.
    const int from = std::max(first - 1, 0);
    const int to = std::min(first + int(replacement.size()), int(m_blocks.size()) - 1);
    for (int i = to; i > from; --i) {
        if (m_blocks[i].status == m_blocks[i - 1].status) {
            m_blocks[i - 1].size += m_blocks[i].size;
            m_blocks.remove(i);
        }
    }
}

bool RescueMap::next(char status, quint64 from, quint64 *start, quint64 *end) const
{
    for (int i = std::max(indexAt(from), 0); i < m_blocks.size(); ++i) {
        const Block &b = m_blocks[i];
        if (b.status == status && b.end() > from) {
            *start = std::max(b.pos, from);
            *end = b.end();
            return true;
        }
    }
    return false;
}

bool RescueMap::previous(char status, quint64 before, quint64 *start, quint64 *end) const
{
    for (int i = std::min(indexAt(before == 0 ? 0 : before - 1), int(m_blocks.size()) - 1); i >= 0; --i) {
        const Block &b = m_blocks[i];
        if (b.status == status && b.pos < before) {
            *start = b.pos;
            *end = std::min(b.end(), before);
            return true;
        }
    }
    return false;
}

quint64 RescueMap::total(char status) const
{
    quint64 sum = 0;
    for (const Block &b : m_blocks)
        sum += b.status == status ? b.size : 0;
    return sum;
}

bool RescueMap::save(const QString &path, QString *error) const
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        *error = f.errorString();
        return false;
    }
    QTextStream out(&f);
    out << "# Mapfile. Created by DiskAnvil " APP_VERSION " (GNU ddrescue format)\n"
        << "# Current time: " << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) << "\n";
    if (done())
        out << "# Finished\n";
    out << "# current_pos  current_status  current_pass\n";
    out << QStringLiteral("0x%1     %2               %3\n").arg(currentPos, 8, 16, QLatin1Char('0')).arg(QLatin1Char(currentStatus)).arg(currentPass);
    out << "#      pos        size  status\n";
    for (const Block &b : m_blocks)
        out << QStringLiteral("0x%1  0x%2  %3\n").arg(b.pos, 8, 16, QLatin1Char('0')).arg(b.size, 8, 16, QLatin1Char('0')).arg(QLatin1Char(b.status));
    out.flush();
    if (!f.commit()) {
        *error = f.errorString();
        return false;
    }
    return true;
}

bool RescueMap::load(const QString &path, quint64 expectedSize, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        *error = f.errorString();
        return false;
    }
    // A map of a badly damaged drive runs to a few megabytes; far bigger isn't a map.
    if (f.size() > 256 * 1024 * 1024) {
        *error = QObject::tr("The map file is too big to be a map");
        return false;
    }
    QVector<Block> blocks;
    bool sawCurrent = false;
    for (const QByteArray &raw : f.readAll().split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        const QList<QByteArray> fields = line.simplified().split(' ');
        bool ok1 = false, ok2 = false;
        if (!sawCurrent) {
            currentPos = fields.value(0).toULongLong(&ok1, 0);
            currentStatus = fields.value(1).isEmpty() ? '?' : fields.value(1).at(0);
            currentPass = fields.size() > 2 ? fields.value(2).toInt() : 1;
            sawCurrent = true;
            continue;
        }
        Block b;
        b.pos = fields.value(0).toULongLong(&ok1, 0);
        b.size = fields.value(1).toULongLong(&ok2, 0);
        b.status = fields.value(2).isEmpty() ? '\0' : fields.value(2).at(0);
        if (!ok1 || !ok2 || !QByteArray("?*/-+").contains(b.status)) {
            *error = QObject::tr("The map file has a line it doesn't understand: %1").arg(QString::fromLatin1(line));
            return false;
        }
        blocks.append(b);
    }
    // Blocks must cover the drive from 0 without gaps (and no block may run past its end,
    // or huge sizes could wrap around and still add up).
    quint64 pos = 0;
    for (const Block &b : blocks) {
        if (b.pos != pos || b.size > expectedSize - pos) {
            *error = QObject::tr("The map file has gaps or overlaps");
            return false;
        }
        pos = b.end();
    }
    if (pos != expectedSize) {
        *error = QObject::tr("The map file is for a drive of a different size (%1, this one is %2)").arg(formatSize(pos), formatSize(expectedSize));
        return false;
    }
    if (currentPos > expectedSize)
        currentPos = 0;
    m_size = expectedSize;
    m_blocks.clear();
    for (const Block &b : blocks) {
        if (!m_blocks.isEmpty() && m_blocks.last().status == b.status)
            m_blocks.last().size += b.size;
        else
            m_blocks.append(b);
    }
    return true;
}

RescueCopy::RescueCopy(int sourceFd, quint64 size, int targetFd, const QString &mapPath)
    : m_source(sourceFd)
    , m_target(targetFd)
    , m_size(size)
    , m_mapPath(mapPath)
    , m_map(size)
{
    qRegisterMetaType<QVector<RescueMap::Block>>();
}

RescueCopy::~RescueCopy()
{
    if (m_source >= 0)
        blockio::close(m_source);
    if (m_target >= 0)
        blockio::close(m_target);
}

bool RescueCopy::readSource(char *buf, quint64 len, quint64 pos)
{
    waitWhilePaused();
    // Read even when stopping meanwhile: a read that didn't happen mustn't count as bad.
    const bool ok = blockio::readAt(m_source, buf, len, pos);
    if (!ok)
        ++m_failedReads;
    coolDown(ok, len);
    return ok;
}

void RescueCopy::waitWhilePaused()
{
    if (!m_paused || m_cancel)
        return;
    const QString was = m_phase;
    m_phase = tr("Resting while the drive cools down");
    tick(true); // saves the map too
    while (m_paused && !m_cancel)
        QThread::msleep(200);
    m_phase = was;
    m_rateClock.invalidate(); // no catching up on the speed limit after a rest
    tick(true);
}

void RescueCopy::coolDown(bool ok, quint64 len)
{
    // Some failing drives do better after a rest than when hammered with retries.
    m_errorsInARow = ok ? 0 : m_errorsInARow + 1;
    const int restAfter = m_restAfter;
    if (restAfter > 0 && m_errorsInARow >= restAfter) {
        m_errorsInARow = 0;
        const int seconds = m_restSeconds;
        const QString was = m_phase;
        m_phase = tr("Resting for %n second(s) after %1 read errors in a row", nullptr, seconds).arg(restAfter);
        tick(true);
        for (int i = 0; i < seconds * 5 && !m_cancel; ++i)
            QThread::msleep(200);
        m_phase = was;
        m_rateClock.invalidate();
        tick(true);
    }
    // The speed limit: wait until what's been read fits under it. Measured over windows of
    // about ten seconds, so a change of limit or a rest starts afresh.
    const quint64 rate = m_maxRate;
    if (rate == 0)
        return;
    if (!m_rateClock.isValid() || rate != m_rateSeen || m_rateClock.elapsed() > 10000) {
        m_rateClock.start();
        m_rateBytes = 0;
        m_rateSeen = rate;
    }
    m_rateBytes += len;
    const qint64 due = qint64(double(m_rateBytes) * 1000.0 / double(rate));
    while (!m_cancel) {
        const qint64 ahead = due - m_rateClock.elapsed();
        if (ahead <= 0)
            break;
        QThread::msleep(quint64(std::min<qint64>(ahead, 200)));
    }
}

bool RescueCopy::writeTarget(const char *buf, quint64 len, quint64 pos)
{
    if (blockio::writeAt(m_target, buf, len, pos))
        return true;
    m_writeError = QString::fromLocal8Bit(std::strerror(errno));
    return false;
}

void RescueCopy::failedStretch(quint64 start, quint64 length)
{
    m_stretches.push_back({start, length});
    if (!m_skipHeads || m_stretches.size() < 4)
        return;
    // The last four: three gaps between their starts, within 15% of each other, and bands much
    // narrower than the gaps (a wide bad area isn't a head).
    const int n = int(m_stretches.size());
    QVector<quint64> gaps, widths;
    for (int i = n - 3; i < n; ++i) {
        gaps << m_stretches[i].first - m_stretches[i - 1].first;
        widths << m_stretches[i].second;
    }
    widths << m_stretches[n - 4].second;
    std::sort(gaps.begin(), gaps.end());
    std::sort(widths.begin(), widths.end());
    if (gaps.first() == 0 || double(gaps.last()) > double(gaps.first()) * 1.15 || widths.last() * 5 > gaps[1] * 3)
        return;
    const bool first = m_period == 0;
    m_period = gaps[1];
    m_band = widths.last() + widths.last() / 4; // a little wider: the edges are found later
    m_anchor = m_stretches.back().first;
    if (first)
        m_phase = tr("Copying the easy parts (a dying head: its stripes are left for last)");
}

bool RescueCopy::nextBand(quint64 pos, quint64 *start, quint64 *end) const
{
    if (!m_period)
        return false;
    // Bands at anchor + k * period; the first one that isn't over yet.
    quint64 k = pos > m_anchor ? (pos - m_anchor) / m_period : 0;
    quint64 s = m_anchor + k * m_period;
    if (s + m_band <= pos)
        s += m_period;
    *start = s;
    *end = std::min(m_size, s + m_band);
    return s < m_size;
}

bool RescueCopy::inHeadZone(quint64 pos, quint64 len, quint64 *zoneStart) const
{
    for (const auto &[start, length] : m_headZones) {
        if (pos < start + length && start < pos + len) {
            *zoneStart = start;
            return true;
        }
    }
    return false;
}

void RescueCopy::tick(bool force)
{
    const qint64 now = m_clock.elapsed();
    if (force || now - m_lastSignal >= 250) {
        m_lastSignal = now;
        emit progress(m_map.total(RescueMap::Finished), m_map.total(RescueMap::Bad), m_size, m_phase);
        emit mapChanged(m_map.blocks());
    }
    if (force || now - m_lastSave >= 30000) {
        m_lastSave = now;
        QString ignored;
        m_map.save(m_mapPath, &ignored);
    }
}

void RescueCopy::run()
{
    m_clock.start();
    if (QFile::exists(m_mapPath)) {
        QString error;
        if (!m_map.load(m_mapPath, m_size, &error)) {
            emit finished(false, tr("Couldn't continue from the map file: %1").arg(error));
            return;
        }
    }
    m_sector = blockio::logicalSize(m_source);
    const quint64 sector = quint64(m_sector);
    constexpr quint64 kChunk = 1024 * 1024;
    blockio::Buffer buf = blockio::alignedBuffer(kChunk);
    if (!buf) {
        emit finished(false, tr("Out of memory"));
        return;
    }
    // An image file grows to the drive's size up front; unreadable parts stay zeros.
    if (blockio::isRegularFile(m_target) && blockio::deviceSize(m_target) < m_size && !blockio::setFileSize(m_target, m_size)) {
        emit finished(false, tr("Couldn't make the image file: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
        return;
    }

    auto stopped = [this] {
        tick(true);
        if (!m_writeError.isEmpty())
            emit finished(false, tr("Couldn't write the copy: %1. Progress is saved; fix that and start again to continue.").arg(m_writeError));
        else
            emit finished(false, tr("Stopped. Progress is saved; start again with the same image to continue."));
    };
    // One piece: copy it if it reads, otherwise give it `failed` status.
    auto attempt = [&](quint64 pos, quint64 len, char failed) {
        m_map.currentPos = pos;
        if (readSource(buf.get(), len, pos)) {
            if (!writeTarget(buf.get(), len, pos))
                return false;
            m_map.set(pos, len, RescueMap::Finished);
        } else {
            m_map.set(pos, len, failed);
        }
        tick();
        return true;
    };

    // 1: forwards, jumping ahead after a failed read so a bad patch doesn't eat hours.
    m_phase = tr("Copying the easy parts");
    m_map.currentPass = 1;
    tick(true); // each pass shows up, however short
    m_map.currentStatus = '?';
    const quint64 maxSkip = std::max<quint64>(kChunk, std::min<quint64>(m_size / 100, 1024ull * kChunk) / sector * sector);
    quint64 start = 0, end = 0, from = 0, skip = 64 * 1024;
    bool failing = false;
    quint64 failedFrom = 0;
    while (m_map.next(RescueMap::NonTried, from, &start, &end)) {
        for (quint64 pos = start; pos < end;) {
            if (m_cancel)
                return stopped();
            // A dying head's band coming up: jumped over, left as it is for the end.
            quint64 bandStart = 0, bandEnd = 0;
            if (!failing && nextBand(pos, &bandStart, &bandEnd) && bandStart <= pos + kChunk && bandEnd > pos) {
                const quint64 jumpFrom = std::max(pos, bandStart > kChunk ? (bandStart - kChunk) / sector * sector : 0);
                const quint64 to = std::min(end, (bandEnd + sector - 1) / sector * sector);
                if (to > jumpFrom) {
                    if (jumpFrom > pos) {
                        // The good part before it first.
                        const quint64 len = std::min(kChunk, jumpFrom - pos);
                        if (!attempt(pos, len, RescueMap::NonTrimmed))
                            return stopped();
                        pos += len;
                        continue;
                    }
                    m_headZones.push_back({jumpFrom, to - jumpFrom});
                    pos = to;
                    continue;
                }
            }
            const quint64 len = std::min(kChunk, end - pos);
            const bool good = readSource(buf.get(), len, pos);
            m_map.currentPos = pos;
            if (good) {
                if (!writeTarget(buf.get(), len, pos))
                    return stopped();
                m_map.set(pos, len, RescueMap::Finished);
                skip = 64 * 1024;
                if (failing)
                    failedStretch(failedFrom, pos - failedFrom);
                failing = false;
                pos += len;
            } else {
                m_map.set(pos, len, RescueMap::NonTrimmed);
                if (!failing)
                    failedFrom = pos;
                failing = true;
                pos = std::min(end, pos + len + skip);
                skip = std::min(skip * 2, maxSkip);
            }
            tick();
        }
        from = end;
    }

    // 2: backwards over what was skipped; bad areas are often easier from the other side.
    m_phase = tr("Copying backwards");
    m_map.currentPass = 2;
    tick(true); // each pass shows up, however short
    quint64 before = m_size;
    while (m_map.previous(RescueMap::NonTried, before, &start, &end)) {
        for (quint64 stop = end; stop > start;) {
            if (m_cancel)
                return stopped();
            const quint64 len = std::min(kChunk, stop - start);
            quint64 zone = 0;
            if (inHeadZone(stop - len, len, &zone)) {
                stop = std::max(start, zone); // left for last
                continue;
            }
            if (!attempt(stop - len, len, RescueMap::NonTrimmed))
                return stopped();
            stop -= len;
        }
        before = start;
    }

    // 3 and 4, again after the dying head's stripes.
    auto trimAndScrape = [&]() -> bool {
        // 3: trim each failed area from both ends, one sector at a time, until it hits a bad one.
        m_phase = tr("Narrowing down bad areas");
        m_map.currentPass = 3;
        tick(true); // each pass shows up, however short
        m_map.currentStatus = '*';
        from = 0;
        while (m_map.next(RescueMap::NonTrimmed, from, &start, &end)) {
            quint64 low = start, high = end;
            while (low < high) {
                if (m_cancel)
                    return false;
                m_map.currentPos = low;
                if (!readSource(buf.get(), sector, low)) {
                    m_map.set(low, sector, RescueMap::Bad);
                    low += sector;
                    break;
                }
                if (!writeTarget(buf.get(), sector, low))
                    return false;
                m_map.set(low, sector, RescueMap::Finished);
                low += sector;
                tick();
            }
            while (high > low) {
                if (m_cancel)
                    return false;
                m_map.currentPos = high - sector;
                if (!readSource(buf.get(), sector, high - sector)) {
                    m_map.set(high - sector, sector, RescueMap::Bad);
                    high -= sector;
                    break;
                }
                if (!writeTarget(buf.get(), sector, high - sector))
                    return false;
                m_map.set(high - sector, sector, RescueMap::Finished);
                high -= sector;
                tick();
            }
            if (high > low)
                m_map.set(low, high - low, RescueMap::NonScraped);
            from = end;
        }

        // 4: everything left, one sector at a time.
        m_phase = tr("Reading what's left sector by sector");
        m_map.currentPass = 4;
        tick(true); // each pass shows up, however short
        m_map.currentStatus = '/';
        from = 0;
        while (m_map.next(RescueMap::NonScraped, from, &start, &end)) {
            for (quint64 pos = start; pos < end; pos += sector) {
                if (m_cancel)
                    return false;
                if (!attempt(pos, sector, RescueMap::Bad))
                    return false;
            }
            from = end;
        }
        return true;
    };
    if (!trimAndScrape())
        return stopped();

    // 5: last, the dying head's stripes that were jumped over: forwards with the usual skipping,
    // then backwards, then trimmed and scraped like the rest.
    if (!m_headZones.isEmpty()) {
        m_phase = tr("Copying the dying head's stripes, last");
        m_map.currentPass = 5;
        tick(true); // each pass shows up, however short
        m_map.currentStatus = '?';
        for (const auto &[zoneStart, zoneLength] : std::as_const(m_headZones)) {
            quint64 pos = zoneStart, skipHere = 64 * 1024;
            const quint64 zoneEnd = zoneStart + zoneLength;
            while (pos < zoneEnd) {
                if (m_cancel)
                    return stopped();
                if (m_map.statusAt(pos) != RescueMap::NonTried) {
                    pos += sector;
                    continue;
                }
                quint64 s = 0, e = 0;
                m_map.next(RescueMap::NonTried, pos, &s, &e);
                const quint64 len = std::min({kChunk, zoneEnd - pos, e - pos});
                const bool good = readSource(buf.get(), len, pos);
                m_map.currentPos = pos;
                if (good) {
                    if (!writeTarget(buf.get(), len, pos))
                        return stopped();
                    m_map.set(pos, len, RescueMap::Finished);
                    skipHere = 64 * 1024;
                    pos += len;
                } else {
                    m_map.set(pos, len, RescueMap::NonTrimmed);
                    pos = std::min(zoneEnd, pos + len + skipHere);
                    skipHere = std::min(skipHere * 2, maxSkip);
                }
                tick();
            }
            for (quint64 stop = zoneEnd; stop > zoneStart;) {
                if (m_cancel)
                    return stopped();
                const quint64 len = std::min(kChunk, stop - zoneStart);
                if (m_map.statusAt(stop - len) == RescueMap::NonTried && m_map.statusAt(stop - 1) == RescueMap::NonTried) {
                    if (!attempt(stop - len, len, RescueMap::NonTrimmed))
                        return stopped();
                } else {
                    // Partly done: what's left of it, a sector at a time is too slow; the chunk's
                    // untried pieces go to trimming instead.
                    for (quint64 p = stop - len; p < stop; p += sector)
                        if (m_map.statusAt(p) == RescueMap::NonTried)
                            m_map.set(p, sector, RescueMap::NonTrimmed);
                }
                stop -= len;
            }
        }
        if (!trimAndScrape())
            return stopped();
    }

    // What couldn't be read becomes zeros in the copy, not whatever the target held before.
    std::memset(buf.get(), 0, kChunk);
    for (const RescueMap::Block &b : m_map.blocks()) {
        if (b.status != RescueMap::Bad)
            continue;
        for (quint64 pos = b.pos; pos < b.end(); pos += kChunk) {
            if (!writeTarget(buf.get(), std::min(kChunk, b.end() - pos), pos))
                return stopped();
        }
    }
    if (!blockio::flush(m_target)) {
        m_writeError = QString::fromLocal8Bit(std::strerror(errno));
        return stopped();
    }
    m_map.currentStatus = '+';
    m_map.currentPos = 0;
    tick(true);
    const quint64 bad = m_map.total(RescueMap::Bad);
    if (bad == 0)
        emit finished(true, tr("Everything was copied: %1.").arg(formatSize(m_size)));
    else
        emit finished(true, tr("Copied %1. %2 couldn't be read and are zeros in the copy.")
                                .arg(formatSize(m_map.total(RescueMap::Finished)), formatSize(bad)));
}
