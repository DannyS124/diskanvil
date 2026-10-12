// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wof.h"

#include <QDir>
#include <QFileInfo>
#include <QtEndian>

#ifndef Q_OS_WIN
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

namespace wof {

bool compressed(const QString &path)
{
#ifdef Q_OS_WIN
    Q_UNUSED(path)
    return false;
#else
    const QByteArray name = path.toLocal8Bit();
    // ntfs-3g: a link, with the reparse data (the tag first) in an attribute of its own.
    struct stat st;
    if (::lstat(name.constData(), &st) == 0 && S_ISLNK(st.st_mode)) {
        QByteArray data(16 * 1024 + 8, '\0'); // the most a reparse point holds
        const ssize_t n = ::lgetxattr(name.constData(), "system.ntfs_reparse_data", data.data(), size_t(data.size()));
        return n >= 4 && qFromLittleEndian<quint32>(data.constData()) == 0x80000017; // IO_REPARSE_TAG_WOF
    }
    // ntfs3 without its LZX/XPRESS support: reading isn't supported.
    const int fd = ::open(name.constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    char c;
    const ssize_t n = ::read(fd, &c, 1);
    const int error = errno;
    ::close(fd);
    return n < 0 && (error == EOPNOTSUPP || error == ENOTSUP);
#endif
}

QDateTime changed(const QString &path)
{
#ifdef Q_OS_WIN
    return QFileInfo(path).lastModified();
#else
    struct stat st;
    if (::lstat(path.toLocal8Bit().constData(), &st) != 0)
        return {};
    return QDateTime::fromSecsSinceEpoch(st.st_mtime);
#endif
}

bool inWindowsFolder(const QString &root, const QString &file)
{
    return QDir(root).relativeFilePath(file).startsWith(QLatin1String("windows/"), Qt::CaseInsensitive);
}

} // namespace wof
