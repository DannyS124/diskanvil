// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Wrapper around the UDisks2 D-Bus API. All writes go through UDisks2/polkit,
// nothing here opens block devices directly (raw access goes through UDisks2's
// OpenDevice/OpenForBenchmark, which also ask polkit).
//
// On Windows the same class is built from src/win/udisks_win.cpp, on Windows' own disk API:
// drives are \\.\PhysicalDriveN, partitions \\?\GLOBALROOT\Device\HarddiskN\PartitionM,
// and `device` is what people know them by there ("Disk 1", "E:").

#include "erasure.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <QVector>

#include <functional>
#include <memory>

class QDBusMessage;

// Partition, or a filesystem directly on a disk.
struct Volume {
    QString objectPath;
    QString device;
    int number = 0; // 0 = fs on the whole disk
    quint64 offset = 0;
    quint64 size = 0;
    QString label;
    QString partName; // GPT only
    QString partUuid; // the partition's own ID (GPT), or "<disk signature>-<number>" (MBR)
    QString raidMemberOf; // part of a RAID array: the array's device (md127)
    // A logical volume in an LVM group. Active ones are ordinary block devices; inactive
    // ones have no device, and objectPath is the volume's own object.
    bool isLv = false;
    bool lvActive = false;
    QString lvPath;       // its LogicalVolume object
    QString lvmMemberOf;  // a physical volume of a group: the group's name
    bool lvmUnknown = false; // an LVM member, but UDisks' lvm2 module isn't there to say more
    QString fsType;
    QString fsUsage;
    QString uuid;
    QString partType; // GUID (gpt) or "0x83" (dos)
    quint64 partFlags = 0; // GPT attribute bits, or 0x80 = bootable (dos)
    bool isContainer = false; // MBR extended
    bool isContained = false; // MBR logical
    bool isEfi = false;
    bool hasFilesystem = false;
    QStringList mountPoints;
    bool swapActive = false;
    bool encrypted = false;
    QString cleartextPath; // unlocked LUKS mapping
    QString cleartextDevice; // its /dev/dm-N
    bool cleartextHasFilesystem = false;
    QString cleartextFsType;
    QStringList cleartextMountPoints;
    QVariantMap fstab; // this volume's /etc/fstab entry, empty if none
    quint64 fsTotal = 0; // only known while mounted
    quint64 fsFree = 0;
    bool isSystem = false;

    // The object that holds the filesystem: the volume itself, or its unlocked cleartext side.
    QString filesystemPath() const { return encrypted ? cleartextPath : objectPath; }
    bool canMount() const { return encrypted ? cleartextHasFilesystem : hasFilesystem; }
    const QStringList &mounts() const { return encrypted ? cleartextMountPoints : mountPoints; }
    QString effectiveFsType() const { return encrypted && !cleartextFsType.isEmpty() ? cleartextFsType : fsType; }
};

// One thing that went into a drive's health verdict, worst first.
struct HealthReason {
    enum class Level { Note, Warning, Failing };
    Level level = Level::Note;
    QString code;      // stable ("pending", "crc", ...), so a dismissed warning is recognized
    qint64 number = -1;
    QString text;      // plain words
};

struct Health {
    enum class State { Unknown, Healthy, Warning, Failing };
    State state = State::Unknown; // Unknown = the drive doesn't report SMART (USB sticks, images)
    QString summary;
    bool nvme = false;
    double temperatureC = -1;
    quint64 powerOnHours = 0;
    qint64 badSectors = -1; // ATA: reallocated + pending, -1 if unknown
    qint64 reallocatedSectors = -1; // swapped for spares by the drive; never goes down
    qint64 pendingSectors = -1; // can't be read right now; fixable by rewriting
    int percentUsed = -1; // NVMe wear
    QStringList criticalWarnings; // NVMe
    QString selftestStatus;
    int selftestPercentRemaining = -1;
    quint64 updated = 0;
    QString key;                // where DiskAnvil keeps what it saw and what was dismissed
    QVector<HealthReason> reasons;
    int lifeLeft = -1;          // percent, SSDs that report it
    double warningTempC = -1;   // NVMe: the drive's own limits
    double criticalTempC = -1;
    bool usbNoData = false;     // a USB adapter that doesn't pass health data through
    bool smartOff = false;      // the drive can do SMART, but it's switched off
    bool nvmeSelftest = false;  // Windows: an NVMe drive that does self-tests (UDisks doesn't say)
};

