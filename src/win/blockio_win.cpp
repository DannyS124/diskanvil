// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// blockio on Windows. The fds are the C library's, wrapping a HANDLE (_open_osfhandle), so
// the rest of DiskAnvil passes them around and closes them as on Linux; reads and writes at
// a position go to the HANDLE directly.

#include "blockio.h"

#include "winutil.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <io.h>
#include <malloc.h>
#include <sys/stat.h>
#include <windows.h>
#include <winioctl.h>

namespace blockio {

namespace {

HANDLE handleOf(int fd)
{
    return reinterpret_cast<HANDLE>(_get_osfhandle(fd));
}

int errnoFor(DWORD error)
{
    switch (error) {
    case ERROR_ACCESS_DENIED:
    case ERROR_WRITE_PROTECT:
        return EACCES;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return ENOSPC;
    case ERROR_INVALID_PARAMETER:
        return EINVAL;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return ENOENT;
    case ERROR_NOT_READY:
    case ERROR_DEVICE_NOT_CONNECTED:
        return ENODEV;
    default:
        return EIO;
    }
}

bool transfer(HANDLE h, bool write, void *buf, DWORD len, quint64 offset, DWORD *done)
{
    OVERLAPPED at {};
    at.Offset = DWORD(offset);
    at.OffsetHigh = DWORD(offset >> 32);
    return write ? WriteFile(h, buf, len, done, &at) : ReadFile(h, buf, len, done, &at);
}

// Drives and partitions only take whole sectors at sector boundaries, from memory on a sector
// boundary. For a piece that isn't, the sectors around it go through a buffer of its own (4 KiB
// covers both sector sizes); a write of whole sectors from memory that's only off isn't read first.
qint64 unaligned(HANDLE h, bool write, char *buf, quint64 len, quint64 offset)
{
    const quint64 start = offset / kAlign * kAlign;
    const quint64 end = (offset + len + kAlign - 1) / kAlign * kAlign;
    Buffer bounce = alignedBuffer(size_t(end - start));
    if (!bounce)
        return errno = ENOMEM, -1;
    DWORD done = 0;
    if (write && start == offset && end == offset + len) {
        std::memcpy(bounce.get(), buf, size_t(len));
        if (!transfer(h, true, bounce.get(), DWORD(len), offset, &done))
            return errno = errnoFor(GetLastError()), -1;
        return qint64(done);
    }
    if (!transfer(h, false, bounce.get(), DWORD(end - start), start, &done))
        return errno = errnoFor(GetLastError()), -1;
    if (done < offset - start)
        return 0;
    const quint64 got = std::min<quint64>(len, done - (offset - start));
    if (!write) {
        std::memcpy(buf, bounce.get() + (offset - start), size_t(got));
        return qint64(got);
    }
    std::memcpy(bounce.get() + (offset - start), buf, size_t(len));
    if (!transfer(h, true, bounce.get(), DWORD(end - start), start, &done))
        return errno = errnoFor(GetLastError()), -1;
    return qint64(len);
}

qint64 positioned(int fd, bool write, char *buf, quint64 len, quint64 offset)
{
    const HANDLE h = handleOf(fd);
    if (h == INVALID_HANDLE_VALUE)
        return errno = EBADF, -1;
    const DWORD chunk = DWORD(std::min<quint64>(len, 1u << 30));
    DWORD done = 0;
    if (transfer(h, write, buf, chunk, offset, &done))
        return qint64(done);
    const DWORD error = GetLastError();
    if (error == ERROR_HANDLE_EOF)
        return 0;
    if (error == ERROR_INVALID_PARAMETER && (offset % kAlign || chunk % kAlign || quintptr(buf) % kAlign) && chunk <= 64 * 1024 * 1024)
        return unaligned(h, write, buf, chunk, offset);
    errno = errnoFor(error);
    return -1;
}

bool lengthOf(HANDLE h, quint64 *size)
{
    GET_LENGTH_INFORMATION info {};
    if (win::ioctl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &info, sizeof info)) {
        *size = quint64(info.Length.QuadPart);
        return true;
    }
    return false;
}

} // namespace

void alignedFree(void *p)
{
    _aligned_free(p);
}

Buffer alignedBuffer(size_t size)
{
    return Buffer(static_cast<char *>(_aligned_malloc(std::max<size_t>(size, 1), kAlign)), &alignedFree);
}

// The sequential calls, at the handle's own position: through the same whole-sector rules as
// the rest, which the C library's read() and write() don't know about.
quint64 positionOf(HANDLE h)
{
    LARGE_INTEGER zero {}, at {};
    SetFilePointerEx(h, zero, &at, FILE_CURRENT);
    return quint64(at.QuadPart);
}

