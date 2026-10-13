// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "holders.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#ifndef Q_OS_WIN
#include <pwd.h>
#include <unistd.h>
#endif

namespace holders {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("holders", text);
}

QString readLink(const QString &path)
{
    return QFileInfo(path).symLinkTarget().isEmpty() ? QFile::symLinkTarget(path) : QFileInfo(path).symLinkTarget();
}

// On one of the mounts (the mount point itself, or under it).
QString under(const QString &path, const QStringList &mounts)
{
    for (const QString &m : mounts) {
        if (path == m || path.startsWith(m.endsWith(QLatin1Char('/')) ? m : m + QLatin1Char('/')))
            return m;
    }
    return {};
}

QString userName(uint uid)
{
#ifndef Q_OS_WIN
    if (const passwd *pw = ::getpwuid(uid))
        return QString::fromLocal8Bit(pw->pw_name);
#endif
    return QString::number(uid);
}

uint ownerOf(const QString &procDir)
{
    QFile status(procDir + QStringLiteral("/status"));
    if (status.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : status.readAll().split('\n'))
            if (line.startsWith("Uid:"))
                return line.mid(4).simplified().split(' ').value(0).toUInt();
    }
    return uint(-1);
}

} // namespace

Report find(const QStringList &mountPoints, const QStringList &devices, const QString &procRoot, const QString &mountInfo, const QString &sysRoot)
{
    Report r;
    QStringList mounts;
    for (const QString &m : mountPoints)
        if (!m.isEmpty())
            mounts << QDir::cleanPath(m);
#ifndef Q_OS_WIN
    const uint me = ::geteuid();
#else
    const uint me = 0;
#endif
    const QStringList pids = QDir(procRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &pidText : pids) {
        bool numeric = false;
        const qint64 pid = pidText.toLongLong(&numeric);
        if (!numeric || pid == QCoreApplication::applicationPid())
            continue;
        const QString dir = procRoot + QLatin1Char('/') + pidText;
        // The kernel's own threads (no command line) hold no files.
        QFile cmdline(dir + QStringLiteral("/cmdline"));
        if (!cmdline.open(QIODevice::ReadOnly))
            continue;
        const QByteArray commandLine = cmdline.readAll();
        if (commandLine.isEmpty())
            continue;
        Holder h;
        h.pid = pid;
        const uint uid = ownerOf(dir);
        h.mine = uid == me;
        h.user = userName(uid);
        QSet<QString> seen;
        const auto add = [&](const QString &words) {
            if (!seen.contains(words) && h.what.size() < 12) {
                seen.insert(words);
                h.what << words;
            }
        };
        // Where it is, what it is, what's open.
        const QString cwd = readLink(dir + QStringLiteral("/cwd"));
        if (!under(cwd, mounts).isEmpty())
            add(tr("in the folder %1").arg(cwd));
        const QString root = readLink(dir + QStringLiteral("/root"));
        if (root != QLatin1String("/") && !under(root, mounts).isEmpty())
            add(tr("running inside %1 (chroot)").arg(root));
        const QString exe = readLink(dir + QStringLiteral("/exe"));
        if (!under(exe, mounts).isEmpty())
            add(tr("started from %1").arg(exe));
        QDir fds(dir + QStringLiteral("/fd"));
        const QStringList entries = fds.entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot);
        if (entries.isEmpty() && !QFileInfo(dir + QStringLiteral("/fd")).isReadable() && !h.mine) {
            if (QFileInfo::exists(dir + QStringLiteral("/fd")))
                ++r.unreadable;
            continue;
        }
        for (const QString &fd : entries) {
            const QString target = readLink(fds.filePath(fd));
            if (target.isEmpty())
                continue;
            if (!under(target, mounts).isEmpty())
                add(tr("the file %1").arg(target));
            else if (devices.contains(target))
                add(tr("the drive itself (%1)").arg(target));
        }
        // Libraries and files it has mapped in.
        QFile maps(dir + QStringLiteral("/maps"));
        if (maps.open(QIODevice::ReadOnly)) {
            for (const QByteArray &line : maps.readAll().split('\n')) {
                const int slash = int(line.indexOf('/'));
                if (slash < 0)
                    continue;
                const QString file = QString::fromLocal8Bit(line.mid(slash)).trimmed();
                if (!under(file, mounts).isEmpty())
                    add(tr("the file %1 (loaded)").arg(file));
            }
        }
        if (h.what.isEmpty())
            continue;
        QFile comm(dir + QStringLiteral("/comm"));
        if (comm.open(QIODevice::ReadOnly))
            h.program = QString::fromLocal8Bit(comm.readAll()).trimmed();
        h.command = QString::fromLocal8Bit(QByteArray(commandLine).replace('\0', ' ')).trimmed();
        r.programs << h;
    }
    // Not programs: disk images on it that are attached, and drives mounted inside it.
    for (const QString &loop : QDir(sysRoot + QStringLiteral("/block")).entryList({QStringLiteral("loop*")}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::System)) {
        QFile backing(sysRoot + QStringLiteral("/block/") + loop + QStringLiteral("/loop/backing_file"));
        if (backing.open(QIODevice::ReadOnly)) {
            const QString file = QString::fromLocal8Bit(backing.readAll()).trimmed();
            if (!under(file, mounts).isEmpty())
                r.others << tr("the disk image %1 is attached (%2): detach it first").arg(file, loop);
        }
    }
    QFile info(mountInfo);
    if (info.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : info.readAll().split('\n')) {
            const QList<QByteArray> f = line.split(' ');
            if (f.size() < 5)
                continue;
            QString point = QString::fromLocal8Bit(f[4]);
            point.replace(QLatin1String("\\040"), QLatin1String(" "));
            if (!mounts.contains(point) && !under(point, mounts).isEmpty())
                r.others << tr("another drive is mounted inside it, at %1: unmount that first").arg(point);
        }
    }
    return r;
}

QString toLines(const Report &r)
{
    QStringList out;
    for (const Holder &h : r.programs) {
        QStringList what = h.what;
        for (QString &w : what)
            w.replace(QLatin1Char('|'), QLatin1Char('/'));
        out << QStringLiteral("P\t%1\t%2\t%3\t%4\t%5").arg(h.pid).arg(QString(h.program).remove(QLatin1Char('\t')), h.user, what.join(QLatin1Char('|')),
                                                           QString(h.command).remove(QLatin1Char('\t')).remove(QLatin1Char('\n')));
    }
    for (const QString &o : r.others)
        out << QStringLiteral("O\t") + QString(o).remove(QLatin1Char('\n'));
    return out.join(QLatin1Char('\n'));
}

Report fromLines(const QString &lines)
{
    Report r;
    for (const QString &line : lines.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList f = line.split(QLatin1Char('\t'));
        if (f.value(0) == QLatin1String("P") && f.size() >= 6) {
            Holder h;
            h.pid = f[1].toLongLong();
            h.program = f[2];
            h.user = f[3];
            h.what = f[4].split(QLatin1Char('|'), Qt::SkipEmptyParts);
            h.command = f.mid(5).join(QLatin1Char('\t'));
            if (h.pid > 0)
                r.programs << h;
        } else if (f.value(0) == QLatin1String("O") && f.size() >= 2) {
            r.others << f.mid(1).join(QLatin1Char('\t'));
        }
    }
    return r;
}

} // namespace holders
