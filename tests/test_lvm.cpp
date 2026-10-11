// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// The LVM view: a volume group on two loop devices shows up like a drive, its logical
// volumes as the volumes (an active one with a file system, an inactive one), its free
// space at the end, and the loop devices say they're part of it. An active logical volume
// mounts and unmounts like any partition. Root only; skipped without lvm2 and
// udisks2-lvm2. Everything it makes is taken apart again.

#include "testkit.h"

#include "../src/common/format.h"
#include "../src/drives/udisks.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <functional>

namespace {

const Disk *groupIn(UDisks &udisks)
{
    for (const Disk &d : udisks.disks()) {
        if (d.isLvm && d.device == QLatin1String("/dev/dftestvg"))
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

void lvmTests()
{
    if (QStandardPaths::findExecutable(QStringLiteral("lvcreate")).isEmpty() || !QFile::exists(QStringLiteral("/usr/lib/udisks2/modules/libudisks2_lvm2.so"))) {
        report(true, QStringLiteral("LVM (skipped: lvm2 or udisks2-lvm2 isn't installed)"));
        return;
    }
    QTemporaryDir dir;
    QStringList loops;
    for (const char *name : {"a.img", "b.img"}) {
        const QString image = dir.filePath(QLatin1String(name));
        QFile f(image);
        if (!f.open(QIODevice::WriteOnly) || !f.resize(64 * 1024 * 1024))
            return report(false, QStringLiteral("make two images for a volume group"));
        f.close();
        loops << sh(QStringLiteral("losetup"), {QStringLiteral("-f"), QStringLiteral("--show"), image}).trimmed();
    }
    auto cleanup = [&loops] {
        sh(QStringLiteral("umount"), {QStringLiteral("/dev/dftestvg/data")});
        sh(QStringLiteral("vgremove"), {QStringLiteral("-q"), QStringLiteral("-f"), QStringLiteral("dftestvg")});
        for (const QString &loop : std::as_const(loops)) {
            sh(QStringLiteral("pvremove"), {QStringLiteral("-q"), loop});
            sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
        }
    };
    int code = -1;
    sh(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("pvcreate -q %1 %2 && vgcreate -q dftestvg %1 %2 && lvcreate -q -L 32M -n data dftestvg "
                                                                    "&& lvcreate -q -L 16M -n logs dftestvg && lvchange -q -an dftestvg/logs "
                                                                    "&& mkfs.ext4 -q -L dfdata /dev/dftestvg/data")
                                                     .arg(loops.value(0), loops.value(1))},
       &code);
    report(code == 0, QStringLiteral("make a volume group with two logical volumes"));
    if (code != 0)
        return cleanup();

    UDisks udisks;
    udisks.setInteractive(false);
    const bool listed = waitFor(udisks, [&] {
        const Disk *g = groupIn(udisks);
        return g && g->volumes.size() == 2 && g->volumes[0].fsType == QLatin1String("ext4");
    });
    const Disk *g = groupIn(udisks);
    const Volume *data = g ? &g->volumes[0] : nullptr;
    const Volume *logs = g && g->volumes.size() > 1 ? &g->volumes[1] : nullptr;
    report(listed && g->model == QLatin1String("LVM volume group dftestvg") && diskKind(*g) == QLatin1String("LVM") && tableName(*g) == QLatin1String("Volume group"),
           QStringLiteral("the volume group shows up like a drive"), g ? g->model : QString());
    report(data && logs && data->partName == QLatin1String("data") && data->lvActive && data->label == QLatin1String("dfdata")
               && data->size == 32ull * 1024 * 1024 && logs->partName == QLatin1String("logs") && !logs->lvActive && logs->offset == data->size,
           QStringLiteral("its logical volumes are its volumes: one active with ext4, one inactive"));
    QVector<Span> spans = g ? diskSpans(*g) : QVector<Span>();
    report(!spans.isEmpty() && spans.last().isFree() && spans.last().size >= 60ull * 1024 * 1024,
           QStringLiteral("its free space is the unallocated part at the end"), spans.isEmpty() ? QString() : formatSize(spans.last().size));
    int members = 0;
    for (const Disk &d : udisks.disks()) {
        if (loops.contains(d.device) && d.lvmMemberOf == QLatin1String("dftestvg"))
            ++members;
    }
    report(members == 2, QStringLiteral("both loop devices say they're part of it"), QString::number(members));

    // An active logical volume mounts and unmounts like any partition.
    if (data) {
        QString message;
        bool ok = false, finished = false;
        auto connection = QObject::connect(&udisks, &UDisks::operationFinished, [&](bool success, const QString &text) {
            ok = success;
            message = text;
            finished = true;
        });
        udisks.mount(*data);
        waitFor(udisks, [&] { return finished; });
        const bool mounted = ok && waitFor(udisks, [&] { return groupIn(udisks) && !groupIn(udisks)->volumes[0].mountPoints.isEmpty(); });
        report(mounted, QStringLiteral("an active logical volume mounts"), message);
        finished = false;
        if (const Disk *now = groupIn(udisks))
            udisks.unmount(now->volumes[0]);
        waitFor(udisks, [&] { return finished; });
        QObject::disconnect(connection);
        report(ok && waitFor(udisks, [&] { return groupIn(udisks) && groupIn(udisks)->volumes[0].mountPoints.isEmpty(); }),
               QStringLiteral("and unmounts"), message);
    }
    cleanup();
}
