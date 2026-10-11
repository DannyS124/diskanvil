// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// UDisks operations beyond partitioning: safe removal, check/repair, fstab, disk images,
// LUKS, wiping, SMART and raw device access.

#include "applog.h"
#include "dbusnames.h"
#include "format.h"
#include "health.h"
#include "udisks.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusUnixFileDescriptor>
#include <QFile>
#include <QRegularExpression>

#include <fcntl.h>
#include <unistd.h>

namespace {

// Sector counts are sometimes packed with other fields in the raw value (HGST/Hitachi
// put extra data in the upper bytes); the count is the low 16 bits.
qint64 sectorCount(qint64 raw)
{
    return raw > 0xFFFF ? (raw & 0xFFFF) : raw;
}

QDBusMessage blockingCall(const QString &path, const QString &interface, const QString &method, const QVariantList &args)
{
    QDBusMessage message = QDBusMessage::createMethodCall(kService, path, interface, method);
    message.setArguments(args);
    return QDBusConnection::systemBus().call(message, QDBus::Block, 15000);
}

QVector<SmartAttribute> decodeAta(const QDBusMessage &reply)
{
    QVector<SmartAttribute> out;
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return out;
    const QDBusArgument arg = reply.arguments().constFirst().value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd()) {
        uchar id = 0;
        QString name;
        ushort flags = 0;
        int value = 0, worst = 0, threshold = 0, unit = 0;
        qlonglong pretty = 0;
        QVariantMap expansion;
        arg.beginStructure();
        arg >> id >> name >> flags >> value >> worst >> threshold >> pretty >> unit >> expansion;
        arg.endStructure();

        SmartAttribute a;
        a.id = id;
        a.name = name;
        a.value = value;
        a.worst = worst;
        a.threshold = threshold;
        a.failing = threshold > 0 && value > 0 && value <= threshold;
        switch (unit) {
        case 2: // milliseconds
            a.raw = pretty >= 3600000 ? UDisks::tr("%L1 hours").arg(pretty / 3600000) : UDisks::tr("%L1 ms").arg(pretty);
            break;
        case 3: // sectors
            a.rawValue = sectorCount(pretty);
            a.raw = UDisks::tr("%L1 sectors").arg(a.rawValue);
            break;
        case 4: // millikelvin
            a.raw = QStringLiteral("%1 °C").arg(pretty / 1000.0 - 273.15, 0, 'f', 0);
            break;
        default:
            // Plain counts (errors, timeouts, retries). Seagate packs three counters into 188;
            // the low 16 bits are the one that matters, like the sector counts above.
            a.rawValue = id == 188 ? sectorCount(pretty) : pretty;
            a.raw = QString::number(pretty);
        }
        out.push_back(a);
    }
    arg.endArray();
    return out;
}

QString startupFsType(const QString &fsType)
{
    return fsType == QLatin1String("ntfs") ? QStringLiteral("ntfs3") : fsType; // the kernel's NTFS driver
}

QByteArray byteString(const QString &s)
{
    QByteArray b = s.toLocal8Bit();
    b.append('\0'); // UDisks reads these with g_variant_get_bytestring, which wants the NUL
    return b;
}

} // namespace

namespace {

// FormatNamespace's options: how to erase, and the sector size when it changes.
QVariantMap nvmeFormatOptions(bool crypto, int sectorSize)
{
    QVariantMap o{{QStringLiteral("secure_erase"), crypto ? QStringLiteral("crypto_erase") : QStringLiteral("user_data")}};
    if (sectorSize > 0) {
        o.insert(QStringLiteral("lba_data_size"), QVariant::fromValue(quint16(sectorSize)));
        o.insert(QStringLiteral("metadata_size"), QVariant::fromValue(quint16(0)));
    }
    return o;
}

} // namespace

