// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "stickcheck.h"

#include "applog.h"
#include "blockio.h"

#include <QDateTime>
#include <QRandomGenerator>

#include <algorithm>
#include <cstring>
#include <unistd.h>

namespace {

constexpr quint64 kMiB = 1024 * 1024;
constexpr qsizetype kSpot = 4096;   // a quick check's stamp
constexpr quint64 kBlock = kMiB;    // a full check's block
constexpr int kMaxSpots = 65536;
constexpr int kTooManyBad = 256;    // like Rufus: past this, it's fake or failing either way
constexpr char kMagic[8] = {'D', 'F', 'C', 'H', 'E', 'C', 'K', '1'};

void putLe64(char *p, quint64 v)
{
    for (int i = 0; i < 8; ++i)
        p[i] = char(v >> (8 * i));
}

quint64 getLe64(const char *p)
{
    quint64 v = 0;
    for (int i = 0; i < 8; ++i)
        v |= quint64(uchar(p[i])) << (8 * i);
    return v;
}

// The stamp, then data that depends on the run, the offset and the pass. The second pass of a
// full check writes every bit the other way round.
void fill(char *buffer, qsizetype length, quint64 run, quint64 offset, int pass)
{
    std::memcpy(buffer, kMagic, 8);
    putLe64(buffer + 8, run);
    putLe64(buffer + 16, offset);
    putLe64(buffer + 24, quint64(pass));
    quint64 x = run ^ (offset * 0x9e3779b97f4a7c15ULL) ^ 0x2545f4914f6cdd1dULL;
    for (qsizetype i = 32; i + 8 <= length; i += 8) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        putLe64(buffer + i, pass % 2 ? ~x : x);
    }
}

enum Seen { Fine, ReadFailed, Wrong, Alias };

// What came back, compared with what should have: fine, someone else's stamp (where from), or
// something else.
Seen classify(const char *got, const char *expected, qsizetype length, quint64 run, quint64 *from)
{
    if (std::memcmp(got, expected, size_t(length)) == 0)
        return Fine;
    if (std::memcmp(got, kMagic, 8) == 0 && getLe64(got + 8) == run && getLe64(got + 16) != getLe64(expected + 16)) {
        *from = getLe64(got + 16);
        return Alias;
    }
    return Wrong;
}

class FdTarget : public stickcheck::Target
{
public:
    explicit FdTarget(int fd)
        : m_fd(fd)
        , m_size(blockio::deviceSize(fd))
    {
    }
    ~FdTarget() override
    {
        if (m_fd >= 0)
            blockio::close(m_fd);
    }
    quint64 size() const override { return m_size; }
    bool write(quint64 offset, const char *data, qsizetype length) override
    {
        return blockio::writeAt(m_fd, data, size_t(length), offset);
    }
    bool read(quint64 offset, char *data, qsizetype length) override
    {
        return blockio::readAt(m_fd, data, size_t(length), offset);
    }
    bool flush() override { return blockio::flush(m_fd); }

private:
    int m_fd;
    quint64 m_size;
};

} // namespace

std::unique_ptr<stickcheck::Target> stickcheck::fromFd(int fd)
{
    return std::make_unique<FdTarget>(fd);
}

QVector<quint64> stickcheck::quickSpots(quint64 size)
{
    QVector<quint64> spots;
    if (size < quint64(kSpot))
        return spots;
    // A stamp at least every MiB, and no more than 65536 of them (a 1 TB stick gets one every
    // 16 MiB). Fake sticks wrap around at a whole number of MiB, so a high write lands on a
    // lower stamp and shows.
    const quint64 blocks = size / kMiB;
    const quint64 stride = std::max<quint64>(1, (blocks + kMaxSpots - 1) / kMaxSpots) * kMiB;
    for (quint64 at = 0; at + quint64(kSpot) <= size; at += stride)
        spots.push_back(at);
    const quint64 last = (size - quint64(kSpot)) / quint64(kSpot) * quint64(kSpot);
    if (spots.isEmpty() || spots.last() != last)
        spots.push_back(last);
    return spots;
}

double stickcheck::estimate(Mode mode, quint64 size)
{
    // A slow USB 2 stick: small writes take a few ms, it writes 10 MB/s and reads 30 MB/s.
    if (mode == Mode::Quick)
        return quickSpots(size).size() * 0.005;
    const double pass = double(size) / 10e6 + double(size) / 30e6;
    return mode == Mode::FullTwice ? 2 * pass : pass;
}

