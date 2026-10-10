// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "blockmapdata.h"

#include <algorithm>

namespace {

constexpr quint64 kMinCellBytes = 1024 * 1024;

} // namespace

BlockMapData::BlockMapData(quint64 size, bool rotational)
    : m_size(size)
    // A healthy hard drive reads 1 MiB in 5-30 ms and an SSD in 1-5 ms; a sector the
    // drive has to retry takes hundreds.
    , m_slowFloorMs(rotational ? 150 : 50)
{
    const quint64 cells = std::clamp<quint64>((size + kMinCellBytes - 1) / kMinCellBytes, 1, kMaxCells);
    m_cells.resize(qsizetype(cells));
}

quint64 BlockMapData::cellStart(int cell) const
{
    return quint64(cell) * m_size / quint64(m_cells.size());
}

quint64 BlockMapData::cellEnd(int cell) const
{
    return quint64(cell + 1) * m_size / quint64(m_cells.size());
}

int BlockMapData::cellAt(quint64 offset) const
{
    if (m_size == 0)
        return 0;
    // cellStart rounds down, so step back if the estimate overshoots.
    int cell = int(std::min<quint64>(offset * quint64(m_cells.size()) / m_size, quint64(m_cells.size() - 1)));
    while (cell > 0 && cellStart(cell) > offset)
        --cell;
    while (cell + 1 < m_cells.size() && cellEnd(cell) <= offset)
        ++cell;
    return cell;
}

template <typename F> void BlockMapData::forCells(quint64 offset, quint64 length, F f)
{
    if (m_cells.isEmpty() || length == 0 || offset >= m_size)
        return;
    const int first = cellAt(offset);
    const int last = cellAt(std::min(offset + length, m_size) - 1);
    for (int c = first; c <= last; ++c)
        f(m_cells[c]);
}

void BlockMapData::add(const ReadSample &s)
{
    if (s.readable) {
        ++m_histogram[std::min<quint32>(s.ms, quint32(m_histogram.size() - 1))];
        ++m_samples;
        m_median = -1;
    }
    forCells(s.offset, s.length, [&](Cell &c) {
        c.read = true;
        if (s.readable)
            c.worstMs = std::max(c.worstMs, s.ms);
        else
            c.marked = State::Bad;
    });
}

void BlockMapData::add(const QVector<ReadSample> &samples)
{
    for (const ReadSample &s : samples)
        add(s);
}

void BlockMapData::clearStates()
{
    for (Cell &c : m_cells)
        c = Cell();
}

void BlockMapData::mark(quint64 offset, quint64 length, State state)
{
    forCells(offset, length, [&](Cell &c) {
        if (state == State::Good || state == State::Slow)
            c.read = true;
        if (state > c.marked && state != State::Good)
            c.marked = state;
    });
}

void BlockMapData::markChecked(quint64 offset, quint64 length)
{
    forCells(offset, length, [&](Cell &c) {
        c.read = true;
        if (c.marked == State::Pending)
            c.marked = State::Unread;
    });
}

BlockMapData::State BlockMapData::state(int cell) const
{
    const Cell &c = m_cells[cell];
    if (c.marked == State::Bad || c.marked == State::Pending)
        return c.marked;
    if (!c.read)
        return State::Unread;
    return isSlow(c.worstMs) ? State::Slow : State::Good;
}

quint32 BlockMapData::medianMs() const
{
    if (m_median >= 0)
        return quint32(m_median);
    m_median = 0;
    quint64 seen = 0;
    for (size_t ms = 0; ms < m_histogram.size() && m_samples > 0; ++ms) {
        seen += m_histogram[ms];
        if (seen * 2 >= m_samples) {
            m_median = qint64(ms);
            break;
        }
    }
    return quint32(m_median);
}

bool BlockMapData::isSlow(quint32 ms) const
{
    // Slow compared to the rest of this drive, not just slow: a USB stick that takes
    // 80 ms for every read is simply a slow stick.
    return ms >= m_slowFloorMs && ms >= 3 * std::max<quint32>(medianMs(), 1);
}

int BlockMapData::count(State s) const
{
    int n = 0;
    for (int c = 0; c < m_cells.size(); ++c)
        n += state(c) == s;
    return n;
}

int BlockMapData::slowAreas() const
{
    int areas = 0;
    bool inside = false;
    for (int c = 0; c < m_cells.size(); ++c) {
        const bool slow = state(c) == State::Slow;
        areas += slow && !inside;
        inside = slow;
    }
    return areas;
}
