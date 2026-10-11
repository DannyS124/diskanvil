// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cleanup.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <cerrno>
#include <cstring>
#ifdef Q_OS_WIN
#include "win/winutil.h"
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cleanup {

#ifndef Q_OS_WIN

namespace {

QByteArrayList entries(int dirFd)
{
    QByteArrayList names;
    DIR *dir = ::fdopendir(::fcntl(dirFd, F_DUPFD_CLOEXEC, 0));
    if (!dir)
        return names;
    while (dirent *e = ::readdir(dir)) {
        if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0)
            names << QByteArray(e->d_name);
    }
    ::closedir(dir);
    return names;
}

quint64 sizeAt(int dirFd, dev_t device, int depth)
{
    quint64 total = 0;
    for (const QByteArray &name : entries(dirFd)) {
        struct stat st {};
        if (::fstatat(dirFd, name.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0 || st.st_dev != device)
            continue;
        total += quint64(st.st_blocks) * 512;
        if (S_ISDIR(st.st_mode) && depth < 400) {
            const int fd = ::openat(dirFd, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd >= 0) {
                total += sizeAt(fd, device, depth + 1);
                ::close(fd);
            }
        }
    }
    return total;
}

void fail(Result &r, const QByteArray &name)
{
    if (r.firstError.isEmpty())
        r.firstError = QStringLiteral("%1: %2").arg(QFile::decodeName(name), QString::fromLocal8Bit(std::strerror(errno)));
    ++r.failed;
}

void removeAt(int dirFd, dev_t device, Result &r, const std::atomic<bool> *cancel, int depth)
{
    for (const QByteArray &name : entries(dirFd)) {
        if (cancel && *cancel)
            return;
        struct stat st {};
        if (::fstatat(dirFd, name.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (st.st_dev != device || depth >= 400)
                continue; // something mounted here, or absurdly deep: leave it
            const int fd = ::openat(dirFd, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) {
                fail(r, name);
                continue;
            }
            removeAt(fd, device, r, cancel, depth + 1);
            ::close(fd);
            if (::unlinkat(dirFd, name.constData(), AT_REMOVEDIR) == 0)
                r.freed += quint64(st.st_blocks) * 512;
            else if (errno != ENOTEMPTY) // not empty: something inside was kept on purpose
                fail(r, name);
        } else if (::unlinkat(dirFd, name.constData(), 0) == 0) {
            // A file with other hard links keeps its space until the last one goes.
            if (st.st_nlink <= 1)
                r.freed += quint64(st.st_blocks) * 512;
        } else {
            fail(r, name);
        }
    }
}

} // namespace

quint64 folderSize(const QString &folder)
{
    const int fd = ::open(QFile::encodeName(folder).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    struct stat st {};
    const quint64 total = ::fstat(fd, &st) == 0 ? sizeAt(fd, st.st_dev, 0) : 0;
    ::close(fd);
    return total;
}

Result removeContents(const QString &folder, const std::atomic<bool> *cancel)
{
    Result r;
    // O_NOFOLLOW: if the folder itself was swapped for a symlink, don't follow it.
    const int fd = ::open(QFile::encodeName(folder).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT)
            fail(r, QFile::encodeName(folder));
        return r;
    }
    struct stat st {};
    if (::fstat(fd, &st) == 0)
        removeAt(fd, st.st_dev, r, cancel, 0);
    ::close(fd);
    return r;
}

QStringList trashFolders(const QStringList &mountPoints)
{
    QStringList folders;
    const QString home = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Trash");
    if (QFileInfo::exists(home))
        folders << home;
    const QString own = QStringLiteral("/.Trash-%1").arg(::getuid());
    for (const QString &mp : mountPoints) {
        const QString t = (mp == QLatin1String("/") ? QString() : mp) + own;
        if (QFileInfo(t).isDir() && !QFileInfo(t).isSymLink() && !folders.contains(t))
            folders << t;
    }
    return folders;
}

#else

// On Windows: sizes as the files say (no clusters counted, Disk Cleanup only needs an idea),
// and links (junctions, symlinks) are removed themselves, never what they point to.

namespace {

template <typename Visit>
void forEntries(const QString &folder, const Visit &visit)
{
    WIN32_FIND_DATAW data {};
    const std::wstring pattern = win::wide(QDir::toNativeSeparators(folder) + QStringLiteral("\\*"));
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        const QString name = win::fromWide(data.cFileName);
        if (name != QLatin1String(".") && name != QLatin1String("..") && !visit(name, data))
            break;
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

quint64 sizeOf(const WIN32_FIND_DATAW &data)
{
    return quint64(data.nFileSizeHigh) << 32 | data.nFileSizeLow;
}

quint64 sizeAt(const QString &folder, int depth)
{
    quint64 total = 0;
    forEntries(folder, [&](const QString &name, const WIN32_FIND_DATAW &data) {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            return true;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 400)
                total += sizeAt(folder + QLatin1Char('/') + name, depth + 1);
        } else {
            total += sizeOf(data);
        }
        return true;
    });
    return total;
}

void fail(Result &r, const QString &name)
{
    if (r.firstError.isEmpty())
        r.firstError = QStringLiteral("%1: %2").arg(name, win::lastErrorText());
    ++r.failed;
}

void removeAt(const QString &folder, Result &r, const std::atomic<bool> *cancel, int depth)
{
    forEntries(folder, [&](const QString &name, const WIN32_FIND_DATAW &data) {
        if (cancel && *cancel)
            return false;
        const QString path = folder + QLatin1Char('/') + name;
        const std::wstring native = win::wide(QDir::toNativeSeparators(path));
        if (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
            SetFileAttributesW(native.c_str(), data.dwFileAttributes & ~DWORD(FILE_ATTRIBUTE_READONLY));
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            if (depth >= 400)
                return true;
            removeAt(path, r, cancel, depth + 1);
            if (!RemoveDirectoryW(native.c_str()) && GetLastError() != ERROR_DIR_NOT_EMPTY)
                fail(r, name);
        } else if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? RemoveDirectoryW(native.c_str()) : DeleteFileW(native.c_str())) {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                r.freed += sizeOf(data);
        } else {
            // Files a running program holds open stay; that's normal for temp folders.
            if (GetLastError() != ERROR_SHARING_VIOLATION)
                fail(r, name);
            else
                ++r.failed;
        }
        return true;
    });
}

} // namespace