struct Disk {
    QString blockPath;
    QString drivePath; // "/" for loop devices
    QString device;
    QString model;
    QString serial;
    QString revision; // firmware version
    QString bus; // "usb" or empty; on Windows also "nvme" and "sd"
    QString tableType; // "gpt", "dos" or empty
    QString tableId;   // the GPT disk ID, or the MBR disk signature (8 hex digits); lower case
    quint64 size = 0;
    int rotationRate = -1; // 0 = SSD, -1 = unknown
    bool removable = false;
    bool sdCard = false; // a memory card (SD, SDHC, SDXC, MMC), in a reader or a slot
    bool canPowerOff = false;
    bool readOnly = false;
    bool isLoop = false;
    QString backingFile; // loop devices
    bool isSystem = false;
    QString systemReason;
    bool isVentoy = false;
    bool discard = false; // supports TRIM
    int sectorSize = 512; // logical
    int ataEraseMinutes = 0; // ATA Secure Erase estimates; 0 = not supported
    int ataEnhancedEraseMinutes = 0;
    bool ataFrozen = false; // the firmware refuses security commands until the next sleep/wake
    // A Windows dynamic volume (opened by ldmtool), listed like a drive.
    bool isDynamicVolume = false;
    // A software RAID array (Linux md), listed like a drive.
    bool isRaid = false;
    QString raidPath;         // its MDRaid object
    QString raidLevel;        // "raid1", "raid5", ...
    int raidDevices = 0;      // how many drives it's made of
    int raidDegraded = 0;     // how many of those are missing
    bool raidRunning = true;
    QString raidSync;         // "idle", "check", "repair", "resync", "recover", "reshape", "frozen"
    double raidSyncDone = 0;  // 0-1
    quint64 raidSyncRate = 0; // bytes per second
    quint64 raidSyncLeftUs = 0;
    QString raidMemberOf;     // a whole drive in an array: the array's device
    // An LVM volume group, listed like a drive: its logical volumes are the volumes, its
    // free space the unallocated part. blockPath is the group's object (not a block device).
    bool isLvm = false;
    int lvmMissing = 0;       // physical volumes it's missing
    QString lvmMemberOf;      // a whole drive that's a physical volume: the group's name
    // Power settings (ATA): what the drive can do, and what UDisks keeps for it.
    bool ataPm = false;
    bool ataApm = false;
    bool ataWriteCache = false;
    bool ataWriteCacheEnabled = false;
    bool ataAam = false;          // how loud a hard drive seeks (Automatic Acoustic Management)
    int ataAamRecommended = 0;
    bool ataLookahead = false;    // reading ahead of what was asked for
    bool ataLookaheadEnabled = false;
    QVariantMap driveConfiguration;
    bool nvmeNamespace = false; // NVMe drive that can do a secure format
    QString nvmeSanitizeStatus; // "never_sanitized", "success", "inprogress", "failure"...
    int nvmeSanitizePercent = -1; // left to do while it's running
    quint64 nvmeUnallocated = 0;  // capacity no namespace uses
    QVector<int> nvmeSectorSizes; // the sector sizes the namespace can be formatted with
    int nvmeSectorSize = 0;       // the one it has
    Health health;
    QVector<Volume> volumes;   // sorted by offset
};

// A long-running UDisks2 operation (format, erase, check...).
struct Job {
    QString path; // the job's object, for Cancel
    QString operation;
    double progress = 0;
    bool progressValid = false;
    QStringList objects;
    quint64 rate = 0; // bytes per second, 0 if unknown
    bool cancelable = false;
    quint64 bytes = 0;       // how much it works through, 0 if unknown
    quint64 started = 0;     // microseconds since the epoch
    quint64 expectedEnd = 0; // the same, 0 if unknown
};

struct FsType {
    QString id;
    QString name;
    QString hint;
    int maxLabel = 0;
    QString package; // provides the mkfs tool
    bool available = false;
    int resizeModes = 0; // ResizeMode flags, 0 = can't resize
    bool resizeAvailable = false;
    bool canCheck = false;
    bool canRepair = false;
};

