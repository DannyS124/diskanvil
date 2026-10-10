// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Low-level helpers for reading and writing devices and files through fds: the ones UDisks2
// opened on Linux, and on Windows CRT fds wrapping the drive's handle. Everything that reads
// or writes by position, flushes or sizes an fd goes through here so it works on both.

#include <QString>

#include <cstdlib>
#include <functional>
#include <memory>

class QFile;

namespace blockio {

constexpr size_t kAlign = 4096; // O_DIRECT wants aligned buffers, offsets and sizes

void alignedFree(void *p);
using Buffer = std::unique_ptr<char, decltype(&alignedFree)>;
Buffer alignedBuffer(size_t size);

// Whole-length reads and writes; false on any error or short transfer.
bool readAt(int fd, char *buf, quint64 len, quint64 offset);
bool writeAt(int fd, const char *buf, quint64 len, quint64 offset);
// One read at `offset`: how much it got, 0 at the end, -1 on an error (errno says which).
qint64 readSome(int fd, char *buf, quint64 len, quint64 offset);
qint64 writeSome(int fd, const char *buf, quint64 len, quint64 offset);
// Sequential, at the fd's current position.
bool writeAll(int fd, const char *data, qint64 size);
qint64 readFull(int fd, char *data, qint64 size);
bool seekTo(int fd, quint64 offset);

int logicalSize(int fd);
int physicalSize(int fd);
quint64 deviceSize(int fd); // block device or regular file
bool isRegularFile(int fd);
bool setFileSize(int fd, quint64 size);
// Last changed (and last opened) time, milliseconds since 1970.
bool setFileTime(int fd, qint64 msecs);

// Files opened the same way on every system (close-on-exec on Linux, binary on Windows).
enum class FileMode {
    Read,
    Replace,   // write; created, or emptied if it's there
    Update,    // read and write; created if it isn't there, kept if it is
    CreateNew, // write; fails if anything (a symlink too) has that name already
    ReadBack,  // read what's on the drive itself, not what the cache kept: to check what was written
               // (and flushed). On Windows, reads have to go through readSome()/readFull() then.
};
int openFile(const QString &path, FileMode mode);

// Closes an fd from openFile() or UDisks::openBlock, then runs what was waiting for it to be
// closed: on Windows, a drive taken offline to be written to is brought back online.
void close(int fd);
// `then` runs once `fd` is closed with close() above (on the thread that closes it).
void whenClosed(int fd, std::function<void()> then);

// What was written is on the drive, not just in the cache. True for things that can't be
// flushed (pipes).
bool flush(int fd);
// The same for a file opened with QFile.
bool flushFile(QFile &file);
// Done with this data, the cache needn't keep it.
void dropCache(int fd);
// Everything written to the file system that holds `path` is on the drive.
void syncFileSystem(const QString &path);

// Whether `path` lives on `diskDevice` (/dev/sdb, or \\.\PhysicalDrive1 on Windows),
// following partitions, LUKS/LVM mappings and Btrfs subvolumes. Used to refuse writing a
// copy onto its own source.
bool pathIsOnDisk(const QString &path, const QString &diskDevice);

} // namespace blockio
