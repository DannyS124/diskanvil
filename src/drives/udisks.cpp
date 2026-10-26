// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "udisks.h"

#include "partrecover.h"

#include "applog.h"
#include "hostprocess.h"
#include "dbusnames.h"
#include "btrfscheck.h"
#include "format.h"
#include "health.h"
#include "rescuestick.h"

#include <QCollator>
#include <QFileInfo>
#include <QTimer>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QMap>
#include <QStorageInfo>
#include <QVariantMap>

#include <algorithm>
#include <unistd.h>
#include <memory>

namespace {

using InterfaceMap = QMap<QString, QVariantMap>;
using ObjectMap = QMap<QDBusObjectPath, InterfaceMap>;

// UDisks gave up waiting for the kernel and udev to catch up after a change of its own.
bool isSyncTimeout(const QString &error)
{
    return error.contains(QLatin1String("Error synchronizing after"));
}

// How long to let udev settle before each new try, in seconds. eMMC (mmcblk) is slow to show
// a change; DISKANVIL_SYNC_WAIT ("10,30,60") overrides it for odd hardware and the tests.
QVector<int> syncWaits(const QString &device)
{
    QVector<int> waits;
    for (const QString &part : qEnvironmentVariable("DISKANVIL_SYNC_WAIT").split(QLatin1Char(','), Qt::SkipEmptyParts))
        waits << std::clamp(part.trimmed().toInt(), 0, 600);
    if (!waits.isEmpty())
        return waits;
    if (kernelName(device).startsWith(QLatin1String("mmcblk")))
        return {10, 30, 60};
    return {5, 15, 30};
}

// What the kernel said about a drive in the last ten minutes (its last 10 lines), for a failure's
// message. journalctl reads it as an ordinary user on most systems; DiskAnvil Live's sudo otherwise.
QString kernelLines(const QString &device)
{
    const QString name = kernelName(device);
    if (name.isEmpty())
        return {};
    const auto read = [](const QStringList &command) {
        QProcess p;
        p.start(command.first(), command.mid(1));
        if (!p.waitForFinished(5000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
            p.kill();
            return QString();
        }
        return QString::fromLocal8Bit(p.readAllStandardOutput());
    };
    QString log = read(host::command({QStringLiteral("journalctl"), QStringLiteral("-k"), QStringLiteral("--since"), QStringLiteral("-10min"),
                                      QStringLiteral("--no-pager"), QStringLiteral("-o"), QStringLiteral("short")}));
    if (log.trimmed().isEmpty() && rescue::runningInRescue())
        log = read({QStringLiteral("sudo"), QStringLiteral("-n"), QStringLiteral("dmesg")});
    QStringList lines;
    const QRegularExpression word(QStringLiteral("\\b%1").arg(QRegularExpression::escape(name)));
    for (const QString &line : log.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        if (line.contains(word))
            lines << line.trimmed();
    }
    if (lines.isEmpty())
        return {};
    return UDisks::tr("What Linux said about %1:").arg(name) + QLatin1Char('\n') + lines.mid(std::max<qsizetype>(0, lines.size() - 10)).join(QLatin1Char('\n'));
}

// "ay" byte strings, NUL-terminated
QString byteString(const QVariant &value)
{
    QByteArray bytes = value.toByteArray();
    if (bytes.endsWith('\0'))
        bytes.chop(1);
    return QString::fromLocal8Bit(bytes);
}

QStringList byteStringList(const QVariant &value)
{
    QStringList out;
    if (!value.canConvert<QDBusArgument>())
        return out;
    const QDBusArgument arg = value.value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd()) {
        QByteArray bytes;
        arg >> bytes;
        if (bytes.endsWith('\0'))
            bytes.chop(1);
        out << QString::fromLocal8Bit(bytes);
    }
    arg.endArray();
    // shortest first so "/" leads for btrfs subvolumes
    std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    return out;
}

QString objectPath(const QVariant &value)
{
    return value.value<QDBusObjectPath>().path();
}

// Anything mounted outside these counts as the running system.
bool isUserMount(const QString &mountPoint)
{
    return mountPoint.startsWith(QLatin1String("/run/media/"))
        || mountPoint.startsWith(QLatin1String("/media/"))
        || mountPoint.startsWith(QLatin1String("/mnt/"));
}

Volume makeVolume(const QString &path, const InterfaceMap &ifaces)
{
    const QVariantMap block = ifaces.value(kBlock);
    Volume v;
    v.objectPath = path;
    v.device = byteString(block.value(QStringLiteral("PreferredDevice")));
    if (v.device.isEmpty())
        v.device = byteString(block.value(QStringLiteral("Device")));
    v.size = block.value(QStringLiteral("Size")).toULongLong();
    // Names and IDs come from the drive itself; cleanName() drops hidden characters.
    v.label = cleanName(block.value(QStringLiteral("IdLabel")).toString());
    v.fsType = block.value(QStringLiteral("IdType")).toString();
    v.fsUsage = block.value(QStringLiteral("IdUsage")).toString();
    v.uuid = cleanName(block.value(QStringLiteral("IdUUID")).toString());

    if (ifaces.contains(kPartition)) {
        const QVariantMap part = ifaces.value(kPartition);
        v.number = part.value(QStringLiteral("Number")).toInt();
        v.offset = part.value(QStringLiteral("Offset")).toULongLong();
        v.size = part.value(QStringLiteral("Size")).toULongLong();
        v.partType = part.value(QStringLiteral("Type")).toString();
        v.partFlags = part.value(QStringLiteral("Flags")).toULongLong();
        v.partName = cleanName(part.value(QStringLiteral("Name")).toString());
        v.partUuid = part.value(QStringLiteral("UUID")).toString();
        v.isContainer = part.value(QStringLiteral("IsContainer")).toBool();
        v.isContained = part.value(QStringLiteral("IsContained")).toBool();
    }
    v.isEfi = v.partType.compare(QLatin1String("c12a7328-f81f-11d2-ba4b-00a0c93ec93b"), Qt::CaseInsensitive) == 0
        || v.partType == QLatin1String("0xef");

    if (ifaces.contains(kFilesystem)) {
        v.hasFilesystem = true;
        v.mountPoints = byteStringList(ifaces.value(kFilesystem).value(QStringLiteral("MountPoints")));
    }
    // Configuration is a(sa{sv}); keep the fstab entry so it can be removed again verbatim.
    const QVariant config = block.value(QStringLiteral("Configuration"));
    if (config.canConvert<QDBusArgument>()) {
        const QDBusArgument arg = config.value<QDBusArgument>();
        arg.beginArray();
        while (!arg.atEnd()) {
            QString type;
            QVariantMap details;
            arg.beginStructure();
            arg >> type >> details;
            arg.endStructure();
            if (type == QLatin1String("fstab"))
                v.fstab = details;
        }
        arg.endArray();
    }
    if (ifaces.contains(kSwapspace))
        v.swapActive = ifaces.value(kSwapspace).value(QStringLiteral("Active")).toBool();
    v.encrypted = ifaces.contains(kEncrypted);
    return v;
}

} // namespace

