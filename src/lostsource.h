// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QMutex>
#include <QString>
#include <QVector>

#include <memory>

// Find Lost Files: where the bytes come from. A drive or partition (an fd UDisks opened
// read-only, usually with O_DIRECT), a disk image file, or memory for the tests. There's no
// way to write through it: looking for lost files never changes the drive they're on.
namespace lost {

struct Extent {
    quint64 start = 0;  // where it starts in the source, in bytes; kZeros: nothing stored, reads as zeros
    quint64 length = 0;
    quint64 end() const { return start + length; }
    static constexpr quint64 kZeros = ~quint64(0); // a sparse part of an NTFS file
    bool zeros() const { return start == kZeros; }
};

// Bytes worked out from other bytes as they're read: one of Windows' shadow copies of a volume,
// say. Reads have to be safe from several threads at once.
class Layer
{
public:
    virtual ~Layer() = default;
    virtual quint64 size() const = 0;
    // Exactly `len` bytes at `offset` (inside size()); false when they can't be had.
    virtual bool read(quint64 offset, char *buf, quint64 len) = 0;
};

class Source
{
public:
    // Takes the fd over and closes it when the last user lets go.
    static std::shared_ptr<Source> fromFd(int fd, bool direct);
    static std::shared_ptr<Source> fromImage(const QString &path, QString *error);
    static std::shared_ptr<Source> fromMemory(QByteArray data);
    static std::shared_ptr<Source> fromLayer(std::unique_ptr<Layer> layer);
    ~Source();
    Source(const Source &) = delete;
    Source &operator=(const Source &) = delete;

    quint64 size() const { return m_size; }

    // Reads `len` bytes at `offset` (cut short at the end of the source) into `buf`. What can't
    // be read comes back as zeros and is remembered as unreadable; the drive isn't asked again
    // and again for the same bad spot. Returns how many bytes it read, zeros included. Safe to
    // call from several threads at once.
    quint64 read(quint64 offset, char *buf, quint64 len);
    // Whether anything in that range turned out unreadable so far.
    bool unreadable(quint64 offset, quint64 len) const;
    QVector<Extent> unreadableParts() const;

    // For the tests: reads that touch these ranges fail, like bad spots on a drive.
    void failReads(const QVector<Extent> &ranges) { m_failForTest = ranges; }

private:
    Source() = default;
    bool rawRead(quint64 offset, char *buf, quint64 len);
    void markBad(quint64 offset, quint64 len);

    int m_fd = -1;
    std::unique_ptr<Layer> m_layer;
    bool m_direct = false;
    bool m_memory = false;
    QByteArray m_data;
    quint64 m_size = 0;
    mutable QMutex m_lock;
    QVector<Extent> m_bad; // sorted by start, never overlapping
    QVector<Extent> m_failForTest;
};

} // namespace lost
