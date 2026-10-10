// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// UDisks on Windows: the same class, built on Windows' own disk API instead of UDisks2.
//
// Drives come from SetupAPI's list of disk devices and are opened as \\.\PhysicalDriveN; the
// partition table from IOCTL_DISK_GET_DRIVE_LAYOUT_EX; what's in each partition from the
// volumes Windows mounted (letters, labels, free space), and from DiskForge's own look at
// the first bytes for file systems Windows can't read (ext4, LUKS...). Windows tells about
// drives and volumes coming and going through CM_Register_Notification; drive letters given
// or taken away are noticed by watching GetLogicalDrives().
//
// Everything runs as administrator (the manifest asks for it), which raw access and the
// partition table need.

#include "udisks.h"

#include "applog.h"
#include "format.h"
#include "fsprobe.h"
#include "health.h"
#include "newids.h"
#include "winutil.h"

#include <QCollator>
#include <QDir>
#include <QHash>
#include <QSysInfo>
#include <QRandomGenerator>
#include <QTimer>

#include <algorithm>

#include <fcntl.h>
#include <io.h>

#include <windows.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <setupapi.h>

namespace {

// GUID_DEVINTERFACE_DISK and GUID_DEVINTERFACE_VOLUME. winioctl.h has them too, but only as
// declarations unless initguid.h comes first.
const GUID kDiskInterface = {0x53f56307, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};
const GUID kVolumeInterface = {0x53f5630d, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};

const QString kEfiType = QStringLiteral("c12a7328-f81f-11d2-ba4b-00a0c93ec93b");
const QString kMsrType = QStringLiteral("e3c9e316-0b5c-4db8-817d-f92df00215ae");

QString diskPath(DWORD number)
{
    return QStringLiteral("\\\\.\\PhysicalDrive%1").arg(number);
}

// Partition 0 is the whole drive; the others open on their own, mounted or not.
QString partitionPath(DWORD disk, DWORD partition)
{
    return QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk%1\\Partition%2").arg(disk).arg(partition);
}

bool elevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation {};
    DWORD size = 0;
    const bool yes = GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size) && elevation.TokenIsElevated;
    CloseHandle(token);
    return yes;
}

// A volume Windows knows, and where it is.
struct VolumeInfo {
    QString name; // \\?\Volume{...}\ with the backslash on the end
    DWORD disk = 0;
    quint64 offset = 0;
    QStringList paths; // E:\ and any folders it's mounted on
    QString fs;        // Windows' name: NTFS, FAT32, exFAT...
    QString label;
    quint64 total = 0;
    quint64 free = 0;
    bool readOnly = false;
};

QString withoutSlash(QString path)
{
    if (path.endsWith(QLatin1Char('\\')))
        path.chop(1);
    return path;
}

QVector<VolumeInfo> listVolumes()
{
    QVector<VolumeInfo> out;
    wchar_t name[MAX_PATH] = {};
    HANDLE find = FindFirstVolumeW(name, MAX_PATH);
    if (find == INVALID_HANDLE_VALUE)
        return out;
    do {
        VolumeInfo v;
        v.name = win::fromWide(name);
        const win::Handle h = win::openDevice(withoutSlash(v.name), 0);
        const QByteArray extents = h.valid() ? win::ioctl(h.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS) : QByteArray();
        if (extents.size() < qsizetype(sizeof(VOLUME_DISK_EXTENTS)))
            continue; // CDs, network drives and the like
        const auto *e = reinterpret_cast<const VOLUME_DISK_EXTENTS *>(extents.constData());
        if (e->NumberOfDiskExtents != 1)
            continue; // spread over several drives (dynamic disks): not something DiskForge edits
        v.disk = e->Extents[0].DiskNumber;
        v.offset = quint64(e->Extents[0].StartingOffset.QuadPart);

        DWORD length = 0;
        GetVolumePathNamesForVolumeNameW(name, nullptr, 0, &length);
        std::wstring paths(length + 1, L'\0');
        if (length && GetVolumePathNamesForVolumeNameW(name, paths.data(), length, &length)) {
            for (const wchar_t *p = paths.c_str(); *p; p += wcslen(p) + 1)
                v.paths << win::fromWide(p);
        }
        wchar_t label[MAX_PATH + 1] = {};
        wchar_t fs[MAX_PATH + 1] = {};
        DWORD serial = 0, maxName = 0, flags = 0;
        if (GetVolumeInformationW(name, label, MAX_PATH + 1, &serial, &maxName, &flags, fs, MAX_PATH + 1)) {
            v.label = win::fromWide(label);
            v.fs = win::fromWide(fs);
            v.readOnly = flags & FILE_READ_ONLY_VOLUME;
            ULARGE_INTEGER avail {}, total {}, free {};
            if (GetDiskFreeSpaceExW(name, &avail, &total, &free)) {
                v.total = total.QuadPart;
                v.free = free.QuadPart;
            }
        }
        out.push_back(v);
    } while (FindNextVolumeW(find, name, MAX_PATH));
    FindVolumeClose(find);
    return out;
}

