// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "btrfscheck.h"

#include "systemd.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>

namespace {

QByteArray readSmall(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.read(4096) : QByteArray();
}

} // namespace

btrfscheck::Counts btrfscheck::parse(const QByteArray &errorStats)
{
    Counts c;
    for (const QByteArray &line : errorStats.split('\n')) {
        const QList<QByteArray> parts = line.simplified().split(' ');
        if (parts.size() != 2)
            continue;
        bool ok = false;
        const qint64 n = parts[1].toLongLong(&ok);
        if (!ok || n < 0)
            continue;
        const qint64 value = qMin<qint64>(n, 1000000000000LL); // so the total can't overflow
        if (parts[0] == "write_errs")
            c.write = value;
        else if (parts[0] == "read_errs")
            c.read = value;
        else if (parts[0] == "flush_errs")
            c.flush = value;
        else if (parts[0] == "corruption_errs")
            c.corruption = value;
        else if (parts[0] == "generation_errs")
            c.generation = value;
        else
            continue;
        c.known = true;
    }
    return c;
}

btrfscheck::Counts btrfscheck::read(const QString &deviceName, const QString &sysfs)
{
    Counts total;
    if (deviceName.isEmpty() || deviceName.contains(QLatin1Char('/')) || deviceName.startsWith(QLatin1Char('.')))
        return total;
    // The file system's folder is the one that lists the device (its name is the file system's
    // ID, or a made-up one for a clone, so it isn't looked up by ID).
    const QDir root(sysfs);
    for (const QString &fs : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!QFileInfo::exists(root.filePath(fs + QStringLiteral("/devices/") + deviceName)))
            continue;
        const QDir devinfo(root.filePath(fs + QStringLiteral("/devinfo")));
        for (const QString &id : devinfo.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const Counts one = parse(readSmall(devinfo.filePath(id + QStringLiteral("/error_stats"))));
            if (!one.known)
                continue;
            total.known = true;
            total.write += one.write;
            total.read += one.read;
            total.flush += one.flush;
            total.corruption += one.corruption;
            total.generation += one.generation;
            ++total.devices;
        }
        total.tempFsid = readSmall(root.filePath(fs + QStringLiteral("/temp_fsid"))).trimmed() == "1";
        break;
    }
    return total;
}

QString btrfscheck::describe(const Counts &c)
{
    if (!c.known)
        return QObject::tr("Not mounted, so the kernel has no error counts for it.");
    QStringList parts;
    if (c.read)
        parts << QObject::tr("%n read error(s)", nullptr, int(qMin<qint64>(c.read, INT_MAX)));
    if (c.write)
        parts << QObject::tr("%n write error(s)", nullptr, int(qMin<qint64>(c.write, INT_MAX)));
    if (c.flush)
        parts << QObject::tr("%n flush error(s)", nullptr, int(qMin<qint64>(c.flush, INT_MAX)));
    if (c.corruption)
        parts << QObject::tr("%n checksum error(s)", nullptr, int(qMin<qint64>(c.corruption, INT_MAX)));
    if (c.generation)
        parts << QObject::tr("%n out-of-date block(s)", nullptr, int(qMin<qint64>(c.generation, INT_MAX)));
    QString text = parts.isEmpty() ? QObject::tr("No errors recorded.") : QObject::tr("Recorded: %1.").arg(parts.join(QStringLiteral(", ")));
    if (c.devices > 1)
        text += QLatin1Char(' ') + QObject::tr("The counts are for all %1 drives of this file system.").arg(c.devices);
    if (c.tempFsid)
        text += QLatin1Char(' ') + QObject::tr("It's a copy of another file system mounted at the same time, under a made-up ID.");
    return text;
}

QString btrfscheck::scrubMountPoint(const QStringList &mountPoints)
{
    if (mountPoints.contains(QStringLiteral("/")))
        return QStringLiteral("/");
    QString best;
    for (const QString &m : mountPoints) {
        if (!m.isEmpty() && (best.isEmpty() || m.size() < best.size()))
            best = m;
    }
    return best;
}

QString btrfscheck::scrubUnit(const QString &mountPoint)
{
    return QStringLiteral("btrfs-scrub@%1.service").arg(Systemd::escapePath(mountPoint));
}

QString btrfscheck::scrubTimer(const QString &mountPoint)
{
    return QStringLiteral("btrfs-scrub@%1.timer").arg(Systemd::escapePath(mountPoint));
}

QString btrfscheck::scrubOutcome(bool conditionMet, int exitStatus, const QString &result, qint64 errorsBefore, qint64 errorsAfter)
{
    if (!conditionMet)
        return QObject::tr("It wasn't mounted there any more, so nothing was checked.");
    if (exitStatus == 3)
        return QObject::tr("It found errors it couldn't fix. Check the drive's health, and restore damaged files from a backup.");
    if (exitStatus == 0 && result == QLatin1String("success"))
        return errorsAfter > errorsBefore ? QObject::tr("It found errors and repaired them from the second copy.")
                                          : QObject::tr("Everything checked out.");
    return QObject::tr("It didn't finish (%1, exit status %2).").arg(result.isEmpty() ? QObject::tr("stopped") : result).arg(exitStatus);
}
