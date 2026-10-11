// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Copies what can still be read off a failing drive, the way GNU ddrescue does: the
// easy parts first, then it comes back for the hard ones. Progress is kept in a map
// file in ddrescue's format, so a rescue can be stopped and resumed (or finished with
// ddrescue itself).

#include <QElapsedTimer>
#include <QObject>
#include <QVector>

#include <atomic>

class RescueMap
{
public:
    enum Status : char {
        NonTried = '?',
        NonTrimmed = '*', // a read failed somewhere in here
        NonScraped = '/', // edges trimmed, middle still to go sector by sector
        Bad = '-',
        Finished = '+',
    };
    struct Block {
        quint64 pos = 0;
        quint64 size = 0;
        char status = NonTried;
        quint64 end() const { return pos + size; }
    };

    explicit RescueMap(quint64 size = 0);

    bool load(const QString &path, quint64 expectedSize, QString *error);
    bool save(const QString &path, QString *error) const;

    void set(quint64 pos, quint64 size, char status);
    char statusAt(quint64 pos) const;
    // The first block with `status` that ends after `from` (clipped to start at `from`).
    bool next(char status, quint64 from, quint64 *start, quint64 *end) const;
    // The last block with `status` that starts before `before` (clipped to end there).
    bool previous(char status, quint64 before, quint64 *start, quint64 *end) const;

    const QVector<Block> &blocks() const { return m_blocks; }
    quint64 size() const { return m_size; }
    quint64 total(char status) const;
    bool done() const { return total(NonTried) + total(NonTrimmed) + total(NonScraped) == 0; }

    quint64 currentPos = 0;
    char currentStatus = '?';
    int currentPass = 1;

private:
    int indexAt(quint64 pos) const;

    quint64 m_size = 0;
    QVector<Block> m_blocks; // sorted, no gaps, neighbours never share a status
};

Q_DECLARE_METATYPE(QVector<RescueMap::Block>)

class RescueCopy : public QObject
{
    Q_OBJECT
public:
    // Takes ownership of both fds. The source should be O_DIRECT (UDisks' benchmark
    // open), the target an image file or a whole drive at least as big.
    RescueCopy(int sourceFd, quint64 size, int targetFd, const QString &mapPath);
    ~RescueCopy() override;
    void cancel() { m_cancel = true; }
    // Going easy on a failing drive. All of these can change while it runs.
    void setSpeedLimit(quint64 bytesPerSecond) { m_maxRate = bytesPerSecond; } // 0 = as fast as it goes
    void setErrorRest(int errorsInARow, int seconds) // 0 = never rest
    {
        m_restSeconds = seconds;
        m_restAfter = errorsInARow;
    }
    void setPaused(bool paused) { m_paused = paused; } // while the drive is too hot

public slots:
    void run();

signals:
    void progress(quint64 rescued, quint64 bad, quint64 total, const QString &phase);
    void mapChanged(const QVector<RescueMap::Block> &blocks);
    void finished(bool completed, const QString &message);

private:
    bool readSource(char *buf, quint64 len, quint64 pos);
    void waitWhilePaused();
    void coolDown(bool readOk, quint64 len); // after each read: rests and the speed limit
    bool writeTarget(const char *buf, quint64 len, quint64 pos);
    void tick(bool force = false);

    int m_source;
    int m_target;
    quint64 m_size;
    QString m_mapPath;
    RescueMap m_map;
    int m_sector = 512;
    QString m_phase;
    QString m_writeError;
    std::atomic<bool> m_cancel{false};
    std::atomic<quint64> m_maxRate{0};
    std::atomic<int> m_restAfter{0};
    std::atomic<int> m_restSeconds{60};
    std::atomic<bool> m_paused{false};
    int m_errorsInARow = 0;
    QElapsedTimer m_rateClock;
    quint64 m_rateBytes = 0;
    quint64 m_rateSeen = 0;
    QElapsedTimer m_clock;
    qint64 m_lastSave = 0, m_lastSignal = 0;
};