// Windows' names for the file systems it reads, as Linux calls them.
QString fsTypeOf(const QString &windowsName)
{
    const QString n = windowsName.toUpper();
    if (n == QLatin1String("NTFS"))
        return QStringLiteral("ntfs");
    if (n.startsWith(QLatin1String("FAT")))
        return QStringLiteral("vfat");
    if (n == QLatin1String("EXFAT"))
        return QStringLiteral("exfat");
    if (n == QLatin1String("REFS"))
        return QStringLiteral("refs");
    if (n == QLatin1String("UDF"))
        return QStringLiteral("udf");
    if (n == QLatin1String("CDFS"))
        return QStringLiteral("iso9660");
    return windowsName.toLower();
}

// The volume Windows itself is on, and the ones with its page file: none of them may be
// touched while it runs.
QStringList systemVolumes(const QVector<VolumeInfo> &volumes)
{
    QStringList out;
    wchar_t dir[MAX_PATH] = {};
    wchar_t root[MAX_PATH] = {};
    wchar_t volume[MAX_PATH] = {};
    if (GetWindowsDirectoryW(dir, MAX_PATH) && GetVolumePathNameW(dir, root, MAX_PATH)
        && GetVolumeNameForVolumeMountPointW(root, volume, MAX_PATH))
        out << win::fromWide(volume);
    for (const VolumeInfo &v : volumes) {
        for (const QString &p : v.paths) {
            if (GetFileAttributesW(win::wide(p + QStringLiteral("pagefile.sys")).c_str()) != INVALID_FILE_ATTRIBUTES)
                out << v.name;
        }
    }
    return out;
}

QString text(const QByteArray &descriptor, DWORD offset)
{
    if (!offset || offset >= DWORD(descriptor.size()))
        return {};
    return QString::fromLatin1(descriptor.constData() + offset).simplified();
}

template <typename T>
bool storageProperty(HANDLE h, STORAGE_PROPERTY_ID id, T *out)
{
    STORAGE_PROPERTY_QUERY query {};
    query.PropertyId = id;
    query.QueryType = PropertyStandardQuery;
    *out = T {};
    return win::ioctl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query, out, sizeof(T));
}

struct DriveInfo {
    DWORD number = 0;
    QString instanceId; // the device's ID in Windows' device tree
};

QVector<DriveInfo> listDrives()
{
    QVector<DriveInfo> out;
    HDEVINFO set = SetupDiGetClassDevsW(&kDiskInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return out;
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA iface {};
        iface.cbSize = sizeof iface;
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &kDiskInterface, i, &iface))
            break;
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &needed, nullptr);
        if (needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
            continue;
        QByteArray buffer(qsizetype(needed), '\0');
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA device {};
        device.cbSize = sizeof device;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, needed, nullptr, &device))
            continue;
        const win::Handle h = win::openDevice(win::fromWide(detail->DevicePath), 0);
        STORAGE_DEVICE_NUMBER number {};
        if (!h.valid() || !win::ioctl(h.get(), IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &number, sizeof number))
            continue;
        DriveInfo d;
        d.number = number.DeviceNumber;
        wchar_t id[MAX_DEVICE_ID_LEN] = {};
        if (CM_Get_Device_IDW(device.DevInst, id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS)
            d.instanceId = win::fromWide(id);
        out.push_back(d);
    }
    SetupDiDestroyDeviceInfoList(set);
    std::sort(out.begin(), out.end(), [](const DriveInfo &a, const DriveInfo &b) { return a.number < b.number; });
    return out;
}

bool isContainerType(BYTE type)
{
    return type == PARTITION_EXTENDED || type == PARTITION_XINT13_EXTENDED || type == 0x85;
}

DWORD CALLBACK deviceChanged(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA, DWORD)
{
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL || action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL)
        QMetaObject::invokeMethod(static_cast<QTimer *>(context), "start", Qt::QueuedConnection);
    return ERROR_SUCCESS;
}

} // namespace

struct UDisks::Windows {
    HCMNOTIFICATION notifications[2] = {};
    QTimer letters;
    DWORD lastLetters = 0;
    // What DiskForge found at the start of partitions Windows can't read, so a refresh
    // doesn't read every drive again (or wake a sleeping one). By drive, offset and size.
    QHash<QString, fsprobe::Result> probed;
    // Each partition's volume, \\?\Volume{...}\, for the ones Windows mounted.
    QHash<QString, QString> volumeNames;

    ~Windows()
    {
        for (HCMNOTIFICATION n : notifications) {
            if (n)
                CM_Unregister_Notification(n);
        }
    }
};

