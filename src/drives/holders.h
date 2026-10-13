// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Who's using this drive? When a drive won't unmount ("target is busy"), the programs that have
// something open on it: a file, a folder they're in (a terminal sitting in it counts), a program
// or library they were started from, or the drive itself. Also what isn't a program: a disk image
// on it that's attached, or another drive mounted inside it. Linux only (read from /proc); a
// program's files can only be seen as its own user or root, so the ones that couldn't be looked
// at are counted.

#include <QString>
#include <QStringList>
#include <QVector>

namespace holders {

struct Holder {
    qint64 pid = 0;
    QString program;  // "bash", "vlc"
    QString command;  // its whole command line
    QString user;     // whose it is
    QStringList what; // what it has open there: "in the folder /media/me/STICK/Photos", "the file .../a.mp4"
    bool mine = false; // the same user as DiskAnvil: it can be closed without the admin password
};

struct Report {
    QVector<Holder> programs;
    QStringList others;  // not programs: "a disk image on it is attached (loop0)", "a drive mounted inside it at ..."
    int unreadable = 0;  // other users' programs that couldn't be looked at
};

// What has the file systems mounted at `mountPoints`, or the devices `devices`, open. `procRoot`
// and `mountInfo` are for the tests.
Report find(const QStringList &mountPoints, const QStringList &devices, const QString &procRoot = QStringLiteral("/proc"),
            const QString &mountInfo = QStringLiteral("/proc/self/mountinfo"), const QString &sysRoot = QStringLiteral("/sys"));

// As lines a root helper prints and DiskAnvil reads back (DiskAnvil Live's sudo):
// "pid\tprogram\tuser\twhat|what|...\tcommand".
QString toLines(const Report &report);
Report fromLines(const QString &lines);

// The option DiskAnvil runs itself with as root to look: `diskanvil --drive-holders <mount> [<device>...]`.
inline constexpr char kOption[] = "--drive-holders";

} // namespace holders