Health UDisks::readHealth(const QString &drivePath, const QVariantMap &drive, const QVariantMap &ata, const QVariantMap &nvme)
{
    Health h;
    HealthCache &cache = m_healthCache[drivePath];
    const QString driveKey = health::keyFor(drive.value(QStringLiteral("Id")).toString(), drivePath);
    h.key = driveKey;

    if (!ata.isEmpty() && ata.value(QStringLiteral("SmartSupported")).toBool()) {
        if (!ata.value(QStringLiteral("SmartEnabled")).toBool()) {
            h.summary = tr("SMART is turned off");
            h.smartOff = true;
            return h;
        }
        h.updated = ata.value(QStringLiteral("SmartUpdated")).toULongLong();
        const double kelvin = ata.value(QStringLiteral("SmartTemperature")).toDouble();
        h.temperatureC = kelvin > 0 ? kelvin - 273.15 : -1;
        h.powerOnHours = ata.value(QStringLiteral("SmartPowerOnSeconds")).toULongLong() / 3600;
        h.selftestStatus = ata.value(QStringLiteral("SmartSelftestStatus")).toString();
        h.selftestPercentRemaining = ata.value(QStringLiteral("SmartSelftestPercentRemaining"), -1).toInt();
        if (h.updated == 0) {
            h.summary = tr("No health data yet");
            return h;
        }
        if (cache.updated != h.updated) {
            cache = {};
            cache.updated = h.updated;
            cache.attributes = decodeAta(blockingCall(drivePath, kAta, QStringLiteral("SmartGetAttributes"), {QVariantMap()}));
            health::remember(driveKey, cache.attributes);
        }
        auto raw = [&cache](int id) {
            for (const SmartAttribute &a : std::as_const(cache.attributes)) {
                if (a.id == id)
                    return qMax<qint64>(0, a.rawValue);
            }
            return qint64(cache.attributes.isEmpty() ? -1 : 0);
        };
        h.reallocatedSectors = raw(5);
        h.pendingSectors = raw(197);
        h.badSectors = cache.attributes.isEmpty() ? -1 : h.reallocatedSectors + h.pendingSectors;

        health::AtaInput in;
        in.driveSaysFailing = ata.value(QStringLiteral("SmartFailing")).toBool();
        const auto failing = std::count_if(cache.attributes.cbegin(), cache.attributes.cend(), [](const SmartAttribute &a) { return a.failing; });
        in.failingNow = qMax(ata.value(QStringLiteral("SmartNumAttributesFailing")).toInt(), int(failing));
        in.failedBefore = ata.value(QStringLiteral("SmartNumAttributesFailedInThePast")).toInt();
        in.attributes = cache.attributes;
        in.seen = health::seen(driveKey);
        in.temperatureC = h.temperatureC;
        in.ssd = drive.value(QStringLiteral("RotationRate"), -1).toInt() == 0;
        const health::Verdict v = health::ata(in);
        h.state = v.state;
        h.summary = v.summary;
        h.reasons = v.reasons;
        h.lifeLeft = v.lifeLeft;
        return h;
    }

    if (!nvme.isEmpty()) {
        h.nvme = true;
        h.updated = nvme.value(QStringLiteral("SmartUpdated")).toULongLong();
        const double kelvin = nvme.value(QStringLiteral("SmartTemperature")).toDouble();
        h.temperatureC = kelvin > 0 ? kelvin - 273.15 : -1;
        h.powerOnHours = nvme.value(QStringLiteral("SmartPowerOnHours")).toULongLong();
        h.criticalWarnings = nvme.value(QStringLiteral("SmartCriticalWarning")).toStringList();
        h.selftestStatus = nvme.value(QStringLiteral("SmartSelftestStatus")).toString();
        h.selftestPercentRemaining = nvme.value(QStringLiteral("SmartSelftestPercentRemaining"), -1).toInt();
        if (h.updated == 0) {
            h.summary = tr("No health data yet");
            return h;
        }
        if (cache.updated != h.updated) {
            cache = {};
            cache.updated = h.updated;
            const QDBusMessage reply = blockingCall(drivePath, kNvme, QStringLiteral("SmartGetAttributes"), {QVariantMap()});
            if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty())
                reply.arguments().constFirst().value<QDBusArgument>() >> cache.nvme;
        }
        const QVariantMap &a = cache.nvme;
        h.percentUsed = a.value(QStringLiteral("percent_used"), -1).toInt();
        const int wctemp = a.value(QStringLiteral("wctemp")).toInt(), cctemp = a.value(QStringLiteral("cctemp")).toInt();
        h.warningTempC = wctemp > 0 ? wctemp - 273.15 : -1;
        h.criticalTempC = cctemp > 0 ? cctemp - 273.15 : -1;

        health::NvmeInput in;
        in.criticalWarnings = h.criticalWarnings;
        in.availableSpare = a.contains(QStringLiteral("avail_spare")) ? a.value(QStringLiteral("avail_spare")).toInt() : -1;
        in.spareThreshold = a.value(QStringLiteral("spare_thresh"), -1).toInt();
        in.percentUsed = h.percentUsed;
        in.mediaErrors = a.value(QStringLiteral("media_errors")).toLongLong();
        in.temperatureC = h.temperatureC;
        in.warningTempC = h.warningTempC;
        const health::Verdict v = health::nvme(in);
        h.state = v.state;
        h.summary = v.summary;
        h.reasons = v.reasons;
        h.lifeLeft = h.percentUsed >= 0 ? qMax(0, 100 - h.percentUsed) : -1;
        return h;
    }

    // Many USB adapters don't pass the drive's health data through.
    h.usbNoData = drive.value(QStringLiteral("ConnectionBus")).toString() == QLatin1String("usb")
        && !drive.value(QStringLiteral("Removable")).toBool();
    return h;
}

