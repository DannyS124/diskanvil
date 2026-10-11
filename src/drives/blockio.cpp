// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "blockio.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QStringList>

#include <algorithm>
#include <cerrno>
#ifndef Q_OS_WIN
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <io.h>
#endif

namespace blockio {

bool readAt(int fd, char *buf, quint64 len, quint64 offset)
{
    while (len > 0) {
        const qint64 n = readSome(fd, buf, len, offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        buf += n;
        len -= quint64(n);
        offset += quint64(n);
    }
    return true;
}

bool writeAt(int fd, const char *buf, quint64 len, quint64 offset)
{
    while (len > 0) {
        const qint64 n = writeSome(fd, buf, len, offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        buf += n;
        len -= quint64(n);
        offset += quint64(n);
    }
    return true;
}

// Closing, and what waits for it: by fd, while it's open.
namespace {
QMutex closeLock;
QHash<int, std::function<void()>> onClose;
} // namespace

void whenClosed(int fd, std::function<void()> then)
{
    QMutexLocker locker(&closeLock);
    onClose.insert(fd, std::move(then));
}

void close(int fd)
{
    if (fd < 0)
        return;
    std::function<void()> then;
    {
        QMutexLocker locker(&closeLock);
        then = onClose.take(fd);
    }
    ::close(fd);
    if (then)
        then();
}

#ifndef Q_OS_WIN // Windows' go through blockio_win.cpp's whole-sector rules
bool writeAll(int fd, const char *data, qint64 size)
{
    while (size > 0) {
        const auto n = ::write(fd, data, unsigned(std::min<qint64>(size, 1 << 30)));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        data += n;
        size -= n;
    }
    return true;
}

qint64 readFull(int fd, char *data, qint64 size)
{
    qint64 total = 0;
    while (total < size) {
        const auto n = ::read(fd, data + total, unsigned(std::min<qint64>(size - total, 1 << 30)));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        total += n;
    }
    return total;
}
#endif

#ifndef Q_OS_WIN

void alignedFree(void *p)
{
    free(p);
}

Buffer alignedBuffer(size_t size)
{
    void *raw = nullptr;
    if (posix_memalign(&raw, kAlign, size) != 0)
        return Buffer(nullptr, &alignedFree);
    return Buffer(static_cast<char *>(raw), &alignedFree);
}

qint64 readSome(int fd, char *buf, quint64 len, quint64 offset)
{
    return ::pread(fd, buf, len, off_t(offset));
}

qint64 writeSome(int fd, const char *buf, quint64 len, quint64 offset)
{
    return ::pwrite(fd, buf, len, off_t(offset));
}

bool seekTo(int fd, quint64 offset)
{
    return ::lseek(fd, off_t(offset), SEEK_SET) == off_t(offset);
}

int logicalSize(int fd)
{
    int size = 0;
    return ::ioctl(fd, BLKSSZGET, &size) == 0 && size > 0 ? size : 512;
}

int physicalSize(int fd)
{
    unsigned int size = 0;
    return ::ioctl(fd, BLKPBSZGET, &size) == 0 && size > 0 ? int(size) : logicalSize(fd);
}

quint64 deviceSize(int fd)
{
    quint64 size = 0;
    if (::ioctl(fd, BLKGETSIZE64, &size) == 0)
        return size;
    struct stat st {};
    return ::fstat(fd, &st) == 0 ? quint64(st.st_size) : 0;
}

bool isRegularFile(int fd)
{
    struct stat st {};
    return ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
}

bool setFileSize(int fd, quint64 size)
{
    return ::ftruncate(fd, off_t(size)) == 0;
}

bool setFileTime(int fd, qint64 msecs)
{
    const timespec time {msecs / 1000, long(msecs % 1000) * 1000000};
    const timespec times[2] = {time, time};
    return ::futimens(fd, times) == 0;
}

int openFile(const QString &path, FileMode mode)
{
    int flags = O_CLOEXEC;
    switch (mode) {
    case FileMode::Read: flags |= O_RDONLY; break;
    case FileMode::Replace: flags |= O_WRONLY | O_CREAT | O_TRUNC; break;
    case FileMode::Update: flags |= O_RDWR | O_CREAT; break;
    case FileMode::CreateNew: flags |= O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW; break;
    case FileMode::ReadBack: flags |= O_RDONLY; break;
    }
    const int fd = ::open(QFile::encodeName(path).constData(), flags, 0644);
    if (fd >= 0 && mode == FileMode::ReadBack)
        dropCache(fd); // flushed already, so the cache lets go of it
    return fd;
}

bool flush(int fd)
{
    return ::fdatasync(fd) == 0 || errno == EINVAL;
}

bool flushFile(QFile &file)
{
    return file.flush() && ::fsync(file.handle()) == 0;
}

void dropCache(int fd)
{
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
}

void syncFileSystem(const QString &path)
{
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ::syncfs(fd);
        ::close(fd);
    }
}

namespace {

// The /dev node holding the file system that `path` is on, from /proc/self/mountinfo
// (st_dev doesn't help for Btrfs subvolumes, which get anonymous device numbers).
QString mountSource(const QString &path)
{
    const QString target = QFileInfo(path).canonicalFilePath().isEmpty() ? QFileInfo(QFileInfo(path).absolutePath()).canonicalFilePath()
                                                                          : QFileInfo(path).canonicalFilePath();
    QFile f(QStringLiteral("/proc/self/mountinfo"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QString best, source;
    for (const QByteArray &line : f.readAll().split('\n')) {
        const QList<QByteArray> fields = line.split(' ');
        const int dash = fields.indexOf("-");
        if (fields.size() < 5 || dash < 0 || dash + 2 >= fields.size())
            continue;
        const QString mp = QString::fromLocal8Bit(fields[4]).replace(QStringLiteral("\\040"), QStringLiteral(" "));
        const bool inside = target == mp || target.startsWith(mp == QLatin1String("/") ? mp : mp + QLatin1Char('/'));
        if (inside && mp.size() >= best.size()) {
            best = mp;
            source = QString::fromLocal8Bit(fields[dash + 2]);
        }
    }
    return source;
}

// Every kernel block device name under `name`: itself, its parent disk, and for
// device-mapper/md devices, the devices they sit on.
void underlying(const QString &name, QStringList *out, int depth = 0)
{
    if (name.isEmpty() || out->contains(name) || depth > 8)
        return;
    *out << name;
    const QString sys = QStringLiteral("/sys/class/block/") + name;
    const QString parent = QFileInfo(QFileInfo(sys).canonicalFilePath()).dir().dirName();
    if (QFileInfo::exists(sys + QStringLiteral("/partition")))
        *out << parent;
    for (const QString &slave : QDir(sys + QStringLiteral("/slaves")).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        underlying(slave, out, depth + 1);
}

} // namespace

bool pathIsOnDisk(const QString &path, const QString &diskDevice)
{
    const QString source = mountSource(path);
    if (!source.startsWith(QLatin1String("/dev/")))
        return false; // tmpfs, network shares and the like
    QStringList names;
    underlying(QFileInfo(source).canonicalFilePath().section(QLatin1Char('/'), -1), &names);
    return names.contains(diskDevice.section(QLatin1Char('/'), -1));
}

#endif // !Q_OS_WIN

} // namespace blockio