// BDFSResizeFlags from libblockdev (Manager.CanResize)
enum ResizeMode {
    OfflineShrink = 2,
    OfflineGrow = 4,
    OnlineShrink = 8,
    OnlineGrow = 16,
};

struct ResizeLimits {
    bool possible = false;
    QString reason;
    quint64 minSize = 0;
    quint64 maxSize = 0; // up to the end of the free space after it
    quint64 used = 0; // 0 if not mounted
};

// One row of a drive's SMART attribute table.
struct SmartAttribute {
    int id = 0;
    QString name;
    int value = -1;
    int worst = -1;
    int threshold = -1;
    QString raw; // already decoded for display
    qint64 rawValue = -1; // sector counts, decoded
    bool failing = false;
};

// Volume or unallocated range, in disk order.
struct Span {
    int volume = -1; // -1 = unallocated
    quint64 offset = 0;
    quint64 size = 0;
    bool isFree() const { return volume < 0; }
};

QVector<Span> diskSpans(const Disk &disk);

// The record of an erasure starting now (erasure.h).
erasure::Record erasureOf(const Disk &disk, const QString &method, bool zeros);

class UDisks : public QObject
{
    Q_OBJECT
public:
    explicit UDisks(QObject *parent = nullptr);

    bool isAvailable() const { return m_error.isEmpty(); }
    QString lastError() const { return m_error; }
    const QVector<Disk> &disks() const { return m_disks; }
    const QVector<Job> &jobs() const { return m_jobs; }
    const QVector<FsType> &filesystems() const { return m_filesystems; }
    const Disk *diskOf(const Volume &volume) const;
    const Disk *diskByPath(const QString &blockPath) const;
    bool isBusy() const { return m_pending > 0; }
    QString daemonVersion() const;

    // false: fail instead of prompting (selftest)
    void setInteractive(bool interactive) { m_interactive = interactive; }
    // For tests: report this health for a drive instead of its own, from the next refresh on.
    void setHealthForTest(const QString &blockPath, const Health &health) { m_testHealth.insert(blockPath, health); }

    void refresh();

    // Async, results via operationFinished. System disks are refused here as well.
    // readOnly: nothing on it can change while it's mounted (Scan for Viruses looks at drives so).
    void mount(const Volume &volume, bool readOnly = false);
    void unmount(const Volume &volume);
    void setLabel(const Volume &volume, const QString &label);
    // Type and Flags: the type first (when it's not empty), then the flags (when asked to).
    void setPartitionTypeAndFlags(const Volume &volume, const QString &type, bool setFlags, quint64 flags);
    // A new file system ID, for a clone kept next to its original.
    void setUuid(const Volume &volume, const QString &uuid);
#ifdef Q_OS_WIN
    // New disk and partition IDs, through Windows' own drive layout call (on Linux they're
    // written to the drive by newids::giveDiskNewIds).
    void giveNewDiskIds(const Disk &disk);
#endif
    // passphrase non-empty: LUKS2 container with the filesystem inside
    void format(const Volume &volume, const QString &fsType, const QString &label, const QString &passphrase = {});
    void deletePartition(const Volume &volume);
    // rounded to 1 MiB, clamped to the free range
    void createPartition(const Disk &disk, quint64 offset, quint64 size, const QString &fsType, const QString &label,
                         const QString &passphrase = {});
    void createPartitionTable(const Disk &disk, const QString &tableType); // "gpt" or "dos"
    // shrink: fs then partition; grow: partition then fs
    void resize(const Volume &volume, quint64 newSize);
    ResizeLimits resizeLimits(const Volume &volume) const;
    // The same, worked out fully before `then` gets them: on Windows with its own (slower) answer
    // about how far NTFS can shrink, which its unmovable files decide.
    void resizeLimitsThen(const Volume &volume, QObject *context, const std::function<void(const ResizeLimits &)> &then);
    // Btrfs only resizes mounted, ext4 only shrinks unmounted
    bool resizeNeedsRemount(const Volume &volume, bool shrink) const;
    const FsType *filesystem(const QString &id) const;