QVector<SmartAttribute> UDisks::smartAttributes(const Disk &disk)
{
    if (disk.health.state == Health::State::Unknown && disk.health.updated == 0)
        return {};
    if (!disk.health.nvme)
        return decodeAta(blockingCall(disk.drivePath, kAta, QStringLiteral("SmartGetAttributes"), {QVariantMap()}));

    QVector<SmartAttribute> out;
    const QDBusMessage reply = blockingCall(disk.drivePath, kNvme, QStringLiteral("SmartGetAttributes"), {QVariantMap()});
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return out;
    QVariantMap a;
    reply.arguments().constFirst().value<QDBusArgument>() >> a;
    auto add = [&out, &a](const char *key, const QString &name, const std::function<QString(const QVariant &)> &fmt) {
        if (a.contains(QLatin1String(key)))
            out.push_back({0, name, -1, -1, -1, fmt(a.value(QLatin1String(key))), false});
    };
    auto number = [](const QVariant &v) { return QStringLiteral("%L1").arg(v.toLongLong()); };
    auto percent = [](const QVariant &v) { return QStringLiteral("%1%").arg(v.toInt()); };
    auto bytes = [](const QVariant &v) { return formatSize(v.toULongLong()); };
    add("avail_spare", tr("Available spare"), percent);
    add("spare_thresh", tr("Spare threshold"), percent);
    add("percent_used", tr("Life used"), percent);
    add("total_data_read", tr("Data read"), bytes);
    add("total_data_written", tr("Data written"), bytes);
    add("power_cycles", tr("Power cycles"), number);
    add("unsafe_shutdowns", tr("Unsafe shutdowns"), number);
    add("media_errors", tr("Media errors"), number);
    add("num_err_log_entries", tr("Error log entries"), number);
    add("warning_temp_time", tr("Minutes above warning temperature"), number);
    add("critical_temp_time", tr("Minutes above critical temperature"), number);
    // Every temperature sensor (Kelvin; 0 = not there), and the drive's own limits.
    QStringList sensors;
    const QVariant temps = a.value(QStringLiteral("temp_sensors"));
    if (temps.canConvert<QDBusArgument>()) {
        const QDBusArgument arg = temps.value<QDBusArgument>();
        arg.beginArray();
        while (!arg.atEnd()) {
            quint16 kelvin = 0;
            arg >> kelvin;
            if (kelvin > 0)
                sensors << QStringLiteral("%1 °C").arg(qRound(kelvin - 273.15));
        }
        arg.endArray();
    }
    if (!sensors.isEmpty())
        out.push_back({0, tr("Temperature sensors"), -1, -1, -1, sensors.join(QStringLiteral(", ")), -1, false});
    const int wctemp = a.value(QStringLiteral("wctemp")).toInt(), cctemp = a.value(QStringLiteral("cctemp")).toInt();
    if (wctemp > 0 && cctemp > 0)
        out.push_back({0, tr("Temperature limits"), -1, -1, -1,
                       tr("warning at %1 °C, critical at %2 °C").arg(qRound(wctemp - 273.15)).arg(qRound(cctemp - 273.15)), -1, false});
    return out;
}