UDisks::UDisks(QObject *parent)
    : QObject(parent)
    , m_windows(std::make_shared<Windows>())
{
    // coalesce bursts (a stick arriving brings a drive and its volumes)
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(250);
    connect(&m_debounce, &QTimer::timeout, this, &UDisks::refresh);

    CM_NOTIFY_FILTER filter {};
    filter.cbSize = sizeof filter;
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.u.DeviceInterface.ClassGuid = kDiskInterface;
    CM_Register_Notification(&filter, &m_debounce, deviceChanged, &m_windows->notifications[0]);
    filter.u.DeviceInterface.ClassGuid = kVolumeInterface;
    CM_Register_Notification(&filter, &m_debounce, deviceChanged, &m_windows->notifications[1]);

    // A letter given or taken away doesn't make a device come or go.
    m_windows->letters.setInterval(2000);
    connect(&m_windows->letters, &QTimer::timeout, this, [this] {
        if (GetLogicalDrives() != m_windows->lastLetters)
            m_debounce.start();
    });
    m_windows->letters.start();

    if (!elevated())
        m_error = tr("DiskForge needs to run as administrator to see and change drives. Right-click it and choose "
                     "\"Run as administrator\".");
    detectFilesystems();
    refresh();
}

void UDisks::detectFilesystems()
{
    // Windows formats these itself. FAT32 only up to 32 GB; the rest can't be made here.
    const QString linuxOnly = tr("Windows can't make this; DiskForge Live can");
    m_filesystems = {
        {QStringLiteral("exfat"), tr("exFAT"), tr("USB drives, works everywhere"), 15, {}, true},
        {QStringLiteral("vfat"), tr("FAT32"), tr("old devices, max 4 GB per file"), 11, {}, true},
        {QStringLiteral("ntfs"), tr("NTFS"), tr("Windows"), 32, {}, true},
        {QStringLiteral("ext4"), tr("ext4"), linuxOnly, 16, {}, false},
        {QStringLiteral("btrfs"), tr("Btrfs"), linuxOnly, 255, {}, false},
        {QStringLiteral("xfs"), tr("XFS"), linuxOnly, 12, {}, false},
    };
}