UDisks::UDisks(QObject *parent)
    : QObject(parent)
{
    // coalesce signal bursts (hotplug, mount)
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(250);
    connect(&m_debounce, &QTimer::timeout, this, &UDisks::refresh);

    QDBusConnection bus = QDBusConnection::systemBus();
    const QString objectManager = QStringLiteral("org.freedesktop.DBus.ObjectManager");
    bus.connect(kService, kRoot, objectManager, QStringLiteral("InterfacesAdded"),
                this, SLOT(onDBusSignal(QDBusMessage)));
    bus.connect(kService, kRoot, objectManager, QStringLiteral("InterfacesRemoved"),
                this, SLOT(onDBusSignal(QDBusMessage)));
    bus.connect(kService, QString(), QStringLiteral("org.freedesktop.DBus.Properties"),
                QStringLiteral("PropertiesChanged"), this, SLOT(onDBusSignal(QDBusMessage)));
    // UDisks restarted (an update, or a setting changed): everything is read again from the new one.
    auto *watcher = new QDBusServiceWatcher(kService, bus, QDBusServiceWatcher::WatchForRegistration, this);
    connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, [this] { m_debounce.start(); });

    detectFilesystems();
    refresh();
}

void UDisks::onDBusSignal(const QDBusMessage &)
{
    m_debounce.start();
}