stickcheck::Checker::Checker(std::unique_ptr<Target> target, Mode mode)
    : m_target(std::move(target))
    , m_mode(mode)
    , m_run(QRandomGenerator::system()->generate64())
{
    qRegisterMetaType<stickcheck::Mark>();
    qRegisterMetaType<QVector<stickcheck::Mark>>();
    qRegisterMetaType<stickcheck::Result>();
}

stickcheck::Checker::~Checker() = default;

void stickcheck::Checker::mark(quint64 offset, quint64 length, Mark::State state)
{
    if (!m_pending.isEmpty() && m_pending.last().state == state && m_pending.last().offset + m_pending.last().length == offset)
        m_pending.last().length += length;
    else
        m_pending.push_back({offset, length, state});
    flushMarks(false);
}

void stickcheck::Checker::flushMarks(bool force)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_pending.isEmpty() || (!force && now - m_lastMarks < 250))
        return;
    m_lastMarks = now;
    emit marks(m_pending);
    m_pending.clear();
}

void stickcheck::Checker::verdict(Result &r, const QVector<quint64> &checked, const QVector<int> &seen,
                                  const QVector<quint64> &aliases) const
{
    // Wrapped: a low block shows the stamp of one higher up, a whole real size further on.
    quint64 wrap = 0;
    for (int i = 0; i < checked.size(); ++i) {
        if (seen[i] == Alias && aliases[i] > checked[i]) {
            const quint64 d = aliases[i] - checked[i];
            wrap = wrap ? std::min(wrap, d) : d;
        }
    }
    if (wrap) {
        r.fake = true;
        r.wraps = true;
        r.real = wrap;
        return;
    }
    // Writes past the real end don't stick: from some point on, (almost) nothing reads back.
    int first = -1;
    for (int i = 0; i < seen.size() && first < 0; ++i) {
        if (seen[i] != Fine)
            first = i;
    }
    if (first < 0)
        return;
    int badAfter = 0;
    for (int i = first; i < seen.size(); ++i)
        badAfter += seen[i] != Fine;
    // Everything from there to the very end, or nearly everything over a longer stretch: that's
    // where the stick really ends (or stops working, which comes to the same).
    const int after = int(seen.size()) - first;
    if ((after >= 2 && badAfter == after) || (after >= 10 && badAfter * 10 >= after * 9)) {
        r.fake = true;
        r.real = checked[first];
    }
}

void stickcheck::Checker::quick(Result &r)
{
    const QVector<quint64> spots = quickSpots(r.claimed);
    const quint64 stride = spots.size() > 1 ? spots[1] - spots[0] : quint64(kSpot);
    auto write = blockio::alignedBuffer(kSpot);
    auto read = blockio::alignedBuffer(kSpot);
    auto expected = blockio::alignedBuffer(kSpot);
    QVector<int> seen(spots.size(), Fine);
    QVector<quint64> aliases(spots.size(), 0);
    const quint64 total = quint64(spots.size()) * 2;

    for (int i = 0; i < spots.size(); ++i) {
        if (m_cancel)
            return;
        fill(write.get(), kSpot, m_run, spots[i], 0);
        if (!m_target->write(spots[i], write.get(), kSpot)) {
            ++r.writeErrors;
            seen[i] = Wrong;
        }
        mark(spots[i], std::min(stride, r.claimed - spots[i]), Mark::Written);
        if (i % 64 == 0)
            emit progress(tr("Writing stamps"), quint64(i), total);
    }
    // Out of the stick's own cache too, as far as it lets us.
    m_target->flush();

    for (int i = 0; i < spots.size(); ++i) {
        if (m_cancel)
            return;
        const quint64 length = std::min(stride, r.claimed - spots[i]);
        if (seen[i] == Wrong) { // the write failed already
            if (r.bad.size() < kTooManyBad)
                r.bad.push_back(spots[i]);
            mark(spots[i], length, Mark::Bad);
            continue;
        }
        fill(expected.get(), kSpot, m_run, spots[i], 0);
        if (!m_target->read(spots[i], read.get(), kSpot)) {
            ++r.readErrors;
            seen[i] = ReadFailed;
        } else {
            seen[i] = classify(read.get(), expected.get(), kSpot, m_run, &aliases[i]);
            if (seen[i] != Fine)
                ++r.wrongData;
        }
        if (seen[i] != Fine && r.bad.size() < kTooManyBad)
            r.bad.push_back(spots[i]);
        mark(spots[i], length, seen[i] == Fine ? Mark::Good : Mark::Bad);
        if (i % 64 == 0)
            emit progress(tr("Reading them back"), quint64(spots.size() + i), total);
    }
    verdict(r, spots, seen, aliases);
    r.completed = true;
}