void UDisks::refresh()
{
    m_windows->lastLetters = GetLogicalDrives();
    const QVector<VolumeInfo> volumes = listVolumes();
    const QStringList systemNames = systemVolumes(volumes);
    QHash<QString, fsprobe::Result> probed;
    m_windows->volumeNames.clear();

    QVector<Disk> disks;
    for (const DriveInfo &drive : listDrives()) {
        Disk d;
        d.blockPath = diskPath(drive.number);
        d.drivePath = drive.instanceId;
        d.device = QStringLiteral("Disk %1").arg(drive.number);

        const win::Handle query = win::openDevice(d.blockPath, 0);
        if (!query.valid())
            continue;
        DISK_GEOMETRY_EX geometry {};
        if (!win::ioctl(query.get(), IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geometry, sizeof geometry)
            || geometry.DiskSize.QuadPart <= 0)
            continue; // a card reader with no card in it
        d.size = quint64(geometry.DiskSize.QuadPart);
        d.sectorSize = int(geometry.Geometry.BytesPerSector ? geometry.Geometry.BytesPerSector : 512);

        STORAGE_PROPERTY_QUERY q {};
        q.PropertyId = StorageDeviceProperty;
        q.QueryType = PropertyStandardQuery;
        const QByteArray desc = win::ioctl(query.get(), IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q);
        if (desc.size() >= qsizetype(sizeof(STORAGE_DEVICE_DESCRIPTOR))) {
            const auto *s = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR *>(desc.constData());
            const QString vendor = text(desc, s->VendorIdOffset);
            const QString product = text(desc, s->ProductIdOffset);
            const bool vendorIsNoise = vendor.isEmpty() || vendor.compare(QLatin1String("ATA"), Qt::CaseInsensitive) == 0
                                    || vendor.compare(QLatin1String("NVMe"), Qt::CaseInsensitive) == 0;
            d.model = vendorIsNoise ? product : QStringLiteral("%1 %2").arg(vendor, product);
            d.revision = text(desc, s->ProductRevisionOffset);
            d.serial = text(desc, s->SerialNumberOffset);
            d.removable = s->RemovableMedia;
            switch (s->BusType) {
            case BusTypeUsb:
                d.bus = QStringLiteral("usb");
                break;
            case BusTypeNvme:
                d.bus = QStringLiteral("nvme");
                d.health.nvme = true;
                break;
            case BusTypeSd:
            case BusTypeMmc:
                d.bus = QStringLiteral("sd");
                break;
            case BusTypeFileBackedVirtual:
                d.isLoop = true;
                break;
            default:
                break;
            }
        }
        d.canPowerOff = d.bus == QLatin1String("usb");
        DEVICE_SEEK_PENALTY_DESCRIPTOR seek {};
        if (storageProperty(query.get(), StorageDeviceSeekPenaltyProperty, &seek) && seek.Size)
            d.rotationRate = seek.IncursSeekPenalty ? 1 : 0; // spins, but how fast isn't said here
        DEVICE_TRIM_DESCRIPTOR trim {};
        if (storageProperty(query.get(), StorageDeviceTrimProperty, &trim) && trim.Size)
            d.discard = trim.TrimEnabled;
        STORAGE_TEMPERATURE_DATA_DESCRIPTOR temperature {};
        if (storageProperty(query.get(), StorageDeviceTemperatureProperty, &temperature) && temperature.Size
            && temperature.InfoCount > 0 && temperature.TemperatureInfo[0].Temperature > -100)
            d.health.temperatureC = temperature.TemperatureInfo[0].Temperature;
        d.health.key = health::keyFor(d.serial.isEmpty() ? QString() : d.model + QLatin1Char('-') + d.serial, d.drivePath);
        if (!win::ioctl(query.get(), IOCTL_DISK_IS_WRITABLE, nullptr, 0, nullptr, 0) && GetLastError() == ERROR_WRITE_PROTECT)
            d.readOnly = true;

        // The partition table takes read access.
        const win::Handle disk = win::openDevice(d.blockPath, GENERIC_READ);
        const QByteArray layout = disk.valid() ? win::ioctl(disk.get(), IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, 16384) : QByteArray();
        auto probeAt = [&](quint64 offset, quint64 size, const QString &key) {
            const auto cached = m_windows->probed.constFind(key);
            fsprobe::Result r;
            if (cached != m_windows->probed.cend()) {
                r = *cached;
            } else if (disk.valid()) {
                // Whole sectors only; 4 KiB steps suit both sector sizes.
                QByteArray head(int(std::min<quint64>(size, fsprobe::kHeadSize)), '\0');
                OVERLAPPED at {};
                at.Offset = DWORD(offset);
                at.OffsetHigh = DWORD(offset >> 32);
                DWORD got = 0;
                if (ReadFile(disk.get(), head.data(), DWORD(head.size()) / 4096 * 4096, &got, &at)) {
                    head.truncate(qsizetype(got));
                    r = fsprobe::probe(head);
                }
            }
            probed.insert(key, r);
            return r;
        };

        auto addVolume = [&](Volume v, quint64 offset) {
            // What Windows mounted here, if anything.
            for (const VolumeInfo &info : volumes) {
                if (info.disk != drive.number || info.offset != offset)
                    continue;
                m_windows->volumeNames.insert(v.objectPath, info.name);
                for (const QString &p : info.paths) {
                    QString path = QDir::fromNativeSeparators(p);
                    if (path.size() > 3 && path.endsWith(QLatin1Char('/')))
                        path.chop(1);
                    v.mountPoints << path;
                }
                if (!info.fs.isEmpty()) {
                    v.fsType = fsTypeOf(info.fs);
                    v.fsUsage = QStringLiteral("filesystem");
                    v.hasFilesystem = true;
                    v.label = info.label;
                    v.fsTotal = info.total;
                    v.fsFree = info.free;
                }
                if (systemNames.contains(info.name)) {
                    v.isSystem = true;
                    d.isSystem = true;
                    d.systemReason = tr("Windows is running from it");
                }
                if (v.mountPoints.size() == 1 && v.mountPoints.first().size() <= 3)
                    v.device = v.mountPoints.first().left(2); // "E:"
            }
            if (v.partType != kMsrType && !v.isContainer) {
                const fsprobe::Result r = probeAt(offset, v.size, QStringLiteral("%1/%2/%3").arg(d.drivePath).arg(offset).arg(v.size));
                if (r.found()) {
                    if (v.fsType.isEmpty()) {
                        v.fsType = r.type;
                        v.fsUsage = r.usage;
                        v.hasFilesystem = r.usage == QLatin1String("filesystem");
                        v.label = r.label;
                    }
                    if (v.fsType == r.type)
                        v.uuid = r.uuid;
                }
            }
            d.volumes.push_back(v);
        };

        if (layout.size() >= qsizetype(sizeof(DRIVE_LAYOUT_INFORMATION_EX))) {
            const auto *info = reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX *>(layout.constData());
            const bool gpt = info->PartitionStyle == PARTITION_STYLE_GPT;
            const bool mbr = info->PartitionStyle == PARTITION_STYLE_MBR;
            // A file system on the whole stick, no partition table: Windows shows it as one
            // MBR partition starting at 0.
            const bool wholeDisk = mbr && info->PartitionCount >= 1 && info->PartitionEntry[0].StartingOffset.QuadPart == 0
                                && info->PartitionEntry[0].PartitionLength.QuadPart > 0;
            d.tableType = gpt ? QStringLiteral("gpt") : mbr && !wholeDisk ? QStringLiteral("dos") : QString();
            if (gpt)
                d.tableId = win::guidString(info->Gpt.DiskId);
            else if (mbr && !wholeDisk)
                d.tableId = QStringLiteral("%1").arg(uint(info->Mbr.Signature), 8, 16, QLatin1Char('0'));
            if (wholeDisk) {
                Volume v;
                v.objectPath = d.blockPath;
                v.device = d.device;
                v.size = d.size;
                addVolume(v, 0);
            }
            for (DWORD i = 0; i < info->PartitionCount && !wholeDisk; ++i) {
                const PARTITION_INFORMATION_EX &p = info->PartitionEntry[i];
                if (p.PartitionLength.QuadPart <= 0)
                    continue;
                Volume v;
                v.offset = quint64(p.StartingOffset.QuadPart);
                v.size = quint64(p.PartitionLength.QuadPart);
                v.number = int(p.PartitionNumber);
                if (gpt) {
                    v.partType = win::guidString(p.Gpt.PartitionType);
                    v.partUuid = win::guidString(p.Gpt.PartitionId);
                    v.partName = win::fromWide(p.Gpt.Name, int(wcsnlen(p.Gpt.Name, 36)));
                    v.partFlags = p.Gpt.Attributes;
                    v.isEfi = v.partType == kEfiType;
                } else {
                    const BYTE type = p.Mbr.PartitionType;
                    if (type == PARTITION_ENTRY_UNUSED)
                        continue;
                    // The first four entries are the MBR's own; after them come the logical
                    // partitions, each with the link to the next one, which isn't shown.
                    v.isContainer = isContainerType(type);
                    if (v.isContainer && i >= 4)
                        continue;
                    v.isContained = i >= 4;
                    v.partType = QStringLiteral("0x%1").arg(uint(type), 2, 16, QLatin1Char('0'));
                    v.partFlags = p.Mbr.BootIndicator ? 0x80 : 0;
                    v.partUuid = QStringLiteral("%1-%2").arg(uint(info->Mbr.Signature), 8, 16, QLatin1Char('0')).arg(v.number, 2, 10, QLatin1Char('0'));
                    v.isEfi = type == 0xef;
                }
                v.objectPath = v.isContainer ? QStringLiteral("%1\\extended@%2").arg(d.blockPath).arg(v.offset)
                                             : partitionPath(drive.number, p.PartitionNumber);
                v.device = QStringLiteral("Disk %1 partition %2").arg(drive.number).arg(v.number);
                addVolume(v, v.offset);
            }
        } else if (disk.valid()) {
            // No table at all; there may still be a file system on the whole drive.
            Volume v;
            v.objectPath = d.blockPath;
            v.device = d.device;
            v.size = d.size;
            const fsprobe::Result r = probeAt(0, d.size, QStringLiteral("%1/0/%2").arg(d.drivePath).arg(d.size));
            if (r.found())
                addVolume(v, 0);
        }
        std::sort(d.volumes.begin(), d.volumes.end(), [](const Volume &a, const Volume &b) { return a.offset < b.offset; });
        if (d.isSystem) {
            // The EFI partition Windows started from is on the same drive.
            for (Volume &v : d.volumes) {
                if (v.isEfi)
                    v.isSystem = true;
            }
        }
        disks.push_back(d);
    }
    m_windows->probed = probed;

    m_disks = disks;
    m_jobs = m_testJobs;
    for (Disk &d : m_disks) {
        if (m_testHealth.contains(d.blockPath))
            d.health = m_testHealth.value(d.blockPath);
    }
    emit changed();
}

