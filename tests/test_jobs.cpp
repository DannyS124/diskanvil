// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Stopping long jobs. --jobs (as the user): which jobs get a bar, which can be stopped and
// what they say. --stop (root): a real wipe on a slow test disk, stopped halfway.

#include "testkit.h"

#include "slowdisk.h"

#include "../src/common/jobui.h"
#include "../src/drives/udisks.h"

#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <fcntl.h>
#include <unistd.h>

namespace {

Job job(const char *operation, bool cancelable = true, double progress = 0.34)
{
    Job j;
    j.path = QStringLiteral("/org/freedesktop/UDisks2/jobs/1");
    j.operation = QLatin1String(operation);
    j.cancelable = cancelable;
    j.progress = progress;
    j.progressValid = true;
    return j;
}

} // namespace

void jobsTests()
{
    // Unplug and Replug finds the drive again by its serial number, whatever name it comes back under.
    QVector<Disk> disks(3);
    disks[0].device = QStringLiteral("/dev/sda"), disks[0].serial = QStringLiteral("5VC1ABCD"), disks[0].model = QStringLiteral("ST9250315AS");
    disks[1].device = QStringLiteral("/dev/loop0"), disks[1].isLoop = true, disks[1].serial = QStringLiteral("5VC1WXYZ");
    disks[2].device = QStringLiteral("/dev/sde"), disks[2].serial = QStringLiteral("5VC1WXYZ"), disks[2].model = QStringLiteral("ST9250315AS"),
    disks[2].size = 250059350016;
    QVector<Disk> noSerial = disks;
    noSerial[2].serial.clear();
    report(UDisks::findDrive(disks, QStringLiteral("5VC1WXYZ"), QStringLiteral("ST9250315AS"), 250059350016) == 2
               && UDisks::findDrive(disks, QStringLiteral("NOPE"), QStringLiteral("ST9250315AS"), 250059350016) == -1
               && UDisks::findDrive(noSerial, QString(), QStringLiteral("ST9250315AS"), 250059350016) == 2
               && UDisks::findDrive(noSerial, QString(), QStringLiteral("ST9250315AS"), 1) == -1,
           QStringLiteral("a replugged drive is found again by serial (or model and size), never a disk image"));
    // The error from the photo: a Seagate laptop drive in a USB adapter refusing ERASE PREPARE.
    const QString refused = QStringLiteral("Error sending ATA command SECURITY ERASE PREPARE: ATA command failed: error=0x04 count=0x00 status=0x51");
    const QString overUsb = UDisks::secureEraseRefused(refused, true), overSata = UDisks::secureEraseRefused(refused, false);
    report(overUsb.contains(QLatin1String("USB adapters")) && overUsb.contains(QLatin1String("Wipe Disk")) && overUsb.contains(QLatin1String("xxxx")),
           QStringLiteral("a refused Secure Erase over USB says why, what works instead, and about the password"), overUsb.left(80));
    report(overSata.contains(QLatin1String("shut the PC down fully")) && !overSata.contains(QLatin1String("USB")),
           QStringLiteral("and on SATA, to turn the drive off and on"), overSata.left(80));
    report(UDisks::secureEraseRefused(QStringLiteral("Drive is frozen, cannot perform a secure erase"), false).isEmpty()
               && UDisks::secureEraseRefused(QStringLiteral("Not authorized to perform operation"), true).isEmpty(),
           QStringLiteral("other errors get nothing added"));
    const Job wipe = job("format-erase");
    report(jobShown(wipe) && jobCantStop(wipe).isEmpty() && jobVerb(wipe) == QLatin1String("Wiping"), QStringLiteral("a wipe can be stopped"));
    report(jobCantStop(job("filesystem-check")).isEmpty(), QStringLiteral("a check can be stopped (it only reads)"));
    report(!jobCantStop(job("format-erase", false)).isEmpty(), QStringLiteral("not when UDisks says it can't be"));
    for (const char *op : {"ata-secure-erase", "ata-enhanced-secure-erase", "nvme-format-ns", "nvme-sanitize"})
        report(jobShown(job(op)) && jobSelfErasing(job(op)) && jobCantStop(job(op)).contains(QLatin1String("can't be interrupted")),
               QStringLiteral("%1 can't be stopped, and says not to unplug").arg(QLatin1String(op)));
    for (const char *op : {"filesystem-repair", "filesystem-resize", "format-mkfs"})
        report(jobShown(job(op)) && jobCantStop(job(op)).contains(QLatin1String("damage")),
               QStringLiteral("%1 finishes on its own").arg(QLatin1String(op)));
    report(!jobShown(job("filesystem-mount")) && !jobShown(job("ata-smart-selftest")),
           QStringLiteral("quick jobs and self-tests (shown from the drive) get no job bar"));

    Job running = wipe;
    running.rate = 49 * 1000 * 1000;
    running.bytes = 500ULL * 1000 * 1000 * 1000;
    const QString progress = jobProgress(running, 0);
    report(progress.startsWith(QLatin1String("34%")) && progress.contains(QLatin1String("/s")) && progress.contains(QLatin1String("hour")),
           QStringLiteral("the bar shows how far, how fast and how long"), progress);
    Job ending = wipe;
    ending.expectedEnd = 1000000ULL * 600;
    report(jobProgress(ending, 1000000ULL * 300).contains(QLatin1String("5 minute")), QStringLiteral("its own end time is used when it has one"),
           jobProgress(ending, 1000000ULL * 300));
    Job odd = wipe;
    odd.progress = 7.5;
    odd.started = 1000000ULL * 100;
    report(jobProgress(odd, 1000000ULL * 50).startsWith(QLatin1String("100%")), QStringLiteral("odd numbers from UDisks don't go past 100%"));

    report(jobStopQuestion(wipe, QStringLiteral("sdb"), true).contains(QLatin1String("new partition table"))
               && jobStopQuestion(wipe, QStringLiteral("sdb1"), false).contains(QLatin1String("format it"))
               && jobStopButton(wipe) == QLatin1String("Stop Wiping") && jobStoppedMessage(wipe, QStringLiteral("sdb")) == QLatin1String("Stopped wiping sdb at 34%."),
           QStringLiteral("Stop asks first, and says what's left afterwards"));
}