void UDisks::smartUpdate(const Disk &disk)
{
    const QString name = shortDevice(disk.device);
    call(disk.drivePath, disk.health.nvme ? kNvme : kAta, QStringLiteral("SmartUpdate"), {options()},
         [name](const QDBusMessage &) { return tr("Read fresh health data from %1").arg(name); },
         tr("Couldn't read health data from %1").arg(name));
}

void UDisks::smartSelftest(const Disk &disk, const QString &type)
{
    const QString name = shortDevice(disk.device);
    call(disk.drivePath, disk.health.nvme ? kNvme : kAta, QStringLiteral("SmartSelftestStart"), {type, options()},
         [name](const QDBusMessage &) { return tr("Self-test started on %1").arg(name); },
         tr("Couldn't start a self-test on %1").arg(name));
}

void UDisks::smartSetEnabled(const Disk &disk, bool enabled)
{
    const QString name = shortDevice(disk.device);
    call(disk.drivePath, kAta, QStringLiteral("SmartSetEnabled"), {enabled, options()},
         [name, enabled](const QDBusMessage &) { return enabled ? tr("SMART is on for %1").arg(name) : tr("SMART is off for %1").arg(name); },
         tr("Couldn't change SMART on %1").arg(name));
}

void UDisks::raidSyncAction(const Disk &array, const QString &action)
{
    const QString name = shortDevice(array.device);
    const bool check = action == QLatin1String("check");
    call(array.raidPath, kMDRaid, QStringLiteral("RequestSyncAction"), {action, options()},
         [name, check](const QDBusMessage &) { return check ? tr("Checking %1").arg(name) : tr("Stopped checking %1").arg(name); },
         check ? tr("Couldn't check %1").arg(name) : tr("Couldn't stop checking %1").arg(name));
}

void UDisks::refreshHealthQuietly(const Disk &disk)
{
    QDBusMessage message = QDBusMessage::createMethodCall(kService, disk.drivePath, disk.health.nvme ? kNvme : kAta, QStringLiteral("SmartUpdate"));
    message.setArguments({options()});
    message.setInteractiveAuthorizationAllowed(false);
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 60000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        watcher->deleteLater();
        refresh();
    });
}

void UDisks::smartSelftestAbort(const Disk &disk)
{
    const QString name = shortDevice(disk.device);
    call(disk.drivePath, disk.health.nvme ? kNvme : kAta, QStringLiteral("SmartSelftestAbort"), {options()},
         [name](const QDBusMessage &) { return tr("Stopped the self-test on %1").arg(name); },
         tr("Couldn't stop the self-test on %1").arg(name));
}

void UDisks::setPowerSettings(const Disk &disk, const QVariantMap &configuration)
{
    const QString name = shortDevice(disk.device);
    call(disk.drivePath, kDrive, QStringLiteral("SetConfiguration"), {configuration, options()},
         [name](const QDBusMessage &) { return tr("Saved the power settings of %1").arg(name); },
         tr("Couldn't save the power settings of %1").arg(name));
}

void UDisks::sleepNow(const Disk &disk)
{
    const QString name = shortDevice(disk.device);
    for (const Volume &v : disk.volumes) {
        if (!v.mounts().isEmpty() || v.swapActive) {
            emit operationFinished(false, tr("Couldn't put %1 to sleep: %2 is in use. Unmount it first.").arg(name, shortDevice(v.device)));
            return;
        }
    }
    call(disk.drivePath, kAta, QStringLiteral("PmStandby"), {options()},
         [name](const QDBusMessage &) { return tr("%1 is asleep now. It wakes up by itself when it's used.").arg(name); },
         tr("Couldn't put %1 to sleep").arg(name));
}

int UDisks::powerState(const Disk &disk)
{
    const QDBusMessage reply = blockingCall(disk.drivePath, kAta, QStringLiteral("PmGetState"), {options()});
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return -1;
    return int(reply.arguments().constFirst().value<uchar>());
}

