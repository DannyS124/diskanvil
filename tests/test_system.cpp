// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Disk Cleanup's file handling, snapper and Btrfs subvolumes, and the systemd units
// behind Optimize Drives.

#include "testkit.h"

#include "../src/health/btrfscheck.h"
#include "../src/tools/cleanup.h"
#include "../src/tools/snapper.h"
#include "../src/tools/systemd.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QThread>
#include <QTemporaryDir>

#include <sys/stat.h>
#include <unistd.h>

namespace {

void writeFile(const QString &path, qint64 size)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(QByteArray(size, 'x'));
}

} // namespace

void cleanupTests()
{
    QTemporaryDir dir;
    const QString cache = dir.filePath(QStringLiteral("cache"));
    const QString outside = dir.filePath(QStringLiteral("outside"));
    writeFile(cache + QStringLiteral("/a/b/c/deep.bin"), 300000);
    writeFile(cache + QStringLiteral("/thumbs/1.png"), 20000);
    writeFile(cache + QStringLiteral("/top.bin"), 100000);
    writeFile(outside + QStringLiteral("/keep.txt"), 5000);
    writeFile(outside + QStringLiteral("/dir/keep-too.txt"), 5000);
    writeFile(outside + QStringLiteral("/linked.bin"), 400000);
    QFile::link(outside + QStringLiteral("/keep.txt"), cache + QStringLiteral("/link-to-file"));
    QFile::link(outside + QStringLiteral("/dir"), cache + QStringLiteral("/link-to-dir"));
    const bool hardLinked = ::link(QFile::encodeName(outside + QStringLiteral("/linked.bin")).constData(),
                                   QFile::encodeName(cache + QStringLiteral("/hardlink.bin")).constData()) == 0;

    const quint64 du = sh(QStringLiteral("du"), {QStringLiteral("-sxB1"), cache}).section(QLatin1Char('\t'), 0, 0).toULongLong();
    const quint64 size = cleanup::folderSize(cache);
    // du counts the folder itself; folderSize counts what's inside it.
    report(size > 0 && du >= size && du - size <= 4096 * 4, QStringLiteral("folder size matches du"), QStringLiteral("%1 vs %2").arg(size).arg(du));

    // A folder the user can't write to: its contents can't go, and that's reported.
    writeFile(cache + QStringLiteral("/locked/stuck.bin"), 1000);
    ::chmod(QFile::encodeName(cache + QStringLiteral("/locked")).constData(), 0555);
    const cleanup::Result r = cleanup::removeContents(cache);
    ::chmod(QFile::encodeName(cache + QStringLiteral("/locked")).constData(), 0755);
    const QStringList left = QDir(cache).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System | QDir::Hidden);
    report(left == QStringList{QStringLiteral("locked")} && QFileInfo::exists(cache), QStringLiteral("everything else inside is deleted, the folder stays"),
           left.join(QLatin1Char(',')));
    report(r.failed == 1 && r.firstError.contains(QLatin1String("stuck.bin")), QStringLiteral("what couldn't be deleted is reported"), r.firstError);
    report(QFile::exists(outside + QStringLiteral("/keep.txt")) && QFile::exists(outside + QStringLiteral("/dir/keep-too.txt")),
           QStringLiteral("links are removed, not what they point to"));
    report(hardLinked && QFile::exists(outside + QStringLiteral("/linked.bin")) && r.freed < 500000,
           QStringLiteral("a hard-linked file doesn't count as freed"), QString::number(r.freed));
    report(cleanup::removeContents(dir.filePath(QStringLiteral("missing"))).failed == 0, QStringLiteral("a missing folder is simply nothing to do"));

    // Trash: files and info go, the folders stay.
    const QString trash = dir.filePath(QStringLiteral("Trash"));
    writeFile(trash + QStringLiteral("/files/old.txt"), 7000);
    writeFile(trash + QStringLiteral("/info/old.txt.trashinfo"), 100);
    writeFile(trash + QStringLiteral("/directorysizes"), 10);
    const cleanup::Result t = cleanup::emptyTrash(trash);
    report(t.failed == 0 && QDir(trash + QStringLiteral("/files")).isEmpty() && QDir(trash + QStringLiteral("/info")).isEmpty()
               && !QFile::exists(trash + QStringLiteral("/directorysizes")),
           QStringLiteral("emptying the trash"));

    report(cleanup::parsePaccacheSaved(QStringLiteral("==> finished dry run: 6 candidates (disk space saved: 167.03 MiB)")) == qint64(167.03 * 1024 * 1024)
               && cleanup::parsePaccacheSaved(QStringLiteral("==> no candidate packages found for pruning")) == 0
               && cleanup::parsePaccacheSaved(QStringLiteral("(disk space saved: 2 GiB)")) == 2ll * 1024 * 1024 * 1024,
           QStringLiteral("reads what paccache would free"));
}

