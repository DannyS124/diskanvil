// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tryrepair.h"

namespace tryrepair {

namespace {

QString q(const QString &text)
{
    QString out = text;
    out.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + out + QLatin1Char('\'');
}

} // namespace

QStringList repairCommand(const QString &fs, const QString &device)
{
    if (fs == QLatin1String("ext4") || fs == QLatin1String("ext3") || fs == QLatin1String("ext2"))
        return {QStringLiteral("e2fsck"), QStringLiteral("-f"), QStringLiteral("-y"), device};
    if (fs == QLatin1String("vfat"))
        return {QStringLiteral("fsck.fat"), QStringLiteral("-a"), QStringLiteral("-w"), device};
    if (fs == QLatin1String("exfat"))
        return {QStringLiteral("fsck.exfat"), QStringLiteral("-y"), device};
    if (fs == QLatin1String("ntfs"))
        return {QStringLiteral("ntfsfix"), device};
    if (fs == QLatin1String("xfs"))
        return {QStringLiteral("xfs_repair"), device};
    if (fs == QLatin1String("btrfs"))
        return {QStringLiteral("btrfs"), QStringLiteral("check"), QStringLiteral("--repair"), QStringLiteral("--force"), device};
    if (fs == QLatin1String("f2fs"))
        return {QStringLiteral("fsck.f2fs"), QStringLiteral("-f"), QStringLiteral("-y"), device};
    return {};
}

QString startScript(const QString &device, const QString &fs, quint64 changeRoom)
{
    QStringList repair;
    for (const QString &word : repairCommand(fs, QStringLiteral("/dev/mapper/%1").arg(QLatin1String(kName))))
        repair << q(word);
    QStringList s;
    s << QStringLiteral("set -e") << QStringLiteral("N=%1").arg(QLatin1String(kName)) << QStringLiteral("COW=/tmp/$N.changes")
      << QStringLiteral("SIZE=$(blockdev --getsz %1)").arg(q(device))
      // The changes, in a sparse file: only what the repair writes takes room.
      << QStringLiteral("rm -f \"$COW\"") << QStringLiteral("truncate -s %1 \"$COW\"").arg(changeRoom)
      << QStringLiteral("LOOP=$(losetup -f --show \"$COW\"); echo \"$LOOP\" > \"$COW.loop\"")
      << QStringLiteral("dmsetup create \"$N\" --table \"0 $SIZE snapshot %1 $LOOP N 8\"").arg(device)
      << QStringLiteral("udevadm settle") << QStringLiteral("set +e")
      << QStringLiteral("echo DISKANVIL-TRY-READY")
      << repair.join(QLatin1Char(' '))
      << QStringLiteral("echo DISKANVIL-TRY-EXIT $?");
    return s.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

QString lookScript(const QString &fs)
{
    // The stand-in has the partition's ID: XFS is told not to mind.
    const QString options = fs == QLatin1String("xfs") ? QStringLiteral("ro,nouuid") : QStringLiteral("ro");
    return QStringLiteral("mkdir -p %1 && mount -o %2 /dev/mapper/%3 %1 && echo DISKANVIL-TRY-MOUNTED\n")
        .arg(QLatin1String(kMount), options, QLatin1String(kName));
}

QString stopScript()
{
    return QStringLiteral("N=%1; COW=/tmp/$N.changes\n"
                          "umount %2 2>/dev/null\n"
                          // udev and UDisks look at the stand-in right after the repair wrote to it: a
                          // remove then finds it busy, so it waits for them and tries again.
                          "udevadm settle --timeout=10 2>/dev/null\n"
                          "dmsetup remove --retry \"$N\" 2>/dev/null\n"
                          "[ -f \"$COW.loop\" ] && losetup -d \"$(cat \"$COW.loop\")\" 2>/dev/null\n"
                          "rm -f \"$COW\" \"$COW.loop\"\n"
                          "echo DISKANVIL-TRY-STOPPED\n")
        .arg(QLatin1String(kName), QLatin1String(kMount));
}

} // namespace tryrepair