QString UDisks::daemonVersion() const
{
    return QSysInfo::prettyProductName();
}

void UDisks::notOnWindows(const QString &failure)
{
    QTimer::singleShot(0, this, [this, failure] {
        emit operationFinished(false, failure + QStringLiteral(": ") + tr("this isn't in the Windows version of DiskForge yet"));
    });
}

void UDisks::unmountThen(const QVector<Volume> &volumes, const QString &failure, const std::function<void()> &then, bool,
                         const std::function<void()> &failed)
{
    // Taking the letters away is what unmounting is on Windows; the volume itself goes
    // offline when it's locked for writing.
    for (const Volume &v : volumes) {
        for (const QString &mp : v.mountPoints) {
            const std::wstring path = win::wide(QDir::toNativeSeparators(mp.endsWith(QLatin1Char('/')) ? mp : mp + QLatin1Char('/')));
            if (!DeleteVolumeMountPointW(path.c_str())) {
                const QString error = win::lastErrorText();
                QTimer::singleShot(0, this, [this, failure, error, failed] {
                    emit operationFinished(false, failure + QStringLiteral(": ") + error);
                    if (failed)
                        failed();
                });
                return;
            }
        }
    }
    QTimer::singleShot(0, this, then);
}

void UDisks::mount(const Volume &volume, bool readOnly)
{
    Q_UNUSED(readOnly) // a letter doesn't make a volume read-only on Windows
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't mount %1").arg(name);
    const QString volumeName = m_windows->volumeNames.value(volume.objectPath);
    if (volumeName.isEmpty()) {
        QTimer::singleShot(0, this, [this, failure] { emit operationFinished(false, failure + QStringLiteral(": ") + tr("Windows can't read what's on it")); });
        return;
    }
    // The first free letter from D on, the way Windows picks one.
    const DWORD used = GetLogicalDrives();
    QChar letter;
    for (int i = 3; i < 26 && letter.isNull(); ++i) {
        if (!(used & (1u << i)))
            letter = QChar(u'A' + i);
    }
    if (letter.isNull()) {
        QTimer::singleShot(0, this, [this, failure] { emit operationFinished(false, failure + QStringLiteral(": ") + tr("every drive letter is taken")); });
        return;
    }
    const QString root = QStringLiteral("%1:\\").arg(letter);
    const bool ok = SetVolumeMountPointW(win::wide(root).c_str(), win::wide(volumeName).c_str());
    const QString error = ok ? QString() : win::lastErrorText();
    QTimer::singleShot(0, this, [this, ok, name, root, failure, error] {
        refresh();
        emit operationFinished(ok, ok ? tr("Mounted %1 at %2").arg(name, root) : failure + QStringLiteral(": ") + error);
    });
}