void UDisks::refresh()
{
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kRoot,
        QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects"));
    const QDBusMessage reply = QDBusConnection::systemBus().call(call, QDBus::Block, 5000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        m_error = reply.errorMessage().isEmpty() ? tr("No reply from UDisks2") : reply.errorMessage();
        m_disks.clear();
        emit changed();
        return;
    }
    m_error.clear();

    ObjectMap objects;
    reply.arguments().constFirst().value<QDBusArgument>() >> objects;

    QMap<QString, InterfaceMap> drives;
    m_jobs.clear();
    for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
        if (it->contains(kDrive))
            drives.insert(it.key().path(), it.value());
        if (it->contains(kJob)) {
            const QVariantMap j = it->value(kJob);
            Job job;
            job.path = it.key().path();
            job.operation = j.value(QStringLiteral("Operation")).toString();
            job.cancelable = j.value(QStringLiteral("Cancelable")).toBool();
            job.bytes = j.value(QStringLiteral("Bytes")).toULongLong();
            job.started = j.value(QStringLiteral("StartTime")).toULongLong();
            job.expectedEnd = j.value(QStringLiteral("ExpectedEndTime")).toULongLong();
            job.progress = j.value(QStringLiteral("Progress")).toDouble();
            job.progressValid = j.value(QStringLiteral("ProgressValid")).toBool();
            job.rate = j.value(QStringLiteral("Rate")).toULongLong();
            const QVariant objs = j.value(QStringLiteral("Objects"));
            if (objs.canConvert<QDBusArgument>()) {
                const QDBusArgument arg = objs.value<QDBusArgument>();
                arg.beginArray();
                while (!arg.atEnd()) {
                    QDBusObjectPath o;
                    arg >> o;
                    job.objects << o.path();
                }
                arg.endArray();
            }
            m_jobs.push_back(job);
        }
    }
    m_jobs += m_testJobs;

    // RAID arrays, and the block device each one runs as.
    QMap<QString, QVariantMap> arrays;
    QMap<QString, QString> arrayDevice;
    for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
        if (it->contains(kMDRaid))
            arrays.insert(it.key().path(), it->value(kMDRaid));
        const QString runsAs = objectPath(it->value(kBlock).value(QStringLiteral("MDRaid")));
        if (it->contains(kBlock) && !runsAs.isEmpty() && runsAs != QLatin1String("/")) {
            // The same name the array itself goes by (its preferred device, like /dev/md/data).
            QString device = byteString(it->value(kBlock).value(QStringLiteral("PreferredDevice")));
            if (device.isEmpty())
                device = byteString(it->value(kBlock).value(QStringLiteral("Device")));
            arrayDevice.insert(runsAs, device);
        }
    }
    // Members: the array lists the ones it runs on (ActiveDevices); udev's MDRaidMember on the
    // member itself can lag behind after the array is made, so it's only the fallback.
    QMap<QString, QString> activeMember; // member block -> array
    for (auto it = arrays.cbegin(); it != arrays.cend(); ++it) {
        const QVariant devices = it->value(QStringLiteral("ActiveDevices"));
        if (!devices.canConvert<QDBusArgument>())
            continue;
        const QDBusArgument arg = devices.value<QDBusArgument>();
        arg.beginArray();
        while (!arg.atEnd()) {
            QDBusObjectPath block;
            int slot = 0;
            QStringList state;
            qulonglong errors = 0;
            QVariantMap expansion;
            arg.beginStructure();
            arg >> block >> slot >> state >> errors >> expansion;
            arg.endStructure();
            activeMember.insert(block.path(), it.key());
        }
        arg.endArray();
    }
    auto memberOf = [&objects, &arrayDevice, &activeMember](const QString &blockPath) {
        QString array = activeMember.value(blockPath);
        if (array.isEmpty())
            array = objectPath(objects.value(QDBusObjectPath(blockPath.isEmpty() ? QStringLiteral("/") : blockPath)).value(kBlock).value(QStringLiteral("MDRaidMember")));
        if (array.isEmpty() || array == QLatin1String("/"))
            return QString();
        return shortDevice(arrayDevice.value(array, QStringLiteral("md")));
    };

    // whole disks
    QMap<QString, Disk> disks;
    for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
        const InterfaceMap &ifaces = it.value();
        if (!ifaces.contains(kBlock) || ifaces.contains(kPartition))
            continue;
        const QVariantMap block = ifaces.value(kBlock);
        if (objectPath(block.value(QStringLiteral("CryptoBackingDevice"))) != QLatin1String("/"))
            continue;

        Disk d;
        d.blockPath = it.key().path();
        d.drivePath = objectPath(block.value(QStringLiteral("Drive")));
        d.isLoop = ifaces.contains(kLoop);
        if (d.isLoop)
            d.backingFile = byteString(ifaces.value(kLoop).value(QStringLiteral("BackingFile")));
        // skip zram/dm (no drive) and unused loop devices; RAID arrays are listed like drives
        const QString raid = objectPath(block.value(QStringLiteral("MDRaid")));
        d.isRaid = arrays.contains(raid);
        // A Windows dynamic volume ldmtool opened: a drive of its own, like an array.
        const QString preferred = byteString(block.value(QStringLiteral("PreferredDevice")));
        d.isDynamicVolume = preferred.startsWith(QLatin1String("/dev/mapper/ldm_vol_"));
        if (d.isLoop ? d.backingFile.isEmpty() : (d.drivePath == QLatin1String("/") && !d.isRaid && !d.isDynamicVolume))
            continue;
        // A live system's own image (DiskAnvil Live, any Debian live): part of the system, not a disk.
        if (d.isLoop && d.backingFile.startsWith(QLatin1String("/run/live/")))
            continue;
        if (d.isRaid) {
            const QVariantMap a = arrays.value(raid);
            d.raidPath = raid;
            d.raidLevel = a.value(QStringLiteral("Level")).toString();
            d.raidDevices = int(a.value(QStringLiteral("NumDevices")).toUInt());
            d.raidDegraded = int(a.value(QStringLiteral("Degraded")).toUInt());
            d.raidRunning = a.value(QStringLiteral("Running"), true).toBool();
            d.raidSync = a.value(QStringLiteral("SyncAction")).toString();
            d.raidSyncDone = std::clamp(a.value(QStringLiteral("SyncCompleted")).toDouble(), 0.0, 1.0);
            d.raidSyncRate = a.value(QStringLiteral("SyncRate")).toULongLong();
            d.raidSyncLeftUs = a.value(QStringLiteral("SyncRemainingTime")).toULongLong();
            d.health.key = health::keyFor(QStringLiteral("raid-") + a.value(QStringLiteral("UUID")).toString(), raid);
        }
        d.size = block.value(QStringLiteral("Size")).toULongLong();
        if (d.size == 0)
            continue; // no media

        d.device = byteString(block.value(QStringLiteral("PreferredDevice")));
        if (d.device.isEmpty())
            d.device = byteString(block.value(QStringLiteral("Device")));

        const InterfaceMap driveIfaces = drives.value(d.drivePath);
        const QVariantMap drive = driveIfaces.value(kDrive);
        const QString vendor = drive.value(QStringLiteral("Vendor")).toString().trimmed();
        d.model = drive.value(QStringLiteral("Model")).toString().trimmed();
        if (!vendor.isEmpty() && !d.model.startsWith(vendor))
            d.model = vendor + QLatin1Char(' ') + d.model;
        if (d.isLoop)
            d.model = d.backingFile.section(QLatin1Char('/'), -1);
        if (d.isRaid) // "nas:0" is host:name; the name is what people chose
            d.model = tr("%1 array %2").arg(raidLevelName(d.raidLevel), arrays.value(raid).value(QStringLiteral("Name")).toString().section(QLatin1Char(':'), -1));
        if (d.isDynamicVolume) // ldm_vol_<disk group>-Dg0_Volume1
            d.model = tr("Windows dynamic volume %1").arg(preferred.section(QLatin1Char('_'), -1));
        d.model = cleanName(d.model);
        d.serial = cleanName(drive.value(QStringLiteral("Serial")).toString());
        d.revision = cleanName(drive.value(QStringLiteral("Revision")).toString());
        d.bus = drive.value(QStringLiteral("ConnectionBus")).toString();
        d.removable = drive.value(QStringLiteral("Removable")).toBool();
        // A card in a slot is mmcblk; in a USB reader, the reader says what it takes and what's in it.
        const QString media = drive.value(QStringLiteral("Media")).toString();
        d.sdCard = d.device.startsWith(QLatin1String("/dev/mmcblk")) || d.bus == QLatin1String("sdio") || media.startsWith(QLatin1String("flash_sd"))
                || media == QLatin1String("flash_mmc");
        d.rotationRate = drive.value(QStringLiteral("RotationRate"), -1).toInt();
        d.canPowerOff = drive.value(QStringLiteral("CanPowerOff")).toBool();
        d.readOnly = block.value(QStringLiteral("ReadOnly")).toBool();
        const QVariantMap ata = driveIfaces.value(kAta);
        d.ataEraseMinutes = ata.value(QStringLiteral("SecurityEraseUnitMinutes")).toInt();
        d.ataEnhancedEraseMinutes = ata.value(QStringLiteral("SecurityEnhancedEraseUnitMinutes")).toInt();
        d.ataFrozen = ata.value(QStringLiteral("SecurityFrozen")).toBool();
        d.ataPm = ata.value(QStringLiteral("PmSupported")).toBool();
        d.ataApm = ata.value(QStringLiteral("ApmSupported")).toBool();
        d.ataWriteCache = ata.value(QStringLiteral("WriteCacheSupported")).toBool();
        d.ataWriteCacheEnabled = ata.value(QStringLiteral("WriteCacheEnabled")).toBool();
        d.ataAam = ata.value(QStringLiteral("AamSupported")).toBool();
        d.ataAamRecommended = ata.value(QStringLiteral("AamVendorRecommendedValue")).toInt();
        d.ataLookahead = ata.value(QStringLiteral("ReadLookaheadSupported")).toBool();
        d.ataLookaheadEnabled = ata.value(QStringLiteral("ReadLookaheadEnabled")).toBool();
        const QVariantMap nvme = driveIfaces.value(kNvme);
        d.nvmeSanitizeStatus = nvme.value(QStringLiteral("SanitizeStatus")).toString();
        d.nvmeSanitizePercent = nvme.value(QStringLiteral("SanitizePercentRemaining"), -1).toInt();
        d.nvmeUnallocated = nvme.value(QStringLiteral("UnallocatedCapacity")).toULongLong();
        {
            const QVariant config = drive.value(QStringLiteral("Configuration"));
            if (config.canConvert<QDBusArgument>())
                config.value<QDBusArgument>() >> d.driveConfiguration;
            else
                d.driveConfiguration = config.toMap();
        }
        d.nvmeNamespace = ifaces.contains(kNvmeNamespace);
        if (d.nvmeNamespace) {
            // LBAFormats a(qqy) and FormattedLBASize (qqy): data size, metadata size, performance.
            const QVariantMap ns = ifaces.value(kNvmeNamespace);
            const QVariant formats = ns.value(QStringLiteral("LBAFormats"));
            if (formats.canConvert<QDBusArgument>()) {
                const QDBusArgument arg = formats.value<QDBusArgument>();
                arg.beginArray();
                while (!arg.atEnd()) {
                    quint16 size = 0, metadata = 0;
                    uchar performance = 0;
                    arg.beginStructure();
                    arg >> size >> metadata >> performance;
                    arg.endStructure();
                    if (metadata == 0 && size >= 512 && !d.nvmeSectorSizes.contains(size))
                        d.nvmeSectorSizes << size;
                }
                arg.endArray();
            }
            const QVariant formatted = ns.value(QStringLiteral("FormattedLBASize"));
            if (formatted.canConvert<QDBusArgument>()) {
                const QDBusArgument arg = formatted.value<QDBusArgument>();
                quint16 size = 0, metadata = 0;
                uchar performance = 0;
                arg.beginStructure();
                arg >> size >> metadata >> performance;
                arg.endStructure();
                d.nvmeSectorSize = size;
            }
        }
        {
            QFile discard(QStringLiteral("/sys/class/block/%1/queue/discard_max_bytes").arg(shortDevice(d.device)));
            d.discard = discard.open(QIODevice::ReadOnly) && discard.readAll().trimmed().toULongLong() > 0;
            QFile sector(QStringLiteral("/sys/class/block/%1/queue/logical_block_size").arg(shortDevice(d.device)));
            if (sector.open(QIODevice::ReadOnly))
                d.sectorSize = std::max(512, sector.readAll().trimmed().toInt());
        }
        if (!d.isLoop)
            d.health = readHealth(d.drivePath, drive, driveIfaces.value(kAta), driveIfaces.value(kNvme));
        health::noteSelftestStatus(d.health.key, d.health.selftestStatus);

        if (ifaces.contains(kPartitionTable)) {
            d.tableType = ifaces.value(kPartitionTable).value(QStringLiteral("Type")).toString();
            d.tableId = recover::diskId(d.device);
        }
        else if (!block.value(QStringLiteral("IdUsage")).toString().isEmpty())
            d.volumes.push_back(makeVolume(d.blockPath, ifaces)); // fs on the bare disk

        disks.insert(d.blockPath, d);
    }

    for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
        if (!it->contains(kPartition))
            continue;
        const QString table = objectPath(it->value(kPartition).value(QStringLiteral("Table")));
        auto disk = disks.find(table);
        if (disk != disks.end())
            disk->volumes.push_back(makeVolume(it.key().path(), it.value()));
    }

    // LVM volume groups (UDisks' lvm2 module): listed like drives, their logical volumes one
    // after the other and their free space at the end.
    QMap<QString, QString> groupName; // VolumeGroup object -> name
    bool lvmMembers = false;
    for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
        lvmMembers = lvmMembers || it->value(kBlock).value(QStringLiteral("IdType")).toString() == QLatin1String("LVM2_member");
        if (!it->contains(kVolumeGroup))
            continue;
        const QVariantMap vg = it->value(kVolumeGroup);
        Disk d;
        d.isLvm = true;
        d.blockPath = it.key().path();
        d.drivePath = QStringLiteral("/");
        const QString name = cleanName(vg.value(QStringLiteral("Name")).toString());
        groupName.insert(d.blockPath, name);
        d.device = QStringLiteral("/dev/") + name;
        d.model = tr("LVM volume group %1").arg(name);
        d.size = vg.value(QStringLiteral("Size")).toULongLong();
        d.lvmMissing = int(vg.value(QStringLiteral("MissingPhysicalVolumes")).toStringList().size());
        d.health.key = health::keyFor(QStringLiteral("lvm-") + vg.value(QStringLiteral("UUID")).toString(), d.blockPath);
        QVector<QPair<QString, QVariantMap>> lvs;
        for (auto lv = objects.cbegin(); lv != objects.cend(); ++lv) {
            if (lv->contains(kLogicalVolume) && objectPath(lv->value(kLogicalVolume).value(QStringLiteral("VolumeGroup"))) == d.blockPath)
                lvs.push_back({lv.key().path(), lv->value(kLogicalVolume)});
        }
        std::sort(lvs.begin(), lvs.end(), [](const auto &a, const auto &b) {
            return a.second.value(QStringLiteral("Name")).toString() < b.second.value(QStringLiteral("Name")).toString();
        });
        quint64 at = 0;
        int number = 0;
        for (const auto &[path, lv] : std::as_const(lvs)) {
            const QString block = objectPath(lv.value(QStringLiteral("BlockDevice")));
            const QDBusObjectPath blockObject(block.isEmpty() ? QStringLiteral("/") : block);
            Volume v = block != QLatin1String("/") && objects.contains(blockObject) ? makeVolume(block, objects.value(blockObject)) : Volume();
            const QString lvName = cleanName(lv.value(QStringLiteral("Name")).toString());
            if (v.objectPath.isEmpty()) {
                v.objectPath = path;
                v.device = d.device + QLatin1Char('/') + lvName;
            }
            v.isLv = true;
            v.lvPath = path;
            v.lvActive = lv.value(QStringLiteral("Active")).toBool() && block != QLatin1String("/");
            v.partName = lvName; // shown when the file system has no label
            v.number = ++number;
            v.size = lv.value(QStringLiteral("Size")).toULongLong();
            v.offset = at;
            at += v.size;
            d.volumes.push_back(v);
        }
        d.size = std::max(d.size, at);
        if (d.lvmMissing > 0) {
            health::add(d.health, {HealthReason::Level::Warning, QStringLiteral("lvm-missing"), d.lvmMissing,
                                   tr("The volume group %1 is missing %n of its drives. What was on them can't be read.", nullptr, d.lvmMissing).arg(name)});
        }
        disks.insert(d.blockPath, d);
    }
    // LVM members but no module to say more: ask UDisks to load it (once; no password needed).
    const bool lvmLoaded = objects.value(QDBusObjectPath(kManagerPath)).contains(kManagerLvm);
    if (lvmMembers && !lvmLoaded && !m_lvmAsked) {
        m_lvmAsked = true;
        QDBusMessage enable = QDBusMessage::createMethodCall(kService, kManagerPath, kManager, QStringLiteral("EnableModule"));
        enable << QStringLiteral("lvm2") << true;
        enable.setInteractiveAuthorizationAllowed(false);
        auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(enable, 30000), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
            watcher->deleteLater();
            if (!watcher->isError())
                QTimer::singleShot(500, this, &UDisks::refresh);
        });
    }
    auto pvOf = [&objects, &groupName](const QString &blockPath) {
        const QString group = objectPath(objects.value(QDBusObjectPath(blockPath.isEmpty() ? QStringLiteral("/") : blockPath)).value(kPhysicalVolume).value(QStringLiteral("VolumeGroup")));
        return groupName.value(group);
    };

    // unlocked LUKS: where the cleartext device is mounted
    for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
        const QString backing = objectPath(it->value(kBlock).value(QStringLiteral("CryptoBackingDevice")));
        if (!it->contains(kBlock) || backing.isEmpty() || backing == QLatin1String("/"))
            continue;
        const QStringList mounts = byteStringList(it->value(kFilesystem).value(QStringLiteral("MountPoints")));
        for (Disk &d : disks) {
            for (Volume &v : d.volumes) {
                if (v.objectPath == backing) {
                    v.cleartextPath = it.key().path();
                    v.cleartextDevice = byteString(it->value(kBlock).value(QStringLiteral("Device")));
                    v.cleartextMountPoints = mounts;
                    v.cleartextHasFilesystem = it->contains(kFilesystem);
                    v.cleartextFsType = it->value(kBlock).value(QStringLiteral("IdType")).toString();
                }
            }
        }
    }

    m_disks.clear();
    for (Disk &d : disks) {
        std::sort(d.volumes.begin(), d.volumes.end(),
                  [](const Volume &a, const Volume &b) { return a.offset < b.offset; });
        for (Volume &v : d.volumes) {
            for (const QString &mp : v.mountPoints + v.cleartextMountPoints) {
                if (!isUserMount(mp)) {
                    v.isSystem = true;
                    // Running from DiskAnvil Live: live-boot mounts the stick there.
                    if (QFileInfo::exists(mp + QStringLiteral("/.disk/diskanvil-live")) || QFileInfo::exists(mp + QStringLiteral("/.disk/diskforge-live"))
                        || QFileInfo::exists(mp + QStringLiteral("/.disk/bluespark")) || QFileInfo::exists(mp + QStringLiteral("/.disk/diskforge-rescue")))
                        d.systemReason = tr("DiskAnvil Live is running from it");
                    else if (d.systemReason.isEmpty())
                        d.systemReason = tr("Holds %1").arg(mp);
                }
            }
            if (v.swapActive) {
                v.isSystem = true;
                if (d.systemReason.isEmpty())
                    d.systemReason = tr("Swap in use");
            }
            d.isSystem = d.isSystem || v.isSystem;
            if (v.label == QLatin1String("VTOYEFI"))
                d.isVentoy = true;

            const QString mp = v.mountPoints.value(0, v.cleartextMountPoints.value(0));
            if (!mp.isEmpty()) {
                const QStorageInfo info(mp);
                if (info.isValid()) {
                    v.fsTotal = info.bytesTotal();
                    v.fsFree = info.bytesAvailable();
                }
            }
        }
        // Btrfs counts the errors it has seen; any at all go into the drive's health.
        for (const Volume &v : std::as_const(d.volumes)) {
            if (v.effectiveFsType() != QLatin1String("btrfs") || v.mounts().isEmpty())
                continue;
            const QString device = v.encrypted ? v.cleartextDevice : v.device;
            const btrfscheck::Counts counts = btrfscheck::read(kernelName(device));
            if (counts.total() > 0) {
                health::add(d.health, {HealthReason::Level::Warning, QStringLiteral("btrfs-") + QFileInfo(v.device).fileName(), counts.total(),
                                       tr("Btrfs on %1 has seen errors. %2 A scrub (in Disk Health) checks everything.")
                                           .arg(QFileInfo(v.device).fileName(), btrfscheck::describe(counts))});
            }
        }
        // RAID and LVM: members say what they're part of; a degraded array goes into its health.
        d.raidMemberOf = memberOf(d.blockPath);
        if (!d.isLvm)
            d.lvmMemberOf = pvOf(d.blockPath);
        for (Volume &v : d.volumes) {
            v.raidMemberOf = memberOf(v.objectPath);
            if (!v.isLv) {
                v.lvmMemberOf = pvOf(v.encrypted ? v.cleartextPath : v.objectPath);
                v.lvmUnknown = v.lvmMemberOf.isEmpty() && (v.fsType == QLatin1String("LVM2_member") || v.cleartextFsType == QLatin1String("LVM2_member"));
            }
        }
        if (d.isRaid && d.raidDegraded > 0) {
            health::add(d.health, {HealthReason::Level::Warning, QStringLiteral("raid-degraded"), d.raidDegraded,
                                   tr("%1 is missing %2 of its %3 drives. It still works, but losing another one can lose "
                                      "everything on it: replace the missing drive.")
                                       .arg(shortDevice(d.device))
                                       .arg(d.raidDegraded)
                                       .arg(d.raidDevices)});
        }
        if (m_testHealth.contains(d.blockPath))
            d.health = m_testHealth.value(d.blockPath);
        m_disks.push_back(d);
    }

    // internal, then removable, then images
    QCollator collator;
    collator.setNumericMode(true);
    auto rank = [](const Disk &d) { return d.isLoop ? 2 : (d.removable || d.bus == QLatin1String("usb")) ? 1 : 0; };
    std::sort(m_disks.begin(), m_disks.end(), [&](const Disk &a, const Disk &b) {
        if (rank(a) != rank(b))
            return rank(a) < rank(b);
        return collator.compare(a.device, b.device) < 0;
    });

    emit changed();
}

