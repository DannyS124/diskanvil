// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "winre.h"

#include <QCoreApplication>
#include <QRegularExpression>

namespace winre {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("winre", text);
}

} // namespace

Location parse(const QString &info)
{
    Location at;
    static const QRegularExpression path(QStringLiteral("harddisk(\\d+)\\\\partition(\\d+)\\\\"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = path.match(info);
    if (m.hasMatch()) {
        at.disk = m.captured(1).toInt();
        at.partition = m.captured(2).toInt();
    }
    return at;
}

Plan plan(const Location &at, const QVector<Disk> &disks, const QString &systemMount)
{
    Plan p;
    if (at.disk < 0) {
        p.problem = tr("Windows' recovery is switched off (reagentc /enable switches it on)");
        return p;
    }
    p.diskNumber = at.disk;
    p.rePartition = at.partition;
    const Disk *disk = nullptr;
    for (const Disk &d : disks) {
        // blockPath is \\.\PhysicalDriveN on Windows (device is "Disk N", for people).
        if (d.blockPath.endsWith(QStringLiteral("PhysicalDrive%1").arg(at.disk), Qt::CaseInsensitive))
            disk = &d;
    }
    if (!disk) {
        p.problem = tr("its drive isn't listed");
        return p;
    }
    p.disk = disk->device;
    p.gpt = disk->tableType == QLatin1String("gpt");
    const Volume *re = nullptr, *os = nullptr;
    for (const Volume &v : disk->volumes) {
        if (v.number == at.partition)
            re = &v;
        for (const QString &m : v.mountPoints) {
            if (m.compare(systemMount, Qt::CaseInsensitive) == 0)
                os = &v;
        }
    }
    if (!re) {
        p.problem = tr("its partition isn't listed");
        return p;
    }
    p.reSize = re->size;
    p.reFree = re->fsFree;
    p.needed = re->fsTotal > 0 && re->fsFree < kRoom;
    if (!os) {
        p.problem = tr("Windows' own partition isn't on the same drive as its recovery partition");
        return p;
    }
    p.osPartition = os->number;
    p.osFree = os->fsFree;
    // Microsoft's fix is for a recovery partition right after Windows' own.
    for (const Volume &v : disk->volumes) {
        if (&v != os && &v != re && !v.isContainer && v.offset >= os->offset + os->size && v.offset < re->offset) {
            p.problem = tr("something lies between Windows' partition and the recovery partition");
            return p;
        }
    }
    if (re->offset < os->offset + os->size) {
        p.problem = tr("the recovery partition isn't after Windows' partition (Microsoft's fix needs it there)");
        return p;
    }
    if (os->fsFree < kRoom + 1024ull * 1024 * 1024) {
        p.problem = tr("Windows' partition needs about 1.25 GB free for it");
        return p;
    }
    return p;
}

QString diskpartScript(const Plan &p)
{
    QStringList s;
    s << QStringLiteral("select disk %1").arg(p.diskNumber) << QStringLiteral("select partition %1").arg(p.osPartition)
      << QStringLiteral("shrink desired=250 minimum=250") << QStringLiteral("select partition %1").arg(p.rePartition)
      << QStringLiteral("delete partition override");
    if (p.gpt)
        s << QStringLiteral("create partition primary id=de94bba4-06d1-4d40-a16a-bfd50179d6ac") << QStringLiteral("gpt attributes=0x8000000000000001");
    else
        s << QStringLiteral("create partition primary id=27");
    s << QStringLiteral("format quick fs=ntfs label=\"Windows RE tools\"");
    if (!p.gpt)
        s << QStringLiteral("set id=27");
    s << QStringLiteral("exit");
    return s.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n");
}

} // namespace winre
