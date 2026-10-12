// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A test disk that writes slowly, so a wipe can be stopped halfway: a small image behind a
// device-mapper "delay" target, with a loop device on top so UDisks lists it like any disk
// image (DiskAnvil doesn't list device-mapper devices). Root only.

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QStringList>
#include <QThread>

struct SlowDisk {
    QString image;
    QString base;   // loop device holding the image
    QString mapper; // the delay target's name
    QString loop;   // what DiskAnvil sees
    QString error;

    static QString run(const QString &program, const QStringList &args, int *code = nullptr)
    {
        QProcess p;
        p.start(program, args);
        p.waitForFinished(60000);
        if (code)
            *code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
        return QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
    }

    bool create(const QString &dir, int sizeMiB, int writeDelayMs)
    {
        image = dir + QStringLiteral("/slow.img");
        QFile f(image);
        if (!f.open(QIODevice::WriteOnly) || !f.resize(qint64(sizeMiB) * 1024 * 1024)) {
            error = QStringLiteral("couldn't make the image");
            return false;
        }
        f.close();
        base = run(QStringLiteral("losetup"), {QStringLiteral("-f"), QStringLiteral("--show"), image});
        const QString sectors = run(QStringLiteral("blockdev"), {QStringLiteral("--getsz"), base});
        mapper = QStringLiteral("diskanvil-slow-%1").arg(QCoreApplication::applicationPid());
        int code = -1;
        run(QStringLiteral("dmsetup"), {QStringLiteral("create"), mapper, QStringLiteral("--table"),
                                        QStringLiteral("0 %1 delay %2 0 0 %2 0 %3").arg(sectors, base).arg(writeDelayMs)}, &code);
        if (base.isEmpty() || code != 0) {
            error = QStringLiteral("dmsetup create failed (base %1)").arg(base);
            return false;
        }
        run(QStringLiteral("udevadm"), {QStringLiteral("settle")});
        loop = run(QStringLiteral("losetup"), {QStringLiteral("-f"), QStringLiteral("--show"), QStringLiteral("--direct-io=on"),
                                               QStringLiteral("/dev/mapper/") + mapper});
        run(QStringLiteral("udevadm"), {QStringLiteral("settle")});
        if (loop.isEmpty())
            error = QStringLiteral("losetup on the delay target failed");
        return !loop.isEmpty();
    }

    void remove()
    {
        if (!loop.isEmpty())
            run(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
        run(QStringLiteral("udevadm"), {QStringLiteral("settle")});
        for (int tries = 0; tries < 20 && !mapper.isEmpty(); ++tries) {
            int code = -1;
            run(QStringLiteral("dmsetup"), {QStringLiteral("remove"), mapper}, &code);
            if (code == 0)
                break;
            QThread::msleep(250); // udev may still hold it for a moment
        }
        if (!base.isEmpty())
            run(QStringLiteral("losetup"), {QStringLiteral("-d"), base});
        if (!image.isEmpty())
            QFile::remove(image);
        loop.clear();
        mapper.clear();
        base.clear();
    }
};