QVariantMap UDisks::options(QVariantMap extra) const
{
    if (!m_interactive)
        extra.insert(QStringLiteral("auth.no_user_interaction"), true);
    return extra;
}

void UDisks::callThen(const QString &path, const QString &interface, const QString &method, const QVariantList &args,
                      const QString &failure, const std::function<void(const QDBusMessage &)> &next,
                      const std::function<void()> &failed, const Explain &explain,
                      const std::function<bool(const QString &error)> &takeOver)
{
    QDBusMessage message = QDBusMessage::createMethodCall(kService, path, interface, method);
    message.setArguments(args);
    message.setInteractiveAuthorizationAllowed(m_interactive);
    // The arguments stay out of the log: passphrases travel in them.
    qCInfo(lcOps).noquote() << method << path;

    ++m_pending;
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, kNoTimeout), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, method, path, failure, next, failed, explain, takeOver](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        --m_pending;
        if (w->isError())
            qCInfo(lcOps).noquote() << method << path << "failed:" << w->error().name() << w->error().message();
        else
            qCInfo(lcOps).noquote() << method << path << "done";
        if (w->isError() && method == QLatin1String("Unmount") && w->error().name() == QLatin1String("org.freedesktop.UDisks2.Error.NotMounted")) {
            // Already unmounted (what's known here can lag a moment behind): that's what was wanted.
            next(w->reply());
        } else if (w->isError() && w->error().name() == QLatin1String("org.freedesktop.UDisks2.Error.Cancelled")) {
            // Stopped on purpose (Stop, or another program): not an error.
            m_stopped = true;
            emit operationFinished(false, m_stopMessages.isEmpty() ? tr("Stopped.") : m_stopMessages.takeFirst());
            m_stopped = false;
            if (failed)
                failed();
        } else if (w->isError() && takeOver && takeOver(w->error().message())) {
            // Handled by the caller (tried again, or another way).
        } else if (w->isError()) {
            // A drive that won't unmount because something has it open: Who's Using It? can say what.
            if (method == QLatin1String("Unmount")
                && (w->error().name() == QLatin1String("org.freedesktop.UDisks2.Error.DeviceBusy") || w->error().message().contains(QLatin1String("busy"))))
                emit busy(path);
            QString text = failure + QStringLiteral(": ") + w->error().message();
            if (const QString more = explain ? explain(w->error().message()) : QString(); !more.isEmpty())
                text += QStringLiteral("\n\n") + more;
            emit operationFinished(false, text);
            if (failed)
                failed();
        } else {
            next(w->reply());
        }
    });
}