void UDisks::cancelJob(const QString &jobPath, const QString &stoppedMessage)
{
    m_stopMessages << stoppedMessage;
    QDBusMessage message = QDBusMessage::createMethodCall(kService, jobPath, kJob, QStringLiteral("Cancel"));
    message.setArguments({options()});
    message.setInteractiveAuthorizationAllowed(m_interactive);
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 60000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, stoppedMessage] {
        watcher->deleteLater();
        if (watcher->isError()) {
            m_stopMessages.removeOne(stoppedMessage);
            emit jobStopFailed(tr("Couldn't stop it: %1").arg(watcher->error().message()));
        }
    });
}

void UDisks::powerOff(const Disk &disk)
{
    const QString name = shortDevice(disk.device);
    const QString failure = tr("Couldn't safely remove %1").arg(name);
    if (refuseSystem(&disk, failure))
        return;
    if (!disk.canPowerOff) {
        emit operationFinished(false, failure + QStringLiteral(": ") + tr("this drive can't be powered off"));
        return;
    }
    const QString drive = disk.drivePath;
    unmountThen(disk.volumes, failure, [this, drive, name, failure] {
        call(drive, kDrive, QStringLiteral("PowerOff"), {options()},
             [name](const QDBusMessage &) { return tr("%1 can be unplugged now").arg(name); }, failure);
    }, true);
}

void UDisks::check(const Volume &volume)
{
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't check %1").arg(name);
    if (refuseSystem(diskOf(volume), failure))
        return;
    if (!volume.canMount()) {
        emit operationFinished(false, failure + QStringLiteral(": ") + tr("no file system to check"));
        return;
    }
    const QString fs = volume.filesystemPath();
    const QString id = volume.objectPath;
    unmountThen({volume}, failure, [this, fs, id, name, failure] {
        callThen(fs, kFilesystem, QStringLiteral("Check"), {options()}, failure, [this, id, name](const QDBusMessage &reply) {
            const bool clean = reply.arguments().value(0).toBool();
            emit checkFinished(id, clean);
            emit operationFinished(true, clean ? tr("No problems found on %1").arg(name) : tr("%1 has errors").arg(name));
        });
    });
}

void UDisks::repair(const Volume &volume)
{
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't repair %1").arg(name);
    if (refuseSystem(diskOf(volume), failure))
        return;
    const QString fs = volume.filesystemPath();
    unmountThen({volume}, failure, [this, fs, name, failure] {
        callThen(fs, kFilesystem, QStringLiteral("Repair"), {options()}, failure, [this, name](const QDBusMessage &reply) {
            const bool repaired = reply.arguments().value(0).toBool();
            emit operationFinished(repaired, repaired ? tr("Repaired %1").arg(name)
                                                      : tr("Some errors on %1 couldn't be repaired").arg(name));
        });
    });
}

QString UDisks::startupMountPoint(const Volume &volume)
{
    QString name = !volume.label.isEmpty() ? volume.label : volume.uuid.left(8);
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    return QStringLiteral("/mnt/") + (name.isEmpty() ? shortDevice(volume.device) : name);
}

void UDisks::setMountAtStartup(const Volume &volume, bool enable)
{
    const QString name = shortDevice(volume.device);
    const QString failure = enable ? tr("Couldn't make %1 mount at startup").arg(name)
                                   : tr("Couldn't stop %1 mounting at startup").arg(name);
    if (refuseSystem(diskOf(volume), failure))
        return;
    if (volume.encrypted || !volume.hasFilesystem || volume.uuid.isEmpty()) {
        emit operationFinished(false, failure + QStringLiteral(": ") + tr("only unencrypted file systems can mount at startup"));
        return;
    }

    QVariantMap details = volume.fstab;
    if (enable) {
        if (!details.isEmpty()) {
            emit operationFinished(true, tr("%1 already mounts at startup").arg(name));
            return;
        }
        QString opts = QStringLiteral("nosuid,nodev,nofail,x-gvfs-show,x-mount.mkdir");
        // These have no Unix permissions; give the files to whoever set this up.
        static const QStringList noPermissions = {QStringLiteral("vfat"), QStringLiteral("exfat"), QStringLiteral("ntfs")};
        if (noPermissions.contains(volume.fsType))
            opts += QStringLiteral(",uid=%1,gid=%2").arg(getuid()).arg(getgid());
        details = {
            {QStringLiteral("fsname"), byteString(QStringLiteral("UUID=") + volume.uuid)},
            {QStringLiteral("dir"), byteString(startupMountPoint(volume))},
            {QStringLiteral("type"), byteString(startupFsType(volume.fsType))},
            {QStringLiteral("opts"), byteString(opts)},
            {QStringLiteral("freq"), 0},
            {QStringLiteral("passno"), 0},
        };
    } else if (details.isEmpty()) {
        emit operationFinished(true, tr("%1 doesn't mount at startup").arg(name));
        return;
    }

    QDBusArgument item;
    item.beginStructure();
    item << QStringLiteral("fstab") << details;
    item.endStructure();
    const QString dir = startupMountPoint(volume);
    call(volume.objectPath, kBlock, enable ? QStringLiteral("AddConfigurationItem") : QStringLiteral("RemoveConfigurationItem"),
         {QVariant::fromValue(item), options()},
         [name, dir, enable](const QDBusMessage &) {
             return enable ? tr("%1 will mount at %2 every time you start your PC").arg(name, dir)
                           : tr("%1 won't mount at startup anymore").arg(name);
         },
         failure);
}