    // Unmounts everything on the disk, locks encrypted volumes, then cuts power (USB drives).
    void powerOff(const Disk &disk);
    // Unmounts first; result also via checkFinished.
    void check(const Volume &volume);
    void repair(const Volume &volume);
    // /etc/fstab entry so the volume mounts at boot (under /mnt).
    void setMountAtStartup(const Volume &volume, bool enable);
    static QString startupMountPoint(const Volume &volume);
    // Disk images: attach as a loop device (read-only by default), detach again.
    void openImage(const QString &path, bool readOnly = true);
    void detachImage(const Disk &disk);
#ifdef Q_OS_WIN
    // An ISO opened the way File Explorer does it, read-only with a drive letter: `done` gets its
    // root ("E:/") and whether it was opened here (one that's open already is used as it is), or
    // why not. closeIso() closes one that was opened here.
    void openIso(const QString &path, QObject *context, const std::function<void(const QString &root, bool openedHere, const QString &error)> &done);
    void closeIso(const QString &path, QObject *context, const std::function<void()> &done);
#endif
    // LUKS
    // `extra`: more of UDisks' unlock options (VeraCrypt's "pim", "hidden", "system", "keyfiles").
    void unlock(const Volume &volume, const QString &passphrase, const QVariantMap &extra = {});
    void lock(const Volume &volume);
    void changePassphrase(const Volume &volume, const QString &oldPassphrase, const QString &newPassphrase);
    // Overwrite the whole disk with zeros; leaves it without a partition table.
    void wipe(const Disk &disk);
    // The drive's own erase command: ATA Security Erase (normal or enhanced) or an NVMe
    // format with secure erase (user data or crypto). Reaches spare areas a wipe can't.
    enum class EraseMethod { AtaNormal, AtaEnhanced, NvmeUserData, NvmeCrypto, NvmeSanitizeBlock, NvmeSanitizeCrypto };
    // nvmeSectorSize: an NVMe drive formatted with another sector size (0 keeps the one it has).
    void secureErase(const Disk &disk, EraseMethod method, int nvmeSectorSize = 0);
    static QString eraseMethodName(EraseMethod method);
    // What it means when an ATA drive refuses Secure Erase (UDisks' error text), in plain words.
    static QString secureEraseRefused(const QString &error, bool usb);
    // Gets a drive ready to have its power pulled: unmounts (and locks) what's on it, then spins
    // it down. `done` says whether it's ready; a failed unmount was reported already.
    void prepareUnplug(const Disk &disk, const std::function<void(bool ready)> &done);
    // The drive among `disks` with this serial number (without one: this model and size), or -1.
    // A drive that's unplugged and plugged back in can come back under another name.
    static int findDrive(const QVector<Disk> &disks, const QString &serial, const QString &model, quint64 size);
    // SMART
    void smartUpdate(const Disk &disk);
    void smartSelftest(const Disk &disk, const QString &type); // "short" or "extended"
    void smartSelftestAbort(const Disk &disk);
    void smartSetEnabled(const Disk &disk, bool enabled);
    // A RAID array: "check" reads it all and compares the copies, "idle" stops that.
    void raidSyncAction(const Disk &array, const QString &action);
    // Fresh health data (the temperature) without a message either way; never asks for a
    // password. Rescue Copy uses it once a minute while it watches the heat.
    void refreshHealthQuietly(const Disk &disk);
    // Power settings, kept by UDisks in /etc/udisks2 (polkit asks). Allowed on the system
    // disk too: nothing here touches what's stored.
    void setPowerSettings(const Disk &disk, const QVariantMap &configuration);
    void sleepNow(const Disk &disk);    // refused while anything on it is mounted
    int powerState(const Disk &disk);   // PmGetState, -1 if it can't tell (doesn't wake it)
    // Asks UDisks to stop a job. The call that started it then fails as "cancelled", which
    // is reported through operationFinished with `stoppedMessage` and wasStopped() true.
    void cancelJob(const QString &jobPath, const QString &stoppedMessage);
    bool wasStopped() const { return m_stopped; } // during operationFinished
    // For tests: jobs to report next to the real ones, from the next refresh on.
    void setJobsForTest(const QVector<Job> &jobs) { m_testJobs = jobs; }
    QVector<SmartAttribute> smartAttributes(const Disk &disk); // blocking
    // Raw access to a whole disk or one partition. Everything except the read-only
    // benchmark mode unmounts first; writable modes refuse system disks. polkit asks
    // for a password. The fd arrives through deviceOpened; the caller closes it.
    // Peek: read-only, as it is (nothing unmounted), and plain: no direct or synchronous reads,
    // so another program can be handed it (Try Starting It's QEMU).
    enum class OpenMode { Read, ReadWrite, ReadWriteDirect, Benchmark, BenchmarkWritable, Peek };
    void openBlock(const QString &objectPath, OpenMode mode);
    void openDevice(const Disk &disk, bool writable, bool forBenchmark = false, bool direct = false);
    // Asks the kernel to re-read the partition table (after writing one directly).
    void rescan(const QString &objectPath);

signals:
    void changed();
    void operationFinished(bool ok, const QString &message);
    void jobStopFailed(const QString &message);
    void checkFinished(const QString &objectPath, bool clean);
    void imageOpened(const QString &loopBlockPath);
    void deviceOpened(const QString &blockPath, int fd); // fd = -1 on failure (reported via operationFinished)
    // A Wipe Disk or Secure Erase finished: what was erased, and how (for its certificate).
    void erased(const erasure::Record &record);

#ifndef Q_OS_WIN
private slots:
    void onDBusSignal(const QDBusMessage &message);
#endif

public:
    // Unmounts (and with lockEncrypted, locks) the given volumes one at a time, then runs `then`,
    // or `failed` when one of them can't be.
    void unmountThen(const QVector<Volume> &volumes, const QString &failure, const std::function<void()> &then,
                     bool lockEncrypted = false, const std::function<void()> &failed = {});

private:
    bool refuseSystem(const Disk *disk, const QString &failure);
    void detectFilesystems();
#ifndef Q_OS_WIN
    using SuccessText = std::function<QString(const QDBusMessage &reply)>;
    // What a failure means in plain words, added under UDisks' own message (empty: nothing to add).
    using Explain = std::function<QString(const QString &error)>;
    void call(const QString &path, const QString &interface, const QString &method, const QVariantList &args,
              const SuccessText &success, const QString &failure, const Explain &explain = {});
    // call() that chains into `next` instead of reporting. On failure it reports, then runs
    // `failed` if there is one.
    void callThen(const QString &path, const QString &interface, const QString &method, const QVariantList &args,
                  const QString &failure, const std::function<void(const QDBusMessage &reply)> &next,
                  const std::function<void()> &failed = {}, const Explain &explain = {});
    QVariantMap options(QVariantMap extra = {}) const;
    Health readHealth(const QString &drivePath, const QVariantMap &drive, const QVariantMap &ata, const QVariantMap &nvme);
#else
    // Windows' side of things: device notifications, drive letters and running jobs
    // (src/win/udisks_win.cpp).
    struct Windows;
    std::shared_ptr<Windows> m_windows;
    void forgetProbes(const Disk *disk);
    QString chkdskTarget(const Volume &volume) const;
    // Wiping, once Windows has let go of the drive: zeros written by DiskAnvil itself, shown as
    // a job (with progress and Stop) like UDisks' own.
    void wipeOffline(const QString &path, quint64 size, int number, const QString &name, const QString &failure);
    void showWipe(const QString &id, quint64 done);
    bool smartSend(const Disk &disk, unsigned char feature, unsigned char sector);
    void ownFat32(const Volume &volume, const QString &label, const std::function<void(bool ok, const QString &error)> &done);
    void finishSmart(const Disk &disk, bool ok, const QString &message);
    // Reports, a moment later, that something isn't in the Windows version yet.
    void notOnWindows(const QString &failure);
#endif

    QVector<Disk> m_disks;
    QVector<Job> m_jobs;
    QVector<FsType> m_filesystems;
    QString m_error;
    QTimer m_debounce;
    int m_pending = 0;
    bool m_interactive = true;
    // SMART attribute details, refetched only when the drive's SmartUpdated changes.
    struct HealthCache {
        quint64 updated = 0;
        QVector<SmartAttribute> attributes; // ATA
        QVariantMap nvme;                   // NVMe SmartGetAttributes
    };
    QMap<QString, HealthCache> m_healthCache;
    QMap<QString, Health> m_testHealth;
#ifndef Q_OS_WIN
    bool m_lvmAsked = false; // asked UDisks to load its lvm2 module
#endif
    QVector<Job> m_testJobs;
    QStringList m_stopMessages; // for jobs asked to stop, in order
    bool m_stopped = false;
};