void UDisks::unmount(const Volume &volume)
{
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't unmount %1").arg(name);
    if (refuseSystem(diskOf(volume), failure))
        return;
    unmountThen({volume}, failure, [this, name] {
        refresh();
        emit operationFinished(true, tr("Unmounted %1").arg(name));
    });
}

void UDisks::setLabel(const Volume &volume, const QString &label)
{
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't rename %1").arg(name);
    if (refuseSystem(diskOf(volume), failure))
        return;
    const QString volumeName = m_windows->volumeNames.value(volume.objectPath);
    if (volumeName.isEmpty())
        return notOnWindows(failure);
    const bool ok = SetVolumeLabelW(win::wide(volumeName).c_str(), win::wide(label).c_str());
    const QString error = ok ? QString() : win::lastErrorText();
    QTimer::singleShot(0, this, [this, ok, name, label, failure, error] {
        refresh();
        emit operationFinished(ok, ok ? tr("Renamed %1 to \"%2\"").arg(name, label) : failure + QStringLiteral(": ") + error);
    });
}

void UDisks::openBlock(const QString &objectPath, OpenMode mode)
{
    const Disk *disk = nullptr;
    QString name = objectPath;
    for (const Disk &d : m_disks) {
        if (d.blockPath == objectPath) {
            disk = &d;
            name = shortDevice(d.device);
        }
        for (const Volume &v : d.volumes) {
            if (v.objectPath == objectPath) {
                disk = &d;
                name = shortDevice(v.device);
            }
        }
    }
    const QString failure = tr("Couldn't open %1").arg(name);
    auto fail = [this, objectPath, failure](const QString &why) {
        QTimer::singleShot(0, this, [this, objectPath, failure, why] {
            emit operationFinished(false, failure + QStringLiteral(": ") + why);
            emit deviceOpened(objectPath, -1);
        });
    };
    if (!disk)
        return fail(tr("it isn't there anymore"));
    const bool writes = mode == OpenMode::ReadWrite || mode == OpenMode::ReadWriteDirect || mode == OpenMode::BenchmarkWritable;
    if (writes)
        return fail(tr("writing to drives isn't in the Windows version of DiskForge yet"));

    // Windows reads drives and partitions straight from the drive anyway, never through the
    // cache, so every reading mode is the same here.
    win::Handle h = win::openDevice(objectPath, GENERIC_READ, FILE_FLAG_NO_BUFFERING);
    if (!h.valid())
        return fail(win::lastErrorText());
    const int fd = _open_osfhandle(intptr_t(h.get()), _O_RDONLY | _O_BINARY);
    if (fd < 0)
        return fail(tr("too many files open"));
    h.release();
    qCInfo(lcOps).noquote() << "open" << objectPath << "mode" << int(mode) << "done";
    QTimer::singleShot(0, this, [this, objectPath, fd] { emit deviceOpened(objectPath, fd); });
}

void UDisks::rescan(const QString &objectPath)
{
    QString path = objectPath;
    for (const Disk &d : m_disks) {
        for (const Volume &v : d.volumes) {
            if (v.objectPath == objectPath)
                path = d.blockPath;
        }
    }
    const win::Handle h = win::openDevice(path, GENERIC_READ | GENERIC_WRITE);
    if (h.valid())
        win::ioctl(h.get(), IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0);
    m_debounce.start();
}

// Not on Windows yet. Each reports so, the way a failed operation does.

void UDisks::setPartitionTypeAndFlags(const Volume &volume, const QString &, bool, quint64)
{
    notOnWindows(tr("Couldn't change the type of %1").arg(shortDevice(volume.device)));
}