void UDisks::openImage(const QString &path, bool readOnly)
{
    const QString name = path.section(QLatin1Char('/'), -1);
    const QString failure = tr("Couldn't open %1").arg(name);
    QFile file(path);
    if (!file.open(readOnly ? QIODevice::ReadOnly : QIODevice::ReadWrite)) {
        emit operationFinished(false, failure + QStringLiteral(": ") + file.errorString());
        return;
    }
    // QDBusUnixFileDescriptor keeps its own copy of the fd, so the file can close afterwards.
    call(kManagerPath, kManager, QStringLiteral("LoopSetup"),
         {QVariant::fromValue(QDBusUnixFileDescriptor(file.handle())), options({{QStringLiteral("read-only"), readOnly}})},
         [this, name](const QDBusMessage &reply) {
             emit imageOpened(reply.arguments().value(0).value<QDBusObjectPath>().path());
             return tr("Opened %1").arg(name);
         },
         failure);
}

void UDisks::detachImage(const Disk &disk)
{
    const QString name = disk.backingFile.section(QLatin1Char('/'), -1);
    const QString failure = tr("Couldn't close %1").arg(name);
    if (!disk.isLoop) {
        emit operationFinished(false, failure + QStringLiteral(": ") + tr("not a disk image"));
        return;
    }
    const QString path = disk.blockPath;
    unmountThen(disk.volumes, failure, [this, path, name, failure] {
        call(path, kLoop, QStringLiteral("Delete"), {options()},
             [name](const QDBusMessage &) { return tr("Closed %1").arg(name); }, failure);
    }, true);
}

void UDisks::unlock(const Volume &volume, const QString &passphrase)
{
    const QString name = shortDevice(volume.device);
    call(volume.objectPath, kEncrypted, QStringLiteral("Unlock"), {passphrase, options()},
         [name](const QDBusMessage &) { return tr("Unlocked %1").arg(name); },
         tr("Couldn't unlock %1. Is the passphrase right?").arg(name));
}

void UDisks::lock(const Volume &volume)
{
    const QString name = shortDevice(volume.device);
    if (refuseSystem(diskOf(volume), tr("Couldn't lock %1").arg(name)))
        return;
    unmountThen({volume}, tr("Couldn't lock %1").arg(name), [this, name] {
        emit operationFinished(true, tr("Locked %1").arg(name));
    }, true);
}

void UDisks::changePassphrase(const Volume &volume, const QString &oldPassphrase, const QString &newPassphrase)
{
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't change the passphrase of %1").arg(name);
    if (refuseSystem(diskOf(volume), failure))
        return;
    call(volume.objectPath, kEncrypted, QStringLiteral("ChangePassphrase"), {oldPassphrase, newPassphrase, options()},
         [name](const QDBusMessage &) { return tr("Changed the passphrase of %1").arg(name); }, failure);
}

