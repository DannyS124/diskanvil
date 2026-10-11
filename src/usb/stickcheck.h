// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QMetaType>
#include <QObject>
#include <QVector>

#include <atomic>
#include <memory>

// Check a USB Stick: writes to a stick or memory card, reads it back, and finds bad spots and
// "fake" sticks, the ones that say they're bigger than they are. A fake 64 GB stick that really
// holds 8 GB takes everything you give it and quietly writes past 8 GB over what's already
// there, so files go bad without any error. Rufus checks for this too; this one also works out
// how much the stick really holds, so it can be made safe to use.
//
// Every block written carries a stamp: a marker, this check's own random ID, and the block's
// offset, followed by data worked out from that offset. A block that reads back with another
// block's stamp means the stick wrapped around; a block that reads back as anything else means
// the data didn't stick.
namespace stickcheck {

// Where the check writes. The real one is the stick opened with O_DIRECT; the tests use one
// in memory that can wrap around and have bad spots.
class Target
{
public:
    virtual ~Target() = default;
    virtual quint64 size() const = 0;
    virtual bool write(quint64 offset, const char *data, qsizetype length) = 0;
    virtual bool read(quint64 offset, char *data, qsizetype length) = 0;
    virtual bool flush() = 0;
};
// The stick, opened with O_DIRECT. Takes the fd and closes it.
std::unique_ptr<Target> fromFd(int fd);

enum class Mode {
    Quick,     // a stamp every few MiB: minutes, catches most fake sticks
    Full,      // every byte, once: hours, catches everything
    FullTwice, // every byte, then again with every bit flipped
};

struct Result {
    bool completed = false; // false: stopped, or it couldn't start (error says why)
    QString error;
    quint64 claimed = 0;    // what the stick says it holds
    quint64 real = 0;       // what it really holds: the same unless it's fake
    bool fake = false;
    bool wraps = false;     // fake, and it writes past the end over the start
    int writeErrors = 0;
    int readErrors = 0;
    int wrongData = 0;      // read back fine, but not what was written
    QVector<quint64> bad;   // where the problems are (the first 256)
    bool good() const { return completed && !fake && writeErrors == 0 && readErrors == 0 && wrongData == 0; }
};

// For the block map: a stretch written (not checked yet), checked fine, or bad.
struct Mark {
    enum State : quint8 { Written, Good, Bad };
    quint64 offset = 0;
    quint64 length = 0;
    State state = Written;
};

class Checker : public QObject
{
    Q_OBJECT
public:
    Checker(std::unique_ptr<Target> target, Mode mode);
    ~Checker() override;
    void cancel() { m_cancel = true; }

public slots:
    void run();

signals:
    void progress(const QString &phase, quint64 done, quint64 total);
    void marks(const QVector<stickcheck::Mark> &marks); // batched, a few times a second
    void finished(const stickcheck::Result &result);

private:
    void quick(Result &r);
    void full(Result &r, int pass, int passes);
    void verdict(Result &r, const QVector<quint64> &checked, const QVector<int> &states, const QVector<quint64> &aliases) const;
    void mark(quint64 offset, quint64 length, Mark::State state);
    void flushMarks(bool force);

    std::unique_ptr<Target> m_target;
    Mode m_mode;
    quint64 m_run = 0;
    QVector<Mark> m_pending;
    qint64 m_lastMarks = 0;
    std::atomic<bool> m_cancel{false};
};

// Where a quick check writes its stamps on a stick of `size` bytes.
QVector<quint64> quickSpots(quint64 size);
// How long a check of a stick this big takes, roughly, in seconds (for the dialog).
double estimate(Mode mode, quint64 size);

} // namespace stickcheck

Q_DECLARE_METATYPE(stickcheck::Mark)
Q_DECLARE_METATYPE(QVector<stickcheck::Mark>)
Q_DECLARE_METATYPE(stickcheck::Result)