// A file system's serial number, written into its boot sectors with the volume locked, so
// nothing reads it halfway. Windows mounts it again by itself afterwards.
void UDisks::setUuid(const Volume &volume, const QString &uuid)
{
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't give %1 a new ID").arg(name);
    const Disk *disk = diskOf(volume);
    if (refuseSystem(disk, failure))
        return;
    const qint64 sector = disk ? std::max(512, disk->sectorSize) : 512;
    QString path = m_windows->volumeNames.value(volume.objectPath);
    if (path.endsWith(QLatin1Char('\\')))
        path.chop(1);
    if (path.isEmpty())
        path = volume.objectPath;
    QString error;
    win::Handle h = win::openDevice(path, GENERIC_READ | GENERIC_WRITE);
    auto at = [&h](qint64 offset) {
        LARGE_INTEGER to;
        to.QuadPart = offset;
        return SetFilePointerEx(h.get(), to, nullptr, FILE_BEGIN);
    };
    auto readAt = [&](qint64 offset, QByteArray &buffer) {
        DWORD done = 0;
        return at(offset) && ReadFile(h.get(), buffer.data(), DWORD(buffer.size()), &done, nullptr) && done == DWORD(buffer.size());
    };
    auto writeAt = [&](qint64 offset, const QByteArray &buffer) {
        DWORD done = 0;
        return at(offset) && WriteFile(h.get(), buffer.constData(), DWORD(buffer.size()), &done, nullptr) && done == DWORD(buffer.size());
    };
    bool ok = h.valid();
    if (!ok) {
        error = win::lastErrorText();
    } else if (!win::ioctl(h.get(), FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0)) {
        ok = false;
        error = tr("something has files open on it; close them and try again");
    }
    if (ok) {
        win::ioctl(h.get(), FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0);
        QByteArray boot(newids::bootBytes(volume.fsType, int(sector)), '\0');
        ok = readAt(0, boot) && newids::setSerial(boot, volume.fsType, uuid, &error) && writeAt(0, boot);
        if (ok && volume.fsType == QLatin1String("ntfs")) {
            // NTFS keeps a copy of its boot sector in the volume's last sector.
            GET_LENGTH_INFORMATION length = {};
            QByteArray last(sector, '\0');
            QString ignored;
            if (win::ioctl(h.get(), IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &length, sizeof length) && length.Length.QuadPart > sector
                && readAt(length.Length.QuadPart - sector, last) && newids::setSerial(last, volume.fsType, uuid, &ignored))
                writeAt(length.Length.QuadPart - sector, last);
        }
        if (!ok && error.isEmpty())
            error = win::lastErrorText();
        FlushFileBuffers(h.get());
        win::ioctl(h.get(), FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0);
    }
    h.reset();
    forgetProbes(disk);
    QTimer::singleShot(0, this, [this, ok, name, failure, error] {
        refresh();
        emit operationFinished(ok, ok ? tr("Gave %1 a new ID").arg(name) : failure + QStringLiteral(": ") + error);
    });
}

namespace {

// IOCTL_DISK_SET_DISK_ATTRIBUTES and its SET_DISK_ATTRIBUTES, from ntddisk.h, which MinGW's
// headers don't have.
constexpr DWORD kSetDiskAttributes = CTL_CODE(IOCTL_DISK_BASE, 0x003d, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS);
constexpr DWORDLONG kDiskOffline = 0x1;
struct SetDiskAttributes {
    DWORD version;
    BOOLEAN persist;
    BYTE reserved1[3];
    DWORDLONG attributes;
    DWORDLONG mask;
    DWORD reserved2[4];
};

} // namespace

void UDisks::giveNewDiskIds(const Disk &disk)
{
    const QString name = shortDevice(disk.device);
    const QString failure = tr("Couldn't give %1 new IDs").arg(name);
    if (refuseSystem(&disk, failure))
        return;
    QString error;
    win::Handle h = win::openDevice(disk.blockPath, GENERIC_READ | GENERIC_WRITE);
    QByteArray layout = h.valid() ? win::ioctl(h.get(), IOCTL_DISK_GET_DRIVE_LAYOUT_EX) : QByteArray();
    bool ok = layout.size() >= qsizetype(sizeof(DRIVE_LAYOUT_INFORMATION_EX));
    if (ok) {
        auto *info = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX *>(layout.data());
        if (info->PartitionStyle == PARTITION_STYLE_GPT) {
            CoCreateGuid(&info->Gpt.DiskId);
            for (DWORD i = 0; i < info->PartitionCount; ++i) {
                PARTITION_INFORMATION_EX &p = info->PartitionEntry[i];
                CoCreateGuid(&p.Gpt.PartitionId);
                p.RewritePartition = TRUE;
            }
        } else if (info->PartitionStyle == PARTITION_STYLE_MBR) {
            DWORD signature = 0;
            while (signature == 0) // 0 means "none"
                signature = QRandomGenerator::system()->generate();
            info->Mbr.Signature = signature;
            for (DWORD i = 0; i < info->PartitionCount; ++i)
                info->PartitionEntry[i].RewritePartition = TRUE;
        } else {
            ok = false;
            error = tr("it has no partition table");
        }
        const DWORD size = DWORD(offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) + info->PartitionCount * sizeof(PARTITION_INFORMATION_EX));
        if (ok && !win::ioctl(h.get(), IOCTL_DISK_SET_DRIVE_LAYOUT_EX, info, size, nullptr, 0)) {
            ok = false;
            error = win::lastErrorText();
        }
        if (ok) {
            win::ioctl(h.get(), IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0);
            // Windows switched it off for having the same IDs as another drive: on again.
            SetDiskAttributes attributes = {};
            attributes.version = sizeof attributes;
            attributes.persist = TRUE;
            attributes.mask = kDiskOffline;
            win::ioctl(h.get(), kSetDiskAttributes, &attributes, sizeof attributes, nullptr, 0);
        }
    } else {
        error = win::lastErrorText();
    }
    h.reset();
    forgetProbes(&disk);
    QTimer::singleShot(0, this, [this, ok, name, failure, error] {
        refresh();
        emit operationFinished(ok, ok ? tr("Gave %1 new IDs").arg(name) : failure + QStringLiteral(": ") + error);
    });
}