void moveTo(HANDLE h, quint64 offset)
{
    LARGE_INTEGER to;
    to.QuadPart = LONGLONG(offset);
    SetFilePointerEx(h, to, nullptr, FILE_BEGIN);
}

bool writeAll(int fd, const char *data, qint64 size)
{
    const HANDLE h = handleOf(fd);
    quint64 at = positionOf(h);
    while (size > 0) {
        const qint64 n = positioned(fd, true, const_cast<char *>(data), quint64(size), at);
        if (n <= 0)
            return false;
        data += n;
        size -= n;
        at += quint64(n);
    }
    moveTo(h, at);
    return true;
}

qint64 readFull(int fd, char *data, qint64 size)
{
    const HANDLE h = handleOf(fd);
    quint64 at = positionOf(h);
    qint64 total = 0;
    while (total < size) {
        const qint64 n = positioned(fd, false, data + total, quint64(size - total), at);
        if (n <= 0)
            break;
        total += n;
        at += quint64(n);
    }
    moveTo(h, at);
    return total;
}

qint64 readSome(int fd, char *buf, quint64 len, quint64 offset)
{
    return positioned(fd, false, buf, len, offset);
}

qint64 writeSome(int fd, const char *buf, quint64 len, quint64 offset)
{
    return positioned(fd, true, const_cast<char *>(buf), len, offset);
}

bool seekTo(int fd, quint64 offset)
{
    return _lseeki64(fd, qint64(offset), SEEK_SET) == qint64(offset);
}

int logicalSize(int fd)
{
    DISK_GEOMETRY_EX geometry {};
    if (win::ioctl(handleOf(fd), IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geometry, sizeof geometry)
        && geometry.Geometry.BytesPerSector > 0)
        return int(geometry.Geometry.BytesPerSector);
    return 512;
}

int physicalSize(int fd)
{
    STORAGE_PROPERTY_QUERY query {};
    query.PropertyId = StorageAccessAlignmentProperty;
    query.QueryType = PropertyStandardQuery;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR alignment {};
    if (win::ioctl(handleOf(fd), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query, &alignment, sizeof alignment)
        && alignment.BytesPerPhysicalSector > 0)
        return int(alignment.BytesPerPhysicalSector);
    return logicalSize(fd);
}

quint64 deviceSize(int fd)
{
    const HANDLE h = handleOf(fd);
    quint64 size = 0;
    if (lengthOf(h, &size))
        return size;
    LARGE_INTEGER file {};
    return GetFileSizeEx(h, &file) ? quint64(file.QuadPart) : 0;
}

bool isRegularFile(int fd)
{
    const HANDLE h = handleOf(fd);
    quint64 size = 0;
    return GetFileType(h) == FILE_TYPE_DISK && !lengthOf(h, &size);
}

bool setFileSize(int fd, quint64 size)
{
    FILE_END_OF_FILE_INFO end {};
    end.EndOfFile.QuadPart = LONGLONG(size);
    return SetFileInformationByHandle(handleOf(fd), FileEndOfFileInfo, &end, sizeof end);
}

bool setFileTime(int fd, qint64 msecs)
{
    // FILETIME counts 100 ns steps since 1601.
    const quint64 ticks = quint64(msecs) * 10000 + 116444736000000000ull;
    FILETIME time {DWORD(ticks), DWORD(ticks >> 32)};
    return SetFileTime(handleOf(fd), nullptr, &time, &time);
}

int openFile(const QString &path, FileMode mode)
{
    int flags = _O_BINARY | _O_NOINHERIT;
    switch (mode) {
    case FileMode::Read: flags |= _O_RDONLY; break;
    case FileMode::Replace: flags |= _O_WRONLY | _O_CREAT | _O_TRUNC; break;
    case FileMode::Update: flags |= _O_RDWR | _O_CREAT; break;
    case FileMode::CreateNew: flags |= _O_WRONLY | _O_CREAT | _O_EXCL; break;
    case FileMode::ReadBack: {
        // Windows can't drop a file's cached pages, but a handle of its own can read past them.
        win::Handle h(CreateFileW(win::wide(QDir::toNativeSeparators(path)).c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_NO_BUFFERING, nullptr));
        if (!h.valid())
            return errno = errnoFor(GetLastError()), -1;
        const int fd = _open_osfhandle(intptr_t(h.get()), _O_RDONLY | _O_BINARY);
        if (fd >= 0)
            h.release();
        return fd;
    }
    }
    return _wopen(win::wide(QDir::toNativeSeparators(path)).c_str(), flags, _S_IREAD | _S_IWRITE);
}

bool flush(int fd)
{
    const HANDLE h = handleOf(fd);
    if (GetFileType(h) == FILE_TYPE_PIPE)
        return true;
    if (FlushFileBuffers(h))
        return true;
    errno = errnoFor(GetLastError());
    return false;
}