void UDisks::wipe(const Disk &disk)
{
    const QString name = shortDevice(disk.device);
    const QString failure = tr("Couldn't wipe %1").arg(name);
    if (refuseSystem(&disk, failure))
        return;
    const QString path = disk.blockPath;
    unmountThen(disk.volumes, failure, [this, path, name, failure] {
        call(path, kBlock, QStringLiteral("Format"),
             {QStringLiteral("empty"), options({{QStringLiteral("erase"), QStringLiteral("zero")}, {QStringLiteral("tear-down"), true}})},
             [name](const QDBusMessage &) { return tr("%1 has been wiped").arg(name); }, failure);
    }, true);
}

void UDisks::secureErase(const Disk &disk, EraseMethod method, int nvmeSectorSize)
{
    const QString name = shortDevice(disk.device);
    const QString failure = tr("Couldn't erase %1").arg(name);
    if (refuseSystem(&disk, failure))
        return;
    const Disk *d = diskByPath(disk.blockPath);
    const bool ata = method == EraseMethod::AtaNormal || method == EraseMethod::AtaEnhanced;
    QString refusal;
    if (!d)
        refusal = tr("it isn't there anymore");
    else if (d->isLoop)
        refusal = tr("it's a disk image, not a drive");
    else if (ata && d->ataEraseMinutes <= 0 && d->ataEnhancedEraseMinutes <= 0)
        refusal = tr("the drive doesn't support it");
    else if (ata && d->ataFrozen)
        refusal = tr("the drive is frozen until the PC sleeps and wakes once");
    else if (!ata && !d->nvmeNamespace)
        refusal = tr("the drive doesn't support it");
    if (!refusal.isEmpty()) {
        emit operationFinished(false, failure + QStringLiteral(": ") + refusal);
        return;
    }
    const QString blockPath = d->blockPath, drivePath = d->drivePath;
    const bool usb = d->bus == QLatin1String("usb");
    unmountThen(d->volumes, failure, [this, method, ata, blockPath, drivePath, name, failure, usb, nvmeSectorSize] {
        // The drive erased itself, so the system still has the old partitions in mind until it
        // reads the drive again. Whatever that says, the erase itself worked.
        auto done = [this, blockPath, name](const QDBusMessage &) {
            QDBusMessage reread = QDBusMessage::createMethodCall(kService, blockPath, kBlock, QStringLiteral("Rescan"));
            reread << options();
            auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(reread, 60000), this);
            connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, name](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                refresh();
                emit operationFinished(true, tr("%1 has been erased. Use New Partition Table to use it again.").arg(name));
            });
        };
        if (ata)
            callThen(drivePath, kAta, QStringLiteral("SecurityEraseUnit"),
                     {options({{QStringLiteral("enhanced"), method == EraseMethod::AtaEnhanced}})}, failure, done, {},
                     [usb](const QString &error) { return secureEraseRefused(error, usb); });
        else if (method == EraseMethod::NvmeSanitizeBlock || method == EraseMethod::NvmeSanitizeCrypto)
            // Sanitize: the controller erases everything, spare areas and caches too, and keeps
            // at it after a power cut until it's done.
            callThen(drivePath, kNvme, QStringLiteral("SanitizeStart"),
                     {method == EraseMethod::NvmeSanitizeCrypto ? QStringLiteral("crypto-erase") : QStringLiteral("block-erase"), options()},
                     failure, done);
        else
            callThen(blockPath, kNvmeNamespace, QStringLiteral("FormatNamespace"),
                     {options(nvmeFormatOptions(method == EraseMethod::NvmeCrypto, nvmeSectorSize))},
                     failure, done);
    }, true);
}

void UDisks::prepareUnplug(const Disk &disk, const std::function<void(bool)> &done)
{
    const QString failure = tr("Couldn't get %1 ready to unplug").arg(shortDevice(disk.device));
    if (refuseSystem(&disk, failure))
        return done(false);
    const QString drivePath = disk.drivePath;
    unmountThen(disk.volumes, failure, [this, drivePath, done] {
        // Spun down, its heads are parked before the power goes. Old controllers can refuse that;
        // the drive then parks them itself when the power goes, so it's no reason to stop.
        QDBusMessage standby = QDBusMessage::createMethodCall(kService, drivePath, kAta, QStringLiteral("PmStandby"));
        standby << options();
        standby.setInteractiveAuthorizationAllowed(m_interactive);
        auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(standby, 30000), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [done](QDBusPendingCallWatcher *w) {
            w->deleteLater();
            qCInfo(lcOps).noquote() << "PmStandby before unplugging" << (w->isError() ? "refused: " + w->error().message() : QStringLiteral("done"));
            done(true);
        });
    }, true, [done] { done(false); });
}

