// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// RAID status: a RAID 1 array on two loop devices shows up like a drive, its members say
// so, a check can be asked for, and after one member fails it's degraded with a warning.
// Root only (mdadm); everything it makes is taken apart again.

#include "testkit.h"

#include "../src/common/format.h"
#include "../src/drives/udisks.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include <functional>

namespace {

const Disk *arrayIn(UDisks &udisks)
{
    for (const Disk &d : udisks.disks()) {
        if (d.isRaid && d.model.endsWith(QLatin1String("dftest")))
            return &d;
    }
    return nullptr;
}

bool waitFor(UDisks &udisks, const std::function<bool()> &done, int ms = 20000)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        udisks.refresh();
        if (done())
            return true;
        QCoreApplication::processEvents();
        QThread::msleep(200);
    }
    return false;
}

} // namespace

void raidTests()
{
    QTemporaryDir dir;
    QStringList loops;
    for (const char *name : {"a.img", "b.img"}) {
        const QString image = dir.filePath(QLatin1String(name));
        QFile f(image);
        if (!f.open(QIODevice::WriteOnly) || !f.resize(64 * 1024 * 1024))
            return report(false, QStringLiteral("make two images for an array"));
        f.close();
        loops << sh(QStringLiteral("losetup"), {QStringLiteral("-f"), QStringLiteral("--show"), image}).trimmed();
    }
    int code = -1;
    sh(QStringLiteral("mdadm"), {QStringLiteral("--create"), QStringLiteral("/dev/md/dftest"), QStringLiteral("--level=1"), QStringLiteral("--raid-devices=2"),
                                 QStringLiteral("--metadata=1.2"), QStringLiteral("--assume-clean"), QStringLiteral("--run"), QStringLiteral("--quiet"),
                                 loops.value(0), loops.value(1)},
       &code);
    report(code == 0, QStringLiteral("make a RAID 1 array on two loop devices"));
    auto cleanup = [&loops] {
        sh(QStringLiteral("mdadm"), {QStringLiteral("--stop"), QStringLiteral("/dev/md/dftest")});
        for (const QString &loop : std::as_const(loops)) {
            sh(QStringLiteral("mdadm"), {QStringLiteral("--zero-superblock"), loop});
            sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
        }
    };
    if (code != 0)
        return cleanup();

    UDisks udisks;
    udisks.setInteractive(false);
    // UDisks fills in an array and its members over a moment: wait for the whole picture.
    auto members = [&] {
        const Disk *array = arrayIn(udisks);
        int count = 0;
        for (const Disk &d : udisks.disks()) {
            if (array && loops.contains(d.device) && d.raidMemberOf == shortDevice(array->device))
                ++count;
        }
        return count;
    };
    const bool listed = waitFor(udisks, [&] {
        const Disk *array = arrayIn(udisks);
        return array && array->raidDevices == 2 && array->raidLevel == QLatin1String("raid1") && members() == 2;
    });
    const Disk *a = arrayIn(udisks);
    report(listed && a && a->raidLevel == QLatin1String("raid1") && a->raidDevices == 2 && a->raidDegraded == 0 && a->raidRunning
               && a->model == QLatin1String("RAID 1 array dftest") && a->health.state != Health::State::Warning,
           QStringLiteral("it shows up like a drive: RAID 1, two drives, all there"), a ? a->model + QStringLiteral(" ") + a->device : QString());
    report(members() == 2, QStringLiteral("both loop devices say they're members of it"), QString::number(members()));

    if (a) {
        QString message;
        bool ok = false, finished = false;
        auto connection = QObject::connect(&udisks, &UDisks::operationFinished, [&](bool success, const QString &text) {
            ok = success;
            message = text;
            finished = true;
        });
        udisks.raidSyncAction(*a, QStringLiteral("check"));
        waitFor(udisks, [&] { return finished; });
        QObject::disconnect(connection);
        report(ok && message.startsWith(QLatin1String("Checking")), QStringLiteral("a check can be asked for"), message);
        waitFor(udisks, [&] {
            const Disk *now = arrayIn(udisks);
            return now && (now->raidSync.isEmpty() || now->raidSync == QLatin1String("idle"));
        }, 60000);
    }

    sh(QStringLiteral("mdadm"), {QStringLiteral("--manage"), QStringLiteral("/dev/md/dftest"), QStringLiteral("--fail"), loops.value(1)}, &code);
    const bool degraded = waitFor(udisks, [&] {
        const Disk *now = arrayIn(udisks);
        return now && now->raidDegraded == 1;
    });
    a = arrayIn(udisks);
    bool warned = false;
    for (const HealthReason &r : a ? a->health.reasons : QVector<HealthReason>())
        warned = warned || (r.code == QLatin1String("raid-degraded") && r.level == HealthReason::Level::Warning);
    report(code == 0 && degraded && a && a->health.state == Health::State::Warning && warned,
           QStringLiteral("with one member failed, it's degraded and warns"), a ? a->health.summary : QString());
    cleanup();
}