// What was read from a drive's partitions is read again after DiskForge changed them.
void UDisks::forgetProbes(const Disk *disk)
{
    if (!disk)
        return;
    const QString prefix = disk->drivePath + QLatin1Char('/');
    m_windows->probed.removeIf([&prefix](const QHash<QString, fsprobe::Result>::iterator it) { return it.key().startsWith(prefix); });
}

void UDisks::format(const Volume &volume, const QString &, const QString &, const QString &)
{
    notOnWindows(tr("Couldn't format %1").arg(shortDevice(volume.device)));
}

void UDisks::deletePartition(const Volume &volume)
{
    notOnWindows(tr("Couldn't delete %1").arg(shortDevice(volume.device)));
}

void UDisks::createPartition(const Disk &disk, quint64, quint64, const QString &, const QString &, const QString &)
{
    notOnWindows(tr("Couldn't create a partition on %1").arg(shortDevice(disk.device)));
}

void UDisks::createPartitionTable(const Disk &disk, const QString &)
{
    notOnWindows(tr("Couldn't create a partition table on %1").arg(shortDevice(disk.device)));
}

void UDisks::resize(const Volume &volume, quint64)
{
    notOnWindows(tr("Couldn't resize %1").arg(shortDevice(volume.device)));
}

ResizeLimits UDisks::resizeLimits(const Volume &) const
{
    ResizeLimits limits;
    limits.reason = tr("this isn't in the Windows version of DiskForge yet");
    return limits;
}

bool UDisks::resizeNeedsRemount(const Volume &, bool) const
{
    return false;
}

void UDisks::powerOff(const Disk &disk)
{
    notOnWindows(tr("Couldn't power off %1").arg(shortDevice(disk.device)));
}

void UDisks::check(const Volume &volume)
{
    notOnWindows(tr("Couldn't check %1").arg(shortDevice(volume.device)));
}

void UDisks::repair(const Volume &volume)
{
    notOnWindows(tr("Couldn't repair %1").arg(shortDevice(volume.device)));
}

void UDisks::setMountAtStartup(const Volume &volume, bool)
{
    notOnWindows(tr("Couldn't change how %1 is mounted").arg(shortDevice(volume.device)));
}

QString UDisks::startupMountPoint(const Volume &)
{
    return {};
}

void UDisks::openImage(const QString &path, bool)
{
    notOnWindows(tr("Couldn't open %1").arg(QDir::toNativeSeparators(path)));
}

void UDisks::detachImage(const Disk &disk)
{
    notOnWindows(tr("Couldn't close %1").arg(shortDevice(disk.device)));
}

void UDisks::unlock(const Volume &volume, const QString &)
{
    notOnWindows(tr("Couldn't unlock %1").arg(shortDevice(volume.device)));
}

void UDisks::lock(const Volume &volume)
{
    notOnWindows(tr("Couldn't lock %1").arg(shortDevice(volume.device)));
}

void UDisks::changePassphrase(const Volume &volume, const QString &, const QString &)
{
    notOnWindows(tr("Couldn't change the passphrase of %1").arg(shortDevice(volume.device)));
}

void UDisks::wipe(const Disk &disk)
{
    notOnWindows(tr("Couldn't wipe %1").arg(shortDevice(disk.device)));
}

void UDisks::secureErase(const Disk &disk, EraseMethod, int)
{
    notOnWindows(tr("Couldn't erase %1").arg(shortDevice(disk.device)));
}

void UDisks::prepareUnplug(const Disk &disk, const std::function<void(bool ready)> &done)
{
    notOnWindows(tr("Couldn't get %1 ready to unplug").arg(shortDevice(disk.device)));
    QTimer::singleShot(0, this, [done] { done(false); });
}

void UDisks::smartUpdate(const Disk &)
{
    m_debounce.start();
}

void UDisks::smartSelftest(const Disk &disk, const QString &)
{
    notOnWindows(tr("Couldn't start a self-test on %1").arg(shortDevice(disk.device)));
}

void UDisks::smartSetEnabled(const Disk &disk, bool)
{
    notOnWindows(tr("Couldn't change SMART on %1").arg(shortDevice(disk.device)));
}

void UDisks::smartSelftestAbort(const Disk &disk)
{
    notOnWindows(tr("Couldn't stop the self-test on %1").arg(shortDevice(disk.device)));
}

void UDisks::raidSyncAction(const Disk &array, const QString &)
{
    notOnWindows(tr("Couldn't check %1").arg(shortDevice(array.device)));
}

void UDisks::refreshHealthQuietly(const Disk &)
{
    m_debounce.start();
}

void UDisks::setPowerSettings(const Disk &disk, const QVariantMap &)
{
    notOnWindows(tr("Couldn't change the power settings of %1").arg(shortDevice(disk.device)));
}

void UDisks::sleepNow(const Disk &disk)
{
    notOnWindows(tr("Couldn't put %1 to sleep").arg(shortDevice(disk.device)));
}

int UDisks::powerState(const Disk &)
{
    return -1;
}

void UDisks::cancelJob(const QString &, const QString &)
{
    QTimer::singleShot(0, this, [this] { emit jobStopFailed(tr("It had already finished.")); });
}

QVector<SmartAttribute> UDisks::smartAttributes(const Disk &)
{
    return {};
}
