// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What the block map shows: the drive split into a few thousand cells, each remembering
// whether it was read, how slow its slowest read was, and whether anything in it failed.

#include <QMetaType>
#include <QVector>

#include <array>

struct ReadSample {
    quint64 offset = 0;
    quint32 length = 0;
    quint32 ms = 0;
    bool readable = true;
};
Q_DECLARE_METATYPE(ReadSample)
Q_DECLARE_METATYPE(QVector<ReadSample>)

class BlockMapData
{
public:
    // Pending: tried, but parts are still to be retried (Rescue Copy).
    enum class State : quint8 { Unread, Good, Slow, Pending, Bad };
    static constexpr int kMaxCells = 4096;

    BlockMapData() = default;
    // rotational: hard drives get a higher "slow" bar than SSDs and flash.
    BlockMapData(quint64 size, bool rotational);

    void add(const ReadSample &sample);
    void add(const QVector<ReadSample> &samples);
    // Rescue Copy paints its whole map; a cell takes its worst state.
    void clearStates();
    void mark(quint64 offset, quint64 length, State state);
    // Read back fine after it was written (Check a USB Stick): no longer Pending there.
    void markChecked(quint64 offset, quint64 length);

    quint64 size() const { return m_size; }
    int cells() const { return int(m_cells.size()); }
    State state(int cell) const;
    quint32 slowestMs(int cell) const { return m_cells[cell].worstMs; }
    quint64 cellStart(int cell) const;
    quint64 cellEnd(int cell) const;
    int cellAt(quint64 offset) const;

    int count(State state) const;
    // Runs of neighbouring slow cells, counted once each: "3 slow areas".
    int slowAreas() const;
    quint32 medianMs() const;
    bool isSlow(quint32 ms) const;

private:
    struct Cell {
        quint32 worstMs = 0;
        bool read = false;
        State marked = State::Unread;
    };
    template <typename F> void forCells(quint64 offset, quint64 length, F f);

    quint64 m_size = 0;
    quint32 m_slowFloorMs = 150;
    QVector<Cell> m_cells;
    std::array<quint32, 1001> m_histogram{}; // read times in ms; the last bin holds everything slower
    quint64 m_samples = 0;
    mutable qint64 m_median = -1; // -1 = needs working out again
};
