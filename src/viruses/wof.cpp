// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wof.h"

#ifndef Q_OS_WIN
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace wof {

bool compressed(const QString &path)
{
#ifdef Q_OS_WIN
    Q_UNUSED(path)
    return false;
#else
    const int fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    char c;
    const ssize_t n = ::read(fd, &c, 1);
    const int error = errno;
    ::close(fd);
    return n < 0 && (error == EOPNOTSUPP || error == ENOTSUP);
#endif
}

} // namespace wof