void snapperTests()
{
    const QByteArray sample =
        "29 1 0:26 /@ / rw,noatime shared:1 - btrfs /dev/nvme0n1p2 rw,ssd,discard=async,space_cache=v2,subvolid=256,subvol=/@\n"
        "45 29 0:26 /@my\\040stuff /mnt/my\\040stuff rw,noatime shared:5 - btrfs /dev/nvme0n1p2 rw,subvolid=300,subvol=/@my stuff\n"
        "50 29 8:1 / /boot rw - vfat /dev/nvme0n1p1 rw\n";
    const QVector<Subvolume> subs = snapper::mountedSubvolumes(sample);
    report(subs.size() == 2 && subs[0].path == QLatin1String("/@") && subs[0].id == 256 && subs[0].device == QLatin1String("/dev/nvme0n1p2"),
           QStringLiteral("Btrfs subvolumes are read from mountinfo"));
    report(subs.size() == 2 && subs[1].mountPoint == QLatin1String("/mnt/my stuff") && subs[1].path == QLatin1String("/@my stuff"),
           QStringLiteral("spaces in names are decoded"));
    report(subs.size() == 2 && subs[0].options.contains(QLatin1String("discard=async")), QStringLiteral("mount options are kept"));

    if (!snapper::available()) {
        out << "SKIP  snapper isn't installed" << Qt::endl;
        return;
    }
    QString error;
    const QVector<SnapperConfig> configs = snapper::configs(&error);
    report(error.isEmpty() || error.contains(QLatin1String("ALLOW_USERS")), QStringLiteral("snapper answers"), error);
    for (const SnapperConfig &c : configs) {
        const QVector<Snapshot> list = snapper::snapshots(c.name, &error);
        bool ordered = !list.isEmpty() && list.first().number == 0;
        for (qsizetype i = 1; i < list.size(); ++i)
            ordered = ordered && list[i].number > list[i - 1].number && list[i].date.isValid();
        report(error.isEmpty() && ordered, QStringLiteral("snapshots of \"%1\" read back in order").arg(c.name),
               QStringLiteral("%1 snapshots").arg(list.size() - 1));
    }
}