void UDisks::openBlock(const QString &objectPath, OpenMode mode)
{
    const Disk *disk = nullptr;
    QVector<Volume> affected;
    QString name = objectPath;
    for (const Disk &d : m_disks) {
        if (d.blockPath == objectPath) {
            disk = &d;
            affected = d.volumes;
            name = shortDevice(d.device);
        }
        for (const Volume &v : d.volumes) {
            if (v.objectPath == objectPath) {
                disk = &d;
                affected = {v};
                name = shortDevice(v.device);
            }
        }
    }
    const QString failure = tr("Couldn't open %1").arg(name);
    if (!disk) {
        emit operationFinished(false, failure + QStringLiteral(": ") + tr("it isn't there anymore"));
        emit deviceOpened(objectPath, -1);
        return;
    }
    const bool writes = mode == OpenMode::ReadWrite || mode == OpenMode::ReadWriteDirect || mode == OpenMode::BenchmarkWritable;
    // Reading needs things unmounted too (except for the benchmark), and nothing on the
    // system disk gets unmounted.
    bool unmounts = false;
    for (const Volume &v : affected)
        unmounts = unmounts || (v.canMount() && !v.mounts().isEmpty()) || (v.encrypted && !v.cleartextPath.isEmpty());
    if ((writes || (unmounts && mode != OpenMode::Benchmark)) && refuseSystem(disk, failure)) {
        emit deviceOpened(objectPath, -1);
        return;
    }

    auto open = [this, objectPath, mode, failure] {
        QDBusMessage message;
        if (mode == OpenMode::Benchmark || mode == OpenMode::BenchmarkWritable) {
            message = QDBusMessage::createMethodCall(kService, objectPath, kBlock, QStringLiteral("OpenForBenchmark"));
            message << options({{QStringLiteral("writable"), mode == OpenMode::BenchmarkWritable}});
        } else {
            int flags = O_CLOEXEC;
            if (mode != OpenMode::Read)
                flags |= O_EXCL;
            if (mode == OpenMode::ReadWriteDirect)
                flags |= O_DIRECT;
            message = QDBusMessage::createMethodCall(kService, objectPath, kBlock, QStringLiteral("OpenDevice"));
            message << (mode == OpenMode::Read ? QStringLiteral("r") : QStringLiteral("rw")) << options({{QStringLiteral("flags"), flags}});
        }
        message.setInteractiveAuthorizationAllowed(m_interactive);
        qCInfo(lcOps).noquote() << message.member() << objectPath << "mode" << int(mode);
        ++m_pending;
        auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, kNoTimeout), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, objectPath, failure](QDBusPendingCallWatcher *w) {
            w->deleteLater();
            --m_pending;
            qCInfo(lcOps).noquote() << "open" << objectPath << (w->isError() ? QStringLiteral("failed: ") + w->error().message() : QStringLiteral("done"));
            if (w->isError()) {
                emit operationFinished(false, failure + QStringLiteral(": ") + w->error().message());
                emit deviceOpened(objectPath, -1);
                return;
            }
            const auto fd = w->reply().arguments().value(0).value<QDBusUnixFileDescriptor>();
            // Close-on-exec: programs DiskForge starts (add-ons, terminals) must never inherit a disk.
            emit deviceOpened(objectPath, fd.isValid() ? ::fcntl(fd.fileDescriptor(), F_DUPFD_CLOEXEC, 0) : -1);
        });
    };
    // Reading for a benchmark or a rescue works while mounted; everything else gets
    // unmounted (and locked) first so the data doesn't change underneath.
    // A failed unmount was reported already; whoever asked still hears it didn't open.
    if (mode == OpenMode::Benchmark)
        open();
    else
        unmountThen(affected, failure, open, true, [this, objectPath] { emit deviceOpened(objectPath, -1); });
}

void UDisks::rescan(const QString &objectPath)
{
    call(objectPath, kBlock, QStringLiteral("Rescan"), {options()},
         [](const QDBusMessage &) { return QString(); }, tr("Couldn't re-read the partition table"));
}