void stickcheck::Checker::full(Result &r, int pass, int passes)
{
    const quint64 blocks = (r.claimed + kBlock - 1) / kBlock;
    auto write = blockio::alignedBuffer(kBlock);
    auto read = blockio::alignedBuffer(kBlock);
    auto expected = blockio::alignedBuffer(kBlock);
    const QString passText = passes > 1 ? tr(" (pass %1 of %2)").arg(pass + 1).arg(passes) : QString();
    auto lengthAt = [&r](quint64 at) {
        // The last block can be short; keep it a whole number of 512-byte sectors.
        return qsizetype(std::min<quint64>(kBlock, (r.claimed - at) / 512 * 512));
    };

    QVector<int> writeFailed(int(blocks), 0);
    for (quint64 b = 0; b < blocks; ++b) {
        if (m_cancel)
            return;
        const quint64 at = b * kBlock;
        const qsizetype length = lengthAt(at);
        if (length <= 0)
            break;
        fill(write.get(), length, m_run, at, pass);
        if (!m_target->write(at, write.get(), length)) {
            ++r.writeErrors;
            writeFailed[int(b)] = 1;
        }
        mark(at, quint64(length), Mark::Written);
        if (b % 16 == 0)
            emit progress(tr("Writing") + passText, at, r.claimed);
    }
    m_target->flush();

    QVector<quint64> checked;
    QVector<int> seen;
    QVector<quint64> aliases;
    int bad = 0;
    for (quint64 b = 0; b < blocks && bad < kTooManyBad; ++b) {
        if (m_cancel)
            return;
        const quint64 at = b * kBlock;
        const qsizetype length = lengthAt(at);
        if (length <= 0)
            break;
        quint64 from = 0;
        int s = Fine;
        if (writeFailed[int(b)]) {
            s = Wrong;
        } else {
            fill(expected.get(), length, m_run, at, pass);
            if (!m_target->read(at, read.get(), length)) {
                ++r.readErrors;
                s = ReadFailed;
            } else {
                s = classify(read.get(), expected.get(), length, m_run, &from);
                if (s != Fine)
                    ++r.wrongData;
            }
        }
        checked.push_back(at);
        seen.push_back(s);
        aliases.push_back(from);
        if (s != Fine) {
            ++bad;
            if (r.bad.size() < kTooManyBad)
                r.bad.push_back(at);
        }
        mark(at, quint64(length), s == Fine ? Mark::Good : Mark::Bad);
        if (b % 16 == 0)
            emit progress(tr("Reading it back") + passText, at, r.claimed);
    }
    verdict(r, checked, seen, aliases);
    r.completed = true;
}

void stickcheck::Checker::run()
{
    Result r;
    r.claimed = m_target->size();
    r.real = r.claimed;
    if (r.claimed < quint64(kSpot)) {
        r.error = tr("The stick is too small to check.");
        emit finished(r);
        return;
    }
    qCInfo(lcOps).noquote() << "Check a USB Stick:" << (m_mode == Mode::Quick ? "quick" : m_mode == Mode::Full ? "full" : "full, twice")
                            << "on" << r.claimed << "bytes";
    if (m_mode == Mode::Quick) {
        quick(r);
    } else {
        full(r, 0, m_mode == Mode::FullTwice ? 2 : 1);
        if (r.completed && !r.fake && m_mode == Mode::FullTwice) {
            r.completed = false;
            full(r, 1, 2);
        }
    }
    flushMarks(true);
    if (m_cancel) {
        r.completed = false;
        r.error = tr("Stopped.");
    }
    qCInfo(lcOps).noquote() << "Check a USB Stick:" << (r.completed ? "done" : "stopped") << "fake" << r.fake << "real" << r.real
                            << "write errors" << r.writeErrors << "read errors" << r.readErrors << "wrong data" << r.wrongData;
    emit finished(r);
}