bool flushFile(QFile &file)
{
    if (!file.flush())
        return false;
    if (file.handle() >= 0)
        return flush(file.handle());
    // Qt keeps a Windows handle of its own here; the cache is the file's, so a second handle
    // flushes the same data.
    win::Handle h(CreateFileW(win::wide(QDir::toNativeSeparators(file.fileName())).c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    return h.valid() && FlushFileBuffers(h.get());
}

void dropCache(int)
{
    // Windows has no way to drop one file's cached pages; drives and partitions aren't
    // cached in the first place.
}

void syncFileSystem(const QString &path)
{
    wchar_t mountPoint[MAX_PATH] = {};
    wchar_t volume[MAX_PATH] = {};
    if (!GetVolumePathNameW(win::wide(QDir::toNativeSeparators(path)).c_str(), mountPoint, MAX_PATH)
        || !GetVolumeNameForVolumeMountPointW(mountPoint, volume, MAX_PATH))
        return;
    QString name = win::fromWide(volume);
    if (name.endsWith(QLatin1Char('\\')))
        name.chop(1);
    // Flushing a whole volume takes an administrator, which DiskAnvil is on Windows.
    win::Handle h = win::openDevice(name, GENERIC_READ | GENERIC_WRITE);
    if (h.valid())
        FlushFileBuffers(h.get());
}

bool pathIsOnDisk(const QString &path, const QString &diskDevice)
{
    // diskDevice is a drive's or a volume's `device`: "Disk 1", "Disk 1 partition 2" or "E:".
    wchar_t mountPoint[MAX_PATH] = {};
    wchar_t volume[MAX_PATH] = {};
    if (!GetVolumePathNameW(win::wide(QDir::toNativeSeparators(path)).c_str(), mountPoint, MAX_PATH)
        || !GetVolumeNameForVolumeMountPointW(mountPoint, volume, MAX_PATH))
        return false; // a network share, or nothing there yet
    auto extentsOf = [](const QString &volumePath) {
        QString name = volumePath;
        if (name.endsWith(QLatin1Char('\\')))
            name.chop(1);
        const win::Handle h = win::openDevice(name, 0);
        return h.valid() ? win::ioctl(h.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS) : QByteArray();
    };
    const QByteArray mine = extentsOf(win::fromWide(volume));
    if (mine.size() < qsizetype(sizeof(VOLUME_DISK_EXTENTS)))
        return false;
    const auto *extents = reinterpret_cast<const VOLUME_DISK_EXTENTS *>(mine.constData());

    // A drive letter counts only for that volume itself, as on Linux, where a partition is
    // compared with the partitions the path is on.
    if (QRegularExpression(QStringLiteral("^[A-Za-z]:$")).match(diskDevice).hasMatch()) {
        wchar_t other[MAX_PATH] = {};
        const std::wstring root = win::wide(diskDevice + QLatin1Char('\\'));
        return GetVolumeNameForVolumeMountPointW(root.c_str(), other, MAX_PATH) && win::fromWide(other) == win::fromWide(volume);
    }
    const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("^Disk (\\d+)(?: partition (\\d+))?$")).match(diskDevice);
    if (!m.hasMatch())
        return false;
    const DWORD disk = m.captured(1).toULong();
    quint64 partStart = 0, partEnd = ~0ull;
    if (!m.captured(2).isEmpty()) {
        const win::Handle h = win::openDevice(QStringLiteral("\\\\.\\PhysicalDrive%1").arg(disk), GENERIC_READ);
        const QByteArray layout = h.valid() ? win::ioctl(h.get(), IOCTL_DISK_GET_DRIVE_LAYOUT_EX) : QByteArray();
        if (layout.size() < qsizetype(sizeof(DRIVE_LAYOUT_INFORMATION_EX)))
            return false;
        const auto *info = reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX *>(layout.constData());
        bool found = false;
        for (DWORD i = 0; i < info->PartitionCount; ++i) {
            const PARTITION_INFORMATION_EX &p = info->PartitionEntry[i];
            if (p.PartitionNumber == m.captured(2).toULong()) {
                partStart = quint64(p.StartingOffset.QuadPart);
                partEnd = partStart + quint64(p.PartitionLength.QuadPart);
                found = true;
            }
        }
        if (!found)
            return false;
    }
    for (DWORD i = 0; i < extents->NumberOfDiskExtents; ++i) {
        const DISK_EXTENT &e = extents->Extents[i];
        const quint64 start = quint64(e.StartingOffset.QuadPart);
        if (e.DiskNumber == disk && start < partEnd && start + quint64(e.ExtentLength.QuadPart) > partStart)
            return true;
    }
    return false;
}

} // namespace blockio