// Btrfs on a loop device: a subvolume mounted on its own shows up with its ID. Root only.
void btrfsTests()
{
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("btrfs.img"));
    QFile f(image);
    if (!f.open(QIODevice::WriteOnly) || !f.resize(300ll * 1024 * 1024))
        return report(false, QStringLiteral("create a Btrfs image"));
    f.close();
    int code = -1;
    sh(QStringLiteral("mkfs.btrfs"), {QStringLiteral("-q"), image}, &code);
    const QString top = dir.filePath(QStringLiteral("top"));
    const QString sub = dir.filePath(QStringLiteral("sub"));
    QDir().mkpath(top);
    QDir().mkpath(sub);
    sh(QStringLiteral("mount"), {QStringLiteral("-o"), QStringLiteral("loop"), image, top}, &code);
    report(code == 0, QStringLiteral("mount a Btrfs test file system"));
    sh(QStringLiteral("btrfs"), {QStringLiteral("subvolume"), QStringLiteral("create"), top + QStringLiteral("/@data")});
    const QString id = sh(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("btrfs subvolume show '%1/@data' | sed -n 's/.*Subvolume ID:[[:space:]]*//p'").arg(top)});
    sh(QStringLiteral("mount"), {QStringLiteral("-o"), QStringLiteral("loop,subvol=/@data"), image, sub}, &code);
    bool found = false;
    for (const Subvolume &s : snapper::mountedSubvolumes()) {
        if (s.mountPoint == sub)
            found = s.path == QLatin1String("/@data") && QString::number(s.id) == id;
    }
    report(code == 0 && found, QStringLiteral("a mounted subvolume shows up with its ID"), id);

    // Its error counts (all zero), and a real scrub through systemd's unit when it's installed.
    const QString source = sh(QStringLiteral("findmnt"), {QStringLiteral("-n"), QStringLiteral("-o"), QStringLiteral("SOURCE"), top}).trimmed();
    const btrfscheck::Counts counts = btrfscheck::read(QFileInfo(source.section(QLatin1Char('['), 0, 0)).fileName());
    report(counts.known && counts.total() == 0 && counts.devices == 1, QStringLiteral("its error counts are read from sysfs (all zero)"), source);
    Systemd systemd;
    const QString unit = btrfscheck::scrubUnit(top);
    if (!systemd.unitExists(unit)) {
        report(true, QStringLiteral("scrub through systemd (skipped: no btrfs-scrub@ units)"));
    } else {
        bool started = false;
        QEventLoop loop;
        systemd.ref(unit);
        systemd.startUnit(unit, [&](bool ok, const QString &) {
            started = ok;
            loop.quit();
        });
        loop.exec();
        QElapsedTimer waited;
        waited.start();
        Systemd::ServiceState state = systemd.serviceState(unit);
        while ((state.running() || state.exited == 0) && waited.elapsed() < 60000) {
            QThread::msleep(200);
            state = systemd.serviceState(unit);
        }
        // btrfs-progs keeps a status file per scrubbed file system; this one was only for the test.
        const QString uuid = sh(QStringLiteral("blkid"), {QStringLiteral("-s"), QStringLiteral("UUID"), QStringLiteral("-o"), QStringLiteral("value"), image}).trimmed();
        if (!uuid.isEmpty() && !uuid.contains(QLatin1Char('/')))
            QFile::remove(QStringLiteral("/var/lib/btrfs/scrub.status.") + uuid);
        report(started && state.exists && !state.running() && state.conditionMet && state.exitStatus == 0 && state.result == QLatin1String("success"),
               QStringLiteral("a scrub runs through %1 and finds nothing").arg(unit),
               QStringLiteral("%1 %2 exit %3, started %4, exists %5, condition %6, ran %7 s")
                   .arg(state.active, state.result)
                   .arg(state.exitStatus)
                   .arg(started)
                   .arg(state.exists)
                   .arg(state.conditionMet)
                   .arg(state.exited && state.started ? double(state.exited - state.started) / 1e6 : -1.0));
    }
    sh(QStringLiteral("umount"), {sub});
    sh(QStringLiteral("umount"), {top});
}

// Optimize Drives' weekly switch, put back the way it was. Root (no password prompts).
void optimizeTests()
{
    Systemd systemd;
    systemd.setInteractive(false);
    const QString timer = QStringLiteral("fstrim.timer");
    const QString before = systemd.unitFileState(timer);
    report(!before.isEmpty(), QStringLiteral("fstrim.timer exists"), before);
    auto set = [&](bool on) {
        bool ok = false;
        QString message;
        QEventLoop loop;
        systemd.setTimerEnabled(timer, on, [&](bool success, const QString &m) {
            ok = success;
            message = m;
            loop.quit();
        });
        loop.exec();
        return ok ? QString() : message;
    };
    const bool wasOn = before == QLatin1String("enabled");
    QString error = set(!wasOn);
    report(error.isEmpty() && (systemd.unitFileState(timer) == QLatin1String("enabled")) == !wasOn, QStringLiteral("switch weekly trimming"), error);
    error = set(wasOn);
    report(error.isEmpty() && systemd.unitFileState(timer) == before, QStringLiteral("and back the way it was"), systemd.unitFileState(timer));
}