void UDisks::formatBlock(const QString &path, const QString &device, const QVariantList &args, const QString &failure,
                         const std::function<void(const QDBusMessage &)> &next, const std::function<void()> &failed,
                         const std::function<void(const QString &error)> &lastResort, int attempt)
{
    const QVector<int> waits = syncWaits(device);
    const auto onError = [=, this](const QString &error) {
        if (!isSyncTimeout(error))
            return false;
        if (attempt < waits.size()) {
            qCInfo(lcOps).noquote() << "UDisks timed out waiting for" << device << "- settling" << waits[attempt] << "s and trying again";
            settleThen(device, waits[attempt], [=, this] { formatBlock(path, device, args, failure, next, failed, lastResort, attempt + 1); });
            return true;
        }
        if (!lastResort)
            return false;
        lastResort(error);
        return true;
    };
    const Explain explain = [device](const QString &) { return kernelLines(device); };
    if (m_failFormatsForTest > 0) {
        --m_failFormatsForTest;
        QTimer::singleShot(0, this, [=, this] {
            const QString error = QStringLiteral("Error synchronizing after initial wipe: Timed out waiting for object");
            if (onError(error))
                return;
            QString text = failure + QStringLiteral(": ") + error;
            if (const QString more = explain(error); !more.isEmpty())
                text += QStringLiteral("\n\n") + more;
            emit operationFinished(false, text);
            if (failed)
                failed();
        });
        return;
    }
    callThen(path, kBlock, QStringLiteral("Format"), args, failure, next, failed, explain, onError);
}

void UDisks::settleThen(const QString &device, int seconds, const std::function<void()> &then)
{
    // The kernel told to read the drive's table again (DiskAnvil Live, or as root: UDisks does
    // the same itself, but a busy eMMC can miss it), then udev let finish what it's doing.
    QStringList steps;
    if (::geteuid() == 0 || rescue::runningInRescue())
        steps << QStringLiteral("rereadpt");
    steps << QStringLiteral("settle");
    auto run = std::make_shared<std::function<void()>>();
    *run = [this, device, seconds, then, steps, weak = std::weak_ptr(run)]() mutable {
        const auto self = weak.lock(); // kept alive by whoever called it
        if (steps.isEmpty()) {
            refresh();
            QTimer::singleShot(2000, this, then);
            return;
        }
        const QString step = steps.takeFirst();
        QStringList command = step == QLatin1String("settle")
            ? host::command({QStringLiteral("udevadm"), QStringLiteral("settle"), QStringLiteral("--timeout=%1").arg(seconds)})
            : QStringList{QStringLiteral("blockdev"), QStringLiteral("--rereadpt"), device};
        if (step == QLatin1String("rereadpt") && ::geteuid() != 0)
            command = QStringList{QStringLiteral("sudo"), QStringLiteral("-n")} + command;
        auto *p = new QProcess(this);
        connect(p, &QProcess::finished, this, [p, self] {
            p->deleteLater();
            (*self)();
        });
        connect(p, &QProcess::errorOccurred, this, [p, self](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart)
                return;
            p->deleteLater();
            (*self)();
        });
        p->start(command.first(), command.mid(1));
    };
    (*run)();
}