quint64 folderSize(const QString &folder)
{
    return sizeAt(QDir::fromNativeSeparators(folder), 0);
}

Result removeContents(const QString &folder, const std::atomic<bool> *cancel)
{
    Result r;
    const QFileInfo info(folder);
    if (!info.exists() || info.isSymLink() || info.isJunction())
        return r;
    removeAt(QDir::fromNativeSeparators(folder), r, cancel, 0);
    return r;
}

QStringList trashFolders(const QStringList &)
{
    return {}; // the Recycle Bin is emptied through Windows itself
}

#endif

Result emptyTrash(const QString &trashFolder)
{
    Result total;
    for (const char *part : {"files", "info", "expunged"}) {
        const Result r = removeContents(trashFolder + QLatin1Char('/') + QLatin1String(part));
        total.freed += r.freed;
        total.failed += r.failed;
        if (total.firstError.isEmpty())
            total.firstError = r.firstError;
    }
    QFile::remove(trashFolder + QStringLiteral("/directorysizes")); // KDE's size cache for the old contents
    return total;
}

qint64 parsePaccacheSaved(const QString &output)
{
    static const QRegularExpression saved(QStringLiteral("disk space saved: ([0-9.]+) ([KMGT]?i?B)"));
    const QRegularExpressionMatch m = saved.match(output);
    if (!m.hasMatch())
        return 0;
    const QString unit = m.captured(2);
    double value = m.captured(1).toDouble();
    const QString prefixes = QStringLiteral("KMGT");
    const int power = unit.size() > 1 ? int(prefixes.indexOf(unit.at(0))) + 1 : 0;
    for (int i = 0; i < power; ++i)
        value *= 1024;
    return qint64(value);
}

qint64 packageCacheReclaimable(bool uninstalled)
{
    const QString paccache = QStandardPaths::findExecutable(QStringLiteral("paccache"));
    if (paccache.isEmpty())
        return -1;
    QStringList args = {QStringLiteral("-d")};
    if (uninstalled)
        args << QStringLiteral("-uk0");
    // paccache.service reads its options from here, e.g. PACCACHE_ARGS='-k1'.
    QFile conf(QStringLiteral("/etc/conf.d/pacman-contrib"));
    if (!uninstalled && conf.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : conf.readAll().split('\n')) {
            if (line.trimmed().startsWith("PACCACHE_ARGS=")) {
                QString value = QString::fromLocal8Bit(line.trimmed().mid(14));
                value.remove(QLatin1Char('\'')).remove(QLatin1Char('"'));
                for (const QString &a : value.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                    if (a != QLatin1String("-r"))
                        args << a;
                }
            }
        }
    }
    QProcess p;
    p.start(paccache, args);
    if (!p.waitForFinished(60000))
        return 0;
    return parsePaccacheSaved(QString::fromLocal8Bit(p.readAllStandardOutput() + p.readAllStandardError()));
}

} // namespace cleanup