void stopTests()
{
    QTemporaryDir dir;
    SlowDisk slow;
    if (!slow.create(dir.path(), 64, 40)) {
        report(false, QStringLiteral("make a slow test disk"), slow.error);
        slow.remove();
        return;
    }
    // A pattern in the last MiB, to show the wipe never got there.
    const qint64 MiB = 1024 * 1024;
    {
        QFile d(slow.loop);
        if (d.open(QIODevice::ReadWrite) && d.seek(63 * MiB)) {
            d.write(QByteArray(MiB, char(0xAA)));
            d.flush();
            ::fsync(d.handle());
        }
    }

    UDisks udisks;
    udisks.setInteractive(false);
    QString target;
    QElapsedTimer t;
    t.start();
    while (target.isEmpty() && t.elapsed() < 15000) {
        udisks.refresh();
        for (const Disk &d : udisks.disks()) {
            if (d.device == slow.loop)
                target = d.blockPath;
        }
        QThread::msleep(200);
    }
    report(!target.isEmpty(), QStringLiteral("the slow test disk shows up"), slow.loop);
    if (target.isEmpty()) {
        slow.remove();
        return;
    }

    bool finished = false, stopped = false;
    QString message;
    QObject::connect(&udisks, &UDisks::operationFinished, [&](bool, const QString &text) {
        finished = true;
        stopped = udisks.wasStopped();
        message = text;
    });
    udisks.wipe(*udisks.diskByPath(target));
    Job found;
    t.restart();
    while (t.elapsed() < 20000 && !finished) {
        QCoreApplication::processEvents();
        udisks.refresh();
        for (const Job &j : udisks.jobs()) {
            if (j.operation == QLatin1String("format-erase") && j.objects.contains(target) && j.progress > 0.02)
                found = j;
        }
        if (!found.path.isEmpty())
            break;
        QThread::msleep(100);
    }
    report(!found.path.isEmpty() && found.cancelable && found.progress < 0.9, QStringLiteral("the wipe runs as a job that can be stopped"),
           QStringLiteral("%1 at %2%").arg(found.path).arg(int(found.progress * 100)));
    if (!found.path.isEmpty()) {
        udisks.cancelJob(found.path, jobStoppedMessage(found, QStringLiteral("the test disk")));
        t.restart();
        while (!finished && t.elapsed() < 20000) {
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }
        report(finished && stopped && message.startsWith(QLatin1String("Stopped wiping the test disk")),
               QStringLiteral("stopping it reports a stop, not an error"), message);
        udisks.refresh();
        bool gone = true;
        for (const Job &j : udisks.jobs())
            gone = gone && j.path != found.path;
        report(gone, QStringLiteral("the job is gone"));

        QFile d(slow.loop);
        QByteArray head, tail;
        if (d.open(QIODevice::ReadOnly)) {
            head = d.read(MiB);
            d.seek(63 * MiB);
            tail = d.read(MiB);
        }
        report(head == QByteArray(MiB, '\0') && tail == QByteArray(MiB, char(0xAA)), QStringLiteral("the start is wiped and the end isn't"));
    }
    slow.remove();
}