void UDisks::tableDirectly(const QString &path, const QString &device, const QString &tableType, const QString &failure,
                           const QString &udisksError)
{
    // sfdisk writes just the empty table, the way `parted mklabel` does. As root: DiskAnvil
    // Live's sudo, or the admin password on a PC.
    qCInfo(lcOps).noquote() << "UDisks couldn't make the table on" << device << "- writing it with sfdisk";
    QStringList command{QStringLiteral("sfdisk"), QStringLiteral("--quiet"), QStringLiteral("--wipe"), QStringLiteral("always"), device};
    if (::geteuid() != 0)
        command = rescue::runningInRescue() ? QStringList{QStringLiteral("sudo"), QStringLiteral("-n")} + command
                                            : host::command(QStringList{QStringLiteral("pkexec")} + command);
    ++m_pending;
    auto *p = new QProcess(this);
    p->setProcessChannelMode(QProcess::MergedChannels);
    const auto finish = [=, this](bool ok, const QString &said) {
        p->deleteLater();
        --m_pending;
        qCInfo(lcOps).noquote() << "sfdisk on" << device << (ok ? "done" : "failed:") << said;
        if (!ok) {
            QString text = failure + QStringLiteral(": ") + udisksError + QStringLiteral("\n\n")
                         + tr("Writing it directly didn't work either: %1").arg(said.isEmpty() ? tr("sfdisk failed") : said);
            if (const QString more = kernelLines(device); !more.isEmpty())
                text += QStringLiteral("\n\n") + more;
            emit operationFinished(false, text);
            return;
        }
        settleThen(device, 10, [=, this] {
            confirmTable(path, tableType, failure,
                         tr("%1 now has an empty %2 partition table (written directly: UDisks kept timing out on this drive)")
                             .arg(shortDevice(device), tableType == QLatin1String("gpt") ? QStringLiteral("GPT") : QStringLiteral("MBR")));
        });
    };
    connect(p, &QProcess::finished, this, [p, finish](int code, QProcess::ExitStatus status) {
        finish(status == QProcess::NormalExit && code == 0, QString::fromLocal8Bit(p->readAll()).trimmed().section(QLatin1Char('\n'), -2));
    });
    connect(p, &QProcess::errorOccurred, this, [p, finish](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            finish(false, p->errorString());
    });
    p->start(command.first(), command.mid(1));
    p->write(tableType == QLatin1String("gpt") ? "label: gpt\n" : "label: dos\n");
    p->closeWriteChannel();
}

void UDisks::confirmTable(const QString &path, const QString &tableType, const QString &failure, const QString &done, int tries)
{
    refresh();
    const Disk *d = diskByPath(path);
    if (d && d->tableType == tableType) {
        emit operationFinished(true, done);
        return;
    }
    if (tries >= 20) {
        emit operationFinished(false, failure + QStringLiteral(": ")
                                          + tr("it was written, but the drive still doesn't show a %1 partition table. Unplug it and plug it "
                                               "back in (or restart), then look again.")
                                                .arg(tableType == QLatin1String("gpt") ? QStringLiteral("GPT") : QStringLiteral("MBR")));
        return;
    }
    QTimer::singleShot(500, this, [=, this] { confirmTable(path, tableType, failure, done, tries + 1); });
}

void UDisks::call(const QString &path, const QString &interface, const QString &method, const QVariantList &args,
                  const SuccessText &success, const QString &failure, const Explain &explain)
{
    callThen(path, interface, method, args, failure, [this, success](const QDBusMessage &reply) {
        emit operationFinished(true, success(reply));
    }, {}, explain);
}

void UDisks::unmountThen(const QVector<Volume> &volumes, const QString &failure, const std::function<void()> &then,
                         bool lockEncrypted, const std::function<void()> &failed)
{
    // "tear-down" doesn't unmount the device itself, so do it first, one step at a time.
    struct Step { QString path, interface, method; };
    QVector<Step> steps;
    for (const Volume &v : volumes) {
        if (v.canMount() && !v.mounts().isEmpty())
            steps.push_back({v.filesystemPath(), kFilesystem, QStringLiteral("Unmount")});
        if (lockEncrypted && v.encrypted && !v.cleartextPath.isEmpty())
            steps.push_back({v.objectPath, kEncrypted, QStringLiteral("Lock")});
    }
    auto run = std::make_shared<std::function<void(int)>>();
    std::weak_ptr<std::function<void(int)>> weak = run;
    *run = [this, steps, failure, then, failed, weak](int i) {
        if (i == steps.size()) {
            then();
            return;
        }
        auto self = weak.lock();
        callThen(steps[i].path, steps[i].interface, steps[i].method, {options()}, failure,
                 [self, i](const QDBusMessage &) { (*self)(i + 1); }, failed);
    };
    (*run)(0);
}

void UDisks::mount(const Volume &volume, bool readOnly, bool readOnlyIfNeeded)
{
    const QString name = shortDevice(volume.device);
    if (volume.encrypted && volume.cleartextPath.isEmpty()) {
        emit operationFinished(false, tr("Couldn't mount %1: unlock it first").arg(name));
        return;
    }
    QVariantMap extra;
    if (readOnly)
        extra.insert(QStringLiteral("options"), QStringLiteral("ro"));
    // bcachefs: each Linux reads the versions of it up to its own, and it left Linux's own kernel
    // in 6.17 (newer ones need the bcachefs-dkms module). Said when it doesn't mount.
    QString failure = tr("Couldn't mount %1").arg(name);
    if (volume.effectiveFsType() == QLatin1String("bcachefs"))
        failure = rescue::runningInRescue()
                      ? tr("Couldn't mount %1 (bcachefs: DiskAnvil Live's Linux 6.12 reads bcachefs file systems made by bcachefs up to about "
                           "version 1.13; a newer one, or one spread over several drives, opens on the PC that made it)").arg(name)
                      : tr("Couldn't mount %1 (bcachefs: Linux reads it only with its bcachefs module, which left Linux itself in version "
                           "6.17, so newer ones need bcachefs-dkms installed; one spread over several drives can't be opened here)").arg(name);
    // NTFS that Windows left hibernated (Fast Startup, on by default) or didn't close properly (a
    // crash, the power): Linux only reads it. Opened read-only then, and said why, instead of
    // Linux's "wrong fs type, bad option, bad superblock".
    if (!readOnly && readOnlyIfNeeded && volume.effectiveFsType() == QLatin1String("ntfs")) {
        QDBusMessage message = QDBusMessage::createMethodCall(kService, volume.filesystemPath(), kFilesystem, QStringLiteral("Mount"));
        message.setArguments({options(extra)});
        message.setInteractiveAuthorizationAllowed(m_interactive);
        qCInfo(lcOps).noquote() << "Mount" << volume.filesystemPath();
        ++m_pending;
        auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, kNoTimeout), this);
        const Volume v = volume;
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, v, name, failure](QDBusPendingCallWatcher *w) {
            w->deleteLater();
            --m_pending;
            if (!w->isError()) {
                qCInfo(lcOps).noquote() << "Mount" << v.filesystemPath() << "done";
                emit operationFinished(true, tr("Mounted %1 at %2").arg(name, QDBusReply<QString>(*w).value()));
                return;
            }
            const QString error = w->error().message();
            qCInfo(lcOps).noquote() << "Mount" << v.filesystemPath() << "failed:" << w->error().name() << error;
            if (!error.contains(QLatin1String("wrong fs type"))) {
                emit operationFinished(false, failure + QStringLiteral(": ") + error);
                return;
            }
            QVariantMap readOnlyOptions;
            readOnlyOptions.insert(QStringLiteral("options"), QStringLiteral("ro"));
            call(v.filesystemPath(), kFilesystem, QStringLiteral("Mount"), {options(readOnlyOptions)},
                 [name](const QDBusMessage &reply) {
                     return tr("Mounted %1 read-only at %2. Windows didn't shut down fully: it's hibernated (Fast Startup, which "
                               "Windows has on by default) or it crashed or lost power. Files can be copied off it; to change them, "
                               "start Windows and shut it down while holding Shift, then mount it again.")
                         .arg(name, reply.arguments().value(0).toString());
                 },
                 failure);
        });
        return;
    }
    call(volume.filesystemPath(), kFilesystem, QStringLiteral("Mount"), {options(extra)},
         [name](const QDBusMessage &reply) { return tr("Mounted %1 at %2").arg(name, reply.arguments().value(0).toString()); }, failure);
}

