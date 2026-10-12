// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "triage.h"

#include "health.h"
#include "hostprocess.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>

namespace triage {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("triage", text);
}

using Odds = Advice::Odds;

// The drive's name, or "it" when none was picked.
QString it(const Drive &d)
{
    return d.known && !d.name.isEmpty() ? d.name : tr("the drive");
}

void failingFirst(Advice &a, const Drive &d)
{
    a.odds = Odds::Fair;
    a.odds_why = tr("%1 has spots it can't read. Files can be in them, and every read can make a failing drive worse, so it's read "
                    "once, gently, into a copy on another drive, and the files are looked for in the copy.")
                     .arg(it(d));
    a.dont << tr("Don't check, repair or keep using %1.").arg(it(d));
    a.steps << tr("Rescue Copy: copy what can still be read onto another drive (the easy parts first).")
            << tr("Find Lost Files, on the copy (\"A Disk Image File\"), not on the drive.");
    a.tools << QStringLiteral("rescue-copy") << QStringLiteral("lost-files");
}

} // namespace

QString oddsWords(Odds odds)
{
    switch (odds) {
    case Odds::Good:
        return tr("Good");
    case Odds::Fair:
        return tr("Fair");
    case Odds::Poor:
        return tr("Poor");
    case Odds::Unknown:
        break;
    }
    return tr("It depends");
}

Advice advise(Symptom symptom, const Drive &d)
{
    Advice a;
    const bool failing = d.known && d.failing;
    switch (symptom) {
    case Symptom::Deleted:
        if (failing) {
            failingFirst(a, d);
            break;
        }
        a.tools << QStringLiteral("lost-files");
        a.dont << tr("Don't save anything to %1, not even the files you get back.").arg(it(d))
               << tr("Don't install recovery programs onto it.");
        a.steps << tr("Look in the Recycle Bin first (Find Lost Files lists what's in it too).")
                << tr("Find Lost Files: pick %1, tick what you want back, and save it to another drive.").arg(it(d));
        if (!d.known) {
            a.odds_why = tr("It depends on the drive: pick it, to know more.");
        } else if (d.ssd && d.trim == Trim::Continuous) {
            a.odds = Odds::Poor;
            a.odds_why = tr("%1 is an SSD that erases what's deleted within minutes (TRIM is on), so deleted files are seldom found on it. "
                            "The Recycle Bin, Windows' earlier versions of the drive, and backups (cloud, phone) are better bets.")
                             .arg(it(d));
        } else if (d.ssd && d.trim == Trim::Weekly) {
            a.odds = Odds::Fair;
            a.odds_why = tr("%1 is an SSD that erases what's deleted once a week (TRIM). Files deleted since it last did can still be there: "
                            "look now, before it does again.")
                             .arg(it(d));
        } else if (d.ssd && (d.trim == Trim::Off || d.trim == Trim::Unsupported)) {
            a.odds = Odds::Good;
            a.odds_why = tr("%1 is an SSD that doesn't erase what's deleted (TRIM is off), so deleted files stay until they're written over.")
                             .arg(it(d));
        } else if (d.ssd) {
            a.odds = Odds::Fair;
            a.odds_why = tr("%1 is an SSD: most erase deleted files soon after (TRIM). Look now.").arg(it(d));
        } else {
            a.odds = Odds::Good;
            a.odds_why = tr("Deleted files stay on %1 until something is written over them.").arg(it(d));
        }
        if (d.system) {
            a.odds_why += QLatin1Char(' ') + tr("The running system is on it and writes to it all the time: the sooner the better, and "
                                                 "best from DiskForge Live.");
            a.dont << tr("Don't keep using this computer more than you must.");
        }
        break;
    case Symptom::Formatted:
        if (failing) {
            failingFirst(a, d);
            break;
        }
        a.tools << QStringLiteral("lost-files");
        if (d.known && d.ssd) {
            a.odds = Odds::Poor;
            a.odds_why = tr("Formatting an SSD usually erases all of it: the format tells it the whole drive is free. Some files can still "
                            "come back, but often nothing does.");
        } else {
            a.odds = d.known ? Odds::Good : Odds::Unknown;
            a.odds_why = tr("A quick format only writes a new, empty list of files: the files themselves are still there, until something "
                            "is written over them.");
        }
        a.dont << tr("Don't put new files on %1.").arg(it(d)) << tr("Don't format it again, and never with a \"full\" format: that does erase it.");
        a.steps << tr("Find Lost Files on %1. Its search by what's inside files finds them without their names; names come back where "
                      "the old file system's list is still there.")
                       .arg(it(d));
        break;
    case Symptom::WontOpen:
        if (failing) {
            failingFirst(a, d);
            break;
        }
        a.odds = d.known ? Odds::Good : Odds::Unknown;
        a.odds_why = tr("Usually the drive is fine and only its list of files is damaged: the files are still there.");
        a.dont << tr("Don't format it, even when Windows asks.")
               << tr("Don't let Windows or chkdsk \"repair\" it: a repair can throw away files it can't place.");
        a.steps << tr("Disk Health, to see whether the drive itself is failing.")
                << tr("Find Lost Files reads damaged file systems, and finds files by what's inside them.")
                << tr("Copy what you need to another drive. Then, if Disk Health says it's fine, it can be formatted and used again.");
        a.tools << QStringLiteral("health") << QStringLiteral("lost-files");
        break;
    case Symptom::Failing:
        a.odds = Odds::Fair;
        a.odds_why = tr("What can be saved depends on how far gone it is: the gentler it's read, the more comes back.");
        a.dont << tr("Don't keep using it: every hour it runs can make it worse.") << tr("Don't check, repair or defragment it.")
               << tr("If it clicks, grinds or beeps: turn it off. For files that matter a great deal, a data recovery lab (with a clean "
                     "room) is the safe choice.");
        a.steps << tr("Disk Health shows what the drive says about itself.")
                << tr("Rescue Copy reads what it still can onto another drive, the easy parts first.")
                << tr("Find Lost Files on the copy, not on the drive.");
        a.tools << QStringLiteral("health") << QStringLiteral("rescue-copy") << QStringLiteral("lost-files");
        break;
    case Symptom::PartitionGone:
        if (failing) {
            failingFirst(a, d);
            break;
        }
        a.odds = d.known ? Odds::Good : Odds::Unknown;
        a.odds_why = tr("When a partition disappears, usually only the table that lists them changed: the partition itself is still there.");
        a.dont << tr("Don't make a new partition in the empty space.") << tr("Don't \"initialize\" the drive when Windows offers to.");
        a.steps << tr("Recover Partitions looks for where partitions were, and puts them back in the table.")
                << tr("If it can't: Find Lost Files gets the files out instead.");
        a.tools << QStringLiteral("recover-partitions") << QStringLiteral("lost-files");
        break;
    case Symptom::Ransomware:
        a.odds = Odds::Fair;
        a.odds_why = tr("Sometimes: with a free decryptor, from Windows' earlier versions of the drive, or from the originals the "
                        "ransomware deleted.");
        a.dont << tr("Don't pay first.") << tr("Don't delete the locked files or its notes: a free way to unlock them can come out later.")
               << tr("Don't keep the computer on the network.");
        a.steps << tr("Scan for Viruses, best from DiskForge Live, to take it off the computer.")
                << tr("Find Lost Files: Windows' earlier versions of the drive, and the deleted originals. Its ransomware bar says what it "
                      "is and what can be done.");
        a.tools << QStringLiteral("virus-scan") << QStringLiteral("lost-files");
        break;
    case Symptom::Damaged:
        a.odds = Odds::Fair;
        a.odds_why = tr("Some can be repaired: a file cut short, a picture with a wrong checksum, a film whose index was never written.");
        a.dont << tr("Don't keep opening and saving them in other programs: work on copies.");
        a.steps << tr("Find Lost Files: Inside shows where a file is damaged, and Repair a Copy fixes what can be fixed.");
        a.tools << QStringLiteral("lost-files");
        break;
    case Symptom::WindowsWontStart:
        a.odds = Odds::Good;
        a.odds_why = tr("Your files are usually fine. A DiskForge Live stick starts the computer without Windows, and copies them off.");
        a.dont << tr("Don't reinstall Windows or \"reset this PC\" before your files are safe.");
        a.steps << tr("Make a DiskForge Live stick (on another computer, if this one won't start).")
                << tr("Start this computer from it, and copy your files to another drive (Find Lost Files lists them all).")
                << tr("Then see to Windows: if it stopped starting just after something was installed, Windows Drivers and Services "
                      "shows the newest drivers, and switches the suspect off.");
        a.tools << QStringLiteral("rescue-usb") << QStringLiteral("lost-files") << QStringLiteral("drivers");
        break;
    }
    return a;
}

Trim trimOf(const Disk &disk)
{
    if (disk.isLoop || disk.removable || disk.rotationRate != 0)
        return Trim::NotSsd;
    if (!disk.discard)
        return Trim::Unsupported;
#ifdef Q_OS_WIN
    // Windows sends TRIM as files are deleted, unless it's switched off for NTFS.
    QProcess fsutil;
    fsutil.start(QStringLiteral("fsutil"), {QStringLiteral("behavior"), QStringLiteral("query"), QStringLiteral("DisableDeleteNotify")});
    if (!fsutil.waitForFinished(5000))
        return Trim::Unknown;
    static const QRegularExpression ntfs(QStringLiteral("NTFS DisableDeleteNotify\\s*=\\s*(\\d)"));
    const QRegularExpressionMatch m = ntfs.match(QString::fromLocal8Bit(fsutil.readAll()));
    return !m.hasMatch() ? Trim::Unknown : m.captured(1) == QLatin1String("0") ? Trim::Continuous : Trim::Off;
#else
    // Mounted with "discard": as files are deleted. Else the weekly fstrim, when it's switched on.
    QFile mounts(QStringLiteral("/proc/self/mounts"));
    if (mounts.open(QIODevice::ReadOnly)) {
        QStringList devices = {disk.device};
        for (const Volume &v : disk.volumes)
            devices << v.device << v.cleartextDevice;
        for (const QByteArray &line : mounts.readAll().split('\n')) {
            const QList<QByteArray> fields = line.split(' ');
            if (fields.size() < 4 || !devices.contains(QString::fromLocal8Bit(fields[0])))
                continue;
            for (const QByteArray &option : fields[3].split(',')) {
                if (option == "discard" || option.startsWith("discard="))
                    return Trim::Continuous;
            }
        }
    }
    return host::run({QStringLiteral("systemctl"), QStringLiteral("is-enabled"), QStringLiteral("--quiet"), QStringLiteral("fstrim.timer")}) == 0 ? Trim::Weekly
                                                                                                                                              : Trim::Off;
#endif
}

Drive facts(const Disk &disk, bool runningFromStick)
{
    Drive d;
    d.known = true;
    d.name = disk.model.isEmpty() ? disk.device : disk.model;
    d.ssd = !disk.isLoop && !disk.removable && disk.rotationRate == 0;
    d.removable = disk.removable || disk.bus == QLatin1String("usb");
    d.failing = health::hasUnreadableSpots(disk.health);
    d.system = disk.isSystem && !runningFromStick;
    // From DiskForge Live nothing trims the computer's own drives, but what happened before it
    // started isn't known here.
    d.trim = runningFromStick ? Trim::Unknown : trimOf(disk);
    return d;
}

} // namespace triage