void UDisks::unmount(const Volume &volume, bool besideSystem)
{
    const QString name = shortDevice(volume.device);
    if (!(besideSystem && !volume.isSystem) && refuseSystem(diskOf(volume), tr("Couldn't unmount %1").arg(name)))
        return;
    call(volume.filesystemPath(), kFilesystem, QStringLiteral("Unmount"), {options()},
         [name](const QDBusMessage &) { return tr("Unmounted %1").arg(name); },
         tr("Couldn't unmount %1").arg(name));
}

void UDisks::setLabel(const Volume &volume, const QString &label)
{
    const QString name = shortDevice(volume.device);
    if (refuseSystem(diskOf(volume), tr("Couldn't rename %1").arg(name)))
        return;
    call(volume.filesystemPath(), kFilesystem, QStringLiteral("SetLabel"), {label, options()},
         [name, label](const QDBusMessage &) { return tr("Renamed %1 to \"%2\"").arg(name, label); },
         tr("Couldn't rename %1").arg(name));
}

void UDisks::setPartitionTypeAndFlags(const Volume &volume, const QString &type, bool setFlags, quint64 flags)
{
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't change the type and flags of %1").arg(name);
    if (refuseSystem(diskOf(volume), failure))
        return;
    const QString path = volume.objectPath;
    auto flagsStep = [this, path, name, failure, flags] {
        call(path, kPartition, QStringLiteral("SetFlags"), {QVariant::fromValue(qulonglong(flags)), options()},
             [name](const QDBusMessage &) { return tr("Changed the type and flags of %1").arg(name); }, failure);
    };
    if (type.isEmpty()) {
        if (setFlags)
            flagsStep();
        return;
    }
    callThen(path, kPartition, QStringLiteral("SetType"), {type, options()}, failure, [this, name, setFlags, flagsStep](const QDBusMessage &) {
        if (setFlags)
            flagsStep();
        else
            emit operationFinished(true, tr("Changed the type of %1").arg(name));
    });
}

void UDisks::setUuid(const Volume &volume, const QString &uuid)
{
    const QString name = shortDevice(volume.device);
    if (refuseSystem(diskOf(volume), tr("Couldn't change the ID of %1").arg(name)))
        return;
    // A swap area has its own interface for it (UDisks 2.10 and later).
    const bool swap = volume.fsType == QLatin1String("swap");
    call(swap ? volume.objectPath : volume.filesystemPath(), swap ? kSwapspace : kFilesystem, QStringLiteral("SetUUID"), {uuid, options()},
         [name](const QDBusMessage &) { return tr("Gave %1 a new ID").arg(name); },
         tr("Couldn't change the ID of %1").arg(name));
}

namespace {

QVariantMap formatOptions(const QString &fsType, const QString &label, const QString &passphrase = {})
{
    QVariantMap o;
    if (!passphrase.isEmpty()) {
        o.insert(QStringLiteral("encrypt.passphrase"), passphrase);
        o.insert(QStringLiteral("encrypt.type"), QStringLiteral("luks2"));
    }
    if (!label.isEmpty())
        o.insert(QStringLiteral("label"), label);
    // root dir owned by the user instead of root
    if (fsType == QLatin1String("ext4") || fsType == QLatin1String("btrfs") || fsType == QLatin1String("xfs"))
        o.insert(QStringLiteral("take-ownership"), true);
    return o;
}

QString partitionTypeFor(const QString &tableType, const QString &fsType, bool encrypted = false)
{
    if (encrypted)
        return tableType == QLatin1String("gpt") ? QStringLiteral("ca7d7ccb-63ed-4c53-861c-1742536059cc") : QStringLiteral("0x83");
    const bool windows = fsType == QLatin1String("vfat") || fsType == QLatin1String("exfat") || fsType == QLatin1String("ntfs");
    if (tableType == QLatin1String("gpt"))
        return windows ? QStringLiteral("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7") : QStringLiteral("0fc63daf-8483-4772-8e79-3d69d8477de4");
    if (fsType == QLatin1String("vfat"))
        return QStringLiteral("0x0c");
    return windows ? QStringLiteral("0x07") : QStringLiteral("0x83");
}

} // namespace

void UDisks::format(const Volume &volume, const QString &fsType, const QString &label, const QString &passphrase)
{
    const QString name = shortDevice(volume.device);
    if (refuseSystem(diskOf(volume), tr("Couldn't format %1").arg(name)))
        return;
    QVariantMap o = formatOptions(fsType, label, passphrase);
    o.insert(QStringLiteral("tear-down"), true);
    o.insert(QStringLiteral("update-partition-type"), true);
    const QString failure = tr("Couldn't format %1").arg(name);
    const QString path = volume.objectPath;
    const bool encrypted = !passphrase.isEmpty();
    const QString fsName = filesystem(fsType) ? filesystem(fsType)->name : fsType; // exFAT, not exfat
    unmountThen({volume}, failure, [this, path, fsType, fsName, o, name, failure, encrypted] {
        call(path, kBlock, QStringLiteral("Format"), {fsType, options(o)},
             [name, fsName, encrypted](const QDBusMessage &) {
                 return encrypted ? tr("Formatted %1 as encrypted %2").arg(name, fsName) : tr("Formatted %1 as %2").arg(name, fsName);
             },
             failure);
    }, true);
}

void UDisks::deletePartition(const Volume &volume, bool besideSystem)
{
    const QString name = shortDevice(volume.device);
    if (!(besideSystem && !volume.isSystem) && refuseSystem(diskOf(volume), tr("Couldn't delete %1").arg(name)))
        return;
    const QString failure = tr("Couldn't delete %1").arg(name);
    const QString path = volume.objectPath;
    unmountThen({volume}, failure, [this, path, name, failure] {
        call(path, kPartition, QStringLiteral("Delete"), {options({{QStringLiteral("tear-down"), true}})},
             [name](const QDBusMessage &) { return tr("Deleted %1").arg(name); }, failure);
    }, true);
}

void UDisks::createPartition(const Disk &disk, quint64 offset, quint64 size, const QString &fsType, const QString &label,
                             const QString &passphrase, bool besideSystem)
{
    const QString name = shortDevice(disk.device);
    const QString failure = tr("Couldn't create a partition on %1").arg(name);
    if (!besideSystem && refuseSystem(&disk, failure))
        return;

    // 1 MiB aligned; keep the last MiB free for the GPT backup header
    constexpr quint64 MiB = 1024 * 1024;
    quint64 freeEnd = 0;
    for (const Span &s : diskSpans(disk)) {
        if (s.isFree() && offset >= s.offset && offset < s.offset + s.size)
            freeEnd = s.offset + s.size;
    }
    freeEnd = std::min<quint64>(freeEnd, disk.size > MiB ? disk.size - MiB : 0) / MiB * MiB;
    const quint64 start = std::max<quint64>((offset + MiB - 1) / MiB * MiB, MiB);
    const quint64 end = std::min<quint64>(start + (size + MiB - 1) / MiB * MiB, freeEnd);
    if (end <= start) {
        emit operationFinished(false, failure + QStringLiteral(": ") + tr("not enough space"));
        return;
    }

    const QString partName = disk.tableType == QLatin1String("gpt") ? label : QString(); // no names on MBR
    call(disk.blockPath, kPartitionTable, QStringLiteral("CreatePartitionAndFormat"),
         {start, end - start, partitionTypeFor(disk.tableType, fsType, !passphrase.isEmpty()), partName, options(),
          fsType, options(formatOptions(fsType, label, passphrase))},
         [name, fsName = filesystem(fsType) ? filesystem(fsType)->name : fsType, start, end](const QDBusMessage &) {
             return tr("Created a %1 %2 partition on %3").arg(formatSize(end - start), fsName, name);
         },
         failure);
}

void UDisks::createPartitionTable(const Disk &disk, const QString &tableType)
{
    const QString name = shortDevice(disk.device);
    const QString failure = tr("Couldn't initialize %1").arg(name);
    if (refuseSystem(&disk, failure))
        return;
    const QString path = disk.blockPath;
    // "tear-down" also takes the drive's lines out of /etc/fstab and /etc/crypttab, which
    // polkit always wants the admin password for. Only ask for it when there's something there.
    bool tearDown = false;
    for (const Volume &v : disk.volumes)
        tearDown = tearDown || !v.fstab.isEmpty() || v.encrypted;
    QVariantMap extra;
    if (tearDown)
        extra.insert(QStringLiteral("tear-down"), true);
    const QString device = disk.device;
    unmountThen(disk.volumes, failure, [this, path, device, tableType, name, failure, extra] {
        const QString done = tr("%1 now has an empty %2 partition table").arg(name, tableType == QLatin1String("gpt") ? QStringLiteral("GPT") : QStringLiteral("MBR"));
        formatBlock(path, device, {tableType, options(extra)}, failure,
                    [this, path, tableType, failure, done](const QDBusMessage &) { confirmTable(path, tableType, failure, done); }, {},
                    [this, path, device, tableType, failure](const QString &error) { tableDirectly(path, device, tableType, failure, error); });
    }, true);
}

void UDisks::detectFilesystems()
{
    m_filesystems = {
        {QStringLiteral("exfat"), tr("exFAT"), tr("USB drives, works everywhere"), 11, QStringLiteral("exfatprogs")},
        {QStringLiteral("vfat"), tr("FAT32"), tr("old devices, max 4 GB per file"), 11, QStringLiteral("dosfstools")},
        {QStringLiteral("ntfs"), tr("NTFS"), tr("Windows"), 32, QStringLiteral("ntfs-3g")},
        {QStringLiteral("ext4"), tr("ext4"), tr("Linux"), 16, QStringLiteral("e2fsprogs")},
        {QStringLiteral("btrfs"), tr("Btrfs"), tr("Linux, snapshots"), 255, QStringLiteral("btrfs-progs")},
        {QStringLiteral("xfs"), tr("XFS"), tr("Linux, big files"), 12, QStringLiteral("xfsprogs")},
    };
    for (FsType &fs : m_filesystems) {
        QDBusMessage message = QDBusMessage::createMethodCall(kService, QStringLiteral("/org/freedesktop/UDisks2/Manager"),
                                                              QStringLiteral("org.freedesktop.UDisks2.Manager"), QStringLiteral("CanFormat"));
        message << fs.id;
        const QDBusMessage reply = QDBusConnection::systemBus().call(message, QDBus::Block, 3000);
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
            continue;
        const QDBusArgument arg = reply.arguments().constFirst().value<QDBusArgument>();
        QString utility;
        arg.beginStructure();
        arg >> fs.available >> utility;
        arg.endStructure();

        for (const auto &[method, flag] : {std::pair{QStringLiteral("CanCheck"), &fs.canCheck}, std::pair{QStringLiteral("CanRepair"), &fs.canRepair}}) {
            QDBusMessage can = QDBusMessage::createMethodCall(kService, kManagerPath, kManager, method);
            can << fs.id;
            const QDBusMessage canReply = QDBusConnection::systemBus().call(can, QDBus::Block, 3000);
            if (canReply.type() == QDBusMessage::ReplyMessage && !canReply.arguments().isEmpty()) {
                const QDBusArgument c = canReply.arguments().constFirst().value<QDBusArgument>();
                QString missing;
                c.beginStructure();
                c >> *flag >> missing;
                c.endStructure();
            }
        }
        // errors for filesystems that can't resize at all (exfat)
        QDBusMessage resize = QDBusMessage::createMethodCall(kService, QStringLiteral("/org/freedesktop/UDisks2/Manager"),
                                                             QStringLiteral("org.freedesktop.UDisks2.Manager"), QStringLiteral("CanResize"));
        resize << fs.id;
        const QDBusMessage resizeReply = QDBusConnection::systemBus().call(resize, QDBus::Block, 3000);
        if (resizeReply.type() == QDBusMessage::ReplyMessage && !resizeReply.arguments().isEmpty()) {
            const QDBusArgument r = resizeReply.arguments().constFirst().value<QDBusArgument>();
            quint64 modes = 0;
            r.beginStructure();
            r >> fs.resizeAvailable >> modes >> utility;
            r.endStructure();
            fs.resizeModes = int(modes);
        }
    }
}

void UDisks::resizeLimitsThen(const Volume &volume, QObject *context, const std::function<void(const ResizeLimits &)> &then)
{
    const ResizeLimits limits = resizeLimits(volume);
    QTimer::singleShot(0, context, [limits, then] { then(limits); });
}

void UDisks::resize(const Volume &volume, quint64 newSize)
{
    constexpr quint64 MiB = 1024 * 1024;
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't resize %1").arg(name);
    if (refuseSystem(diskOf(volume), failure))
        return;
    const ResizeLimits limits = resizeLimits(volume);
    newSize = newSize / MiB * MiB;
    if (!limits.possible || newSize < limits.minSize || newSize > limits.maxSize || newSize == volume.size) {
        emit operationFinished(false, failure + QStringLiteral(": ")
                                          + (limits.possible ? tr("size out of range") : limits.reason));
        return;
    }

    const bool shrink = newSize < volume.size;
    const QString path = volume.objectPath;
    const QString done = tr("Resized %1 to %2").arg(name, formatSize(newSize));
    // resize2fs won't shrink an ext file system that's been mounted since its last full check
    // ("Please run e2fsck -f first"), so it's checked first, unmounted.
    const bool checkFirst = shrink && volume.fsType.startsWith(QLatin1String("ext"));
    auto resizeNow = [this, path, newSize, shrink, checkFirst, failure, done] {
        if (shrink) {
            // fs first so the partition never cuts into data
            auto shrinkNow = [this, path, newSize, failure, done] {
                callThen(path, kFilesystem, QStringLiteral("Resize"), {newSize, options()}, failure, [this, path, newSize, failure, done](const QDBusMessage &) {
                    call(path, kPartition, QStringLiteral("Resize"), {newSize, options()},
                         [done](const QDBusMessage &) { return done; }, failure);
                });
            };
            if (checkFirst)
                callThen(path, kFilesystem, QStringLiteral("Repair"), {options()}, failure, [shrinkNow](const QDBusMessage &) { shrinkNow(); });
            else
                shrinkNow();
        } else {
            // size 0 = fill the partition
            callThen(path, kPartition, QStringLiteral("Resize"), {newSize, options()}, failure, [this, path, newSize, failure, done](const QDBusMessage &) {
                call(path, kFilesystem, QStringLiteral("Resize"), {quint64(0), options()},
                     [done](const QDBusMessage &) { return done; }, failure);
            });
        }
    };

    if (!resizeNeedsRemount(volume, shrink))
        resizeNow();
    else if (volume.mountPoints.isEmpty())
        callThen(path, kFilesystem, QStringLiteral("Mount"), {options()}, failure, [resizeNow](const QDBusMessage &) { resizeNow(); });
    else
        unmountThen({volume}, failure, resizeNow);
}

QString UDisks::daemonVersion() const
{
    QDBusMessage message = QDBusMessage::createMethodCall(kService, QStringLiteral("/org/freedesktop/UDisks2/Manager"),
                                                          QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    message << QStringLiteral("org.freedesktop.UDisks2.Manager") << QStringLiteral("Version");
    const QDBusMessage reply = QDBusConnection::systemBus().call(message, QDBus::Block, 3000);
    return reply.arguments().value(0).value<QDBusVariant>().variant().toString();
}
