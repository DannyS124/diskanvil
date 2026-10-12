// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// UDisks on Windows: the same class, built on Windows' own disk API instead of UDisks2.
//
// Drives come from SetupAPI's list of disk devices and are opened as \\.\PhysicalDriveN; the
// partition table from IOCTL_DISK_GET_DRIVE_LAYOUT_EX; what's in each partition from the
// volumes Windows mounted (letters, labels, free space), and from DiskAnvil's own look at
// the first bytes for file systems Windows can't read (ext4, LUKS...). Windows tells about
// drives and volumes coming and going through CM_Register_Notification; drive letters given
// or taken away are noticed by watching GetLogicalDrives().
//
// Everything runs as administrator (the manifest asks for it), which raw access and the
// partition table need.

#include "udisks.h"

#include "applog.h"
#include "blockio.h"
#include "fat32format.h"
#include "format.h"
#include "fsprobe.h"
#include "health.h"
#include "newids.h"
#include "smartdata.h"
#include "winutil.h"

#include <QCollator>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include <fcntl.h>
#include <io.h>

#include <windows.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <virtdisk.h>
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

// What a storage cmdlet said, without its trimmings: the sentence after "Extended information",
// when there's one, and no "Activity ID".
void storage(QObject *owner, const QString &script, const QHash<QString, QString> &values, const std::function<void(bool, const QString &)> &done)
{
    win::powerShell(owner, script, values, done);
}

// What comes between "Couldn't lock J:" and why: no second colon after a drive letter.
QString reasonAfter(const QString &failure)
{
    return failure.endsWith(QLatin1Char(':')) ? QStringLiteral(" ") : QStringLiteral(": ");
}

int diskNumber(const QString &blockPath)
{
    return blockPath.section(QStringLiteral("PhysicalDrive"), 1).toInt();
}

// Windows' name for a file system DiskAnvil knows by its Linux one; empty when Windows can't make it.
QString windowsFs(const QString &fsType)
{
    if (fsType == QLatin1String("ntfs"))
        return QStringLiteral("NTFS");
    if (fsType == QLatin1String("vfat"))
        return QStringLiteral("FAT32");
    if (fsType == QLatin1String("exfat"))
        return QStringLiteral("exFAT");
    return {};
}

// What DiskAnvil Live can do and Windows can't, in the words for it.
QString onlyOnLive(const QString &fsName)
{
    return QCoreApplication::translate("UDisks", "Windows can't make %1 file systems; DiskAnvil Live can").arg(fsName);
}

constexpr quint64 kFat32Most = 32ull * 1024 * 1024 * 1024; // Windows makes FAT32 only this big


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

// IOCTL_DISK_GET_DISK_ATTRIBUTES and its answer, the same.
constexpr DWORD kGetDiskAttributes = CTL_CODE(IOCTL_DISK_BASE, 0x003c, METHOD_BUFFERED, FILE_ANY_ACCESS);
struct GetDiskAttributes {
    DWORD version;
    DWORD reserved1;
    DWORDLONG attributes;
};

bool isOffline(const QString &blockPath)
{
    const win::Handle h = win::openDevice(blockPath, GENERIC_READ);
    GetDiskAttributes attributes = {};
    return h.valid() && win::ioctl(h.get(), kGetDiskAttributes, nullptr, 0, &attributes, sizeof attributes)
        && (attributes.attributes & kDiskOffline);
}

// Locks volumes and takes them out of use, for as long as the handles in `locks` are kept:
// writing over them through the drive needs that, and reading them so nothing changes halfway.
// With `force`, something with files open on one (a File Explorer window) gets them taken away:
// for erasing the whole drive, which was confirmed. The device of one that can't be locked, if any.
QString lockVolumes(const QVector<Volume> &volumes, const QHash<QString, QString> &volumeNames, std::vector<win::Handle> &locks, bool force)
{
    for (const Volume &v : volumes) {
        QString path = volumeNames.value(v.objectPath);
        if (path.endsWith(QLatin1Char('\\')))
            path.chop(1);
        win::Handle volume = path.isEmpty() ? win::Handle() : win::openDevice(path, GENERIC_READ | GENERIC_WRITE);
        if (!volume.valid())
            continue;
        // Something like File Explorer can have it open for a moment.
        bool locked = false;
        for (int tries = 0; tries < (force ? 6 : 5) && !locked; ++tries) {
            if (tries == 5)
                win::ioctl(volume.get(), FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0);
            locked = win::ioctl(volume.get(), FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0) || GetLastError() == ERROR_INVALID_FUNCTION;
            if (!locked && tries < 4)
                Sleep(200);
        }
        if (!locked)
            return v.device;
        win::ioctl(volume.get(), FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0);
        locks.push_back(std::move(volume));
    }
    return {};
}

// Takes a drive offline (Windows lets go of everything on it, and leaves it alone) or back
// online; only until the PC restarts.
bool setOffline(const QString &blockPath, bool offline)
{
    const win::Handle h = win::openDevice(blockPath, GENERIC_READ | GENERIC_WRITE);
    SetDiskAttributes attributes = {};
    attributes.version = sizeof attributes;
    attributes.persist = FALSE;
    attributes.attributes = offline ? kDiskOffline : 0;
    attributes.mask = kDiskOffline;
    return h.valid() && win::ioctl(h.get(), kSetDiskAttributes, &attributes, sizeof attributes, nullptr, 0);
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
            continue; // spread over several drives (dynamic disks): not something DiskAnvil edits
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

// What a drive says about its health, as it hands it over: the SMART pages of an ATA drive (and
// whether it says it's failing), or an NVMe drive's health log and its identify page.
struct RawHealth {
    qint64 when = 0; // seconds since 1970
    bool read = false, ata = false, nvme = false, failing = false, asleep = false, smartOff = false;
    QByteArray data, thresholds, log, identify, selftest;
};

// NVMe's Device Self-test, through Windows' pass-through for NVMe admin commands: `code` 1 starts
// a short one, 2 an extended one, 0xF stops the one running. For all of the drive (namespace ID
// all ones). False with GetLastError when Windows or the drive turned it down.
bool nvmeSelftestCommand(HANDLE drive, DWORD code)
{
    constexpr DWORD kCommand = STORAGE_PROTOCOL_COMMAND_LENGTH_NVME;
    const DWORD size = DWORD(FIELD_OFFSET(STORAGE_PROTOCOL_COMMAND, Command)) + kCommand;
    QByteArray buffer(int(size), '\0');
    auto *c = reinterpret_cast<STORAGE_PROTOCOL_COMMAND *>(buffer.data());
    c->Version = STORAGE_PROTOCOL_STRUCTURE_VERSION;
    c->Length = sizeof(STORAGE_PROTOCOL_COMMAND);
    c->ProtocolType = ProtocolTypeNvme;
    c->Flags = STORAGE_PROTOCOL_COMMAND_FLAG_ADAPTER_REQUEST;
    c->CommandLength = kCommand;
    c->TimeOutValue = 30;
    c->CommandSpecific = STORAGE_PROTOCOL_SPECIFIC_NVME_ADMIN_COMMAND;
    // The 64-byte NVMe command: the opcode in dword 0, the namespace in 1, the test in 10.
    auto *command = reinterpret_cast<uchar *>(c->Command);
    command[0] = 0x14;
    qToLittleEndian<quint32>(0xFFFFFFFF, command + 4);
    qToLittleEndian<quint32>(quint32(code), command + 40);
    DWORD returned = 0;
    if (!DeviceIoControl(drive, IOCTL_STORAGE_PROTOCOL_COMMAND, buffer.data(), size, buffer.data(), size, &returned, nullptr))
        return false;
    if (c->ReturnStatus != STORAGE_PROTOCOL_STATUS_SUCCESS) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }
    return true;
}

// An ATA SMART command through Windows' own SMART calls: a 512-byte page back, or with `regs`
// the registers the drive answered with (how it says it's failing).
bool smartCommand(HANDLE drive, BYTE command, BYTE feature, BYTE sector, QByteArray *page, IDEREGS *regs = nullptr)
{
    SENDCMDINPARAMS in {};
    in.cBufferSize = page ? 512 : 0;
    in.irDriveRegs.bFeaturesReg = feature;
    in.irDriveRegs.bSectorCountReg = 1;
    in.irDriveRegs.bSectorNumberReg = sector;
    in.irDriveRegs.bCylLowReg = command == SMART_CMD ? SMART_CYL_LOW : 0;
    in.irDriveRegs.bCylHighReg = command == SMART_CMD ? SMART_CYL_HI : 0;
    in.irDriveRegs.bDriveHeadReg = 0xA0;
    in.irDriveRegs.bCommandReg = command;
    QByteArray out(qsizetype(sizeof(SENDCMDOUTPARAMS) + 512), '\0');
    DWORD got = 0;
    if (!DeviceIoControl(drive, page ? SMART_RCV_DRIVE_DATA : SMART_SEND_DRIVE_COMMAND, &in, sizeof in - 1, out.data(), DWORD(out.size()), &got, nullptr))
        return false;
    const auto *o = reinterpret_cast<const SENDCMDOUTPARAMS *>(out.constData());
    if (o->DriverStatus.bDriverError)
        return false;
    if (page)
        *page = QByteArray(reinterpret_cast<const char *>(o->bBuffer), 512);
    if (regs)
        memcpy(regs, o->bBuffer, sizeof(IDEREGS));
    return true;
}

// An NVMe page through the storage stack's protocol queries: the health log, or identify.
QByteArray nvmeQuery(HANDLE drive, STORAGE_PROPERTY_ID property, STORAGE_PROTOCOL_NVME_DATA_TYPE type, DWORD value, DWORD length)
{
    const qsizetype header = qsizetype(offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters) + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA));
    QByteArray buffer(header + qsizetype(length), '\0');
    auto *query = reinterpret_cast<STORAGE_PROPERTY_QUERY *>(buffer.data());
    auto *data = reinterpret_cast<STORAGE_PROTOCOL_SPECIFIC_DATA *>(query->AdditionalParameters);
    query->PropertyId = property;
    query->QueryType = PropertyStandardQuery;
    data->ProtocolType = ProtocolTypeNvme;
    data->DataType = type;
    data->ProtocolDataRequestValue = value;
    data->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    data->ProtocolDataLength = length;
    DWORD got = 0;
    if (!DeviceIoControl(drive, IOCTL_STORAGE_QUERY_PROPERTY, buffer.data(), DWORD(buffer.size()), buffer.data(), DWORD(buffer.size()), &got, nullptr))
        return {};
    const auto *descriptor = reinterpret_cast<const STORAGE_PROTOCOL_DATA_DESCRIPTOR *>(buffer.constData());
    const STORAGE_PROTOCOL_SPECIFIC_DATA &answer = descriptor->ProtocolSpecificData;
    const qsizetype at = qsizetype(offsetof(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData) + answer.ProtocolDataOffset);
    if (answer.ProtocolDataLength < length || at + qsizetype(length) > buffer.size())
        return {};
    return buffer.mid(at, qsizetype(length));
}

// Not from a drive that's asleep: asking would wake it.
RawHealth readRawHealth(const QString &blockPath, bool nvme)
{
    RawHealth r;
    const win::Handle drive = win::openDevice(blockPath, GENERIC_READ | GENERIC_WRITE);
    if (!drive.valid())
        return r;
    BOOL on = TRUE;
    if (GetDevicePowerState(drive.get(), &on) && !on) {
        r.asleep = true;
        return r;
    }
    if (nvme) {
        constexpr DWORD kHealthLog = 2, kIdentifyController = 1;
        r.log = nvmeQuery(drive.get(), StorageAdapterProtocolSpecificProperty, NVMeDataTypeLogPage, kHealthLog, 512);
        if (r.log.isEmpty())
            r.log = nvmeQuery(drive.get(), StorageDeviceProtocolSpecificProperty, NVMeDataTypeLogPage, kHealthLog, 512);
        r.identify = nvmeQuery(drive.get(), StorageAdapterProtocolSpecificProperty, NVMeDataTypeIdentify, kIdentifyController, 4096);
        if (smart::nvmeCanSelftest(r.identify)) {
            constexpr DWORD kSelftestLog = 6;
            r.selftest = nvmeQuery(drive.get(), StorageAdapterProtocolSpecificProperty, NVMeDataTypeLogPage, kSelftestLog, 564);
        }
        r.nvme = r.read = !r.log.isEmpty();
        return r;
    }
    if (!smartCommand(drive.get(), SMART_CMD, READ_ATTRIBUTES, 1, &r.data)) {
        // Can it do SMART, switched off? IDENTIFY says: word 82 bit 0 it can, word 85 bit 0 it's on.
        QByteArray identify;
        if (smartCommand(drive.get(), ID_CMD, 0, 1, &identify)) {
            const auto *w = reinterpret_cast<const uchar *>(identify.constData());
            r.smartOff = (w[164] & 1) && !(w[170] & 1);
        }
        return r;
    }
    smartCommand(drive.get(), SMART_CMD, READ_THRESHOLDS, 1, &r.thresholds);
    IDEREGS answer {};
    if (smartCommand(drive.get(), SMART_CMD, RETURN_SMART_STATUS, 0, nullptr, &answer))
        r.failing = answer.bCylLowReg == 0xF4 && answer.bCylHighReg == 0x2C;
    r.ata = r.read = true;
    return r;
}

// What a drive's raw health data means: the same verdict as Linux reaches from UDisks.
Health healthFrom(const RawHealth &r, const Disk &d)
{
    Health h = d.health; // its key, and the temperature Windows already says
    h.updated = quint64(r.when);
    if (r.ata) {
        const QVector<SmartAttribute> attributes = smart::ataAttributes(r.data, r.thresholds);
        auto raw = [&attributes](int id) {
            for (const SmartAttribute &a : attributes) {
                if (a.id == id)
                    return qMax<qint64>(0, a.rawValue);
            }
            return qint64(attributes.isEmpty() ? -1 : 0);
        };
        for (const SmartAttribute &a : attributes) {
            if ((a.id == 194 || (a.id == 190 && h.temperatureC < 0)) && a.rawValue > 0)
                h.temperatureC = double(a.rawValue);
            if (a.id == 9 && a.rawValue >= 0)
                h.powerOnHours = quint64(a.rawValue);
        }
        h.reallocatedSectors = raw(5);
        h.pendingSectors = raw(197);
        h.badSectors = attributes.isEmpty() ? -1 : h.reallocatedSectors + h.pendingSectors;
        const smart::AtaSelftest test = smart::ataSelftest(r.data);
        h.selftestStatus = test.status;
        h.selftestPercentRemaining = test.percentRemaining;
        health::remember(h.key, attributes);
        health::AtaInput in;
        in.driveSaysFailing = r.failing;
        in.failingNow = int(std::count_if(attributes.cbegin(), attributes.cend(), [](const SmartAttribute &a) { return a.failing; }));
        in.failedBefore = int(std::count_if(attributes.cbegin(), attributes.cend(), [](const SmartAttribute &a) {
            return a.threshold > 0 && a.worst > 0 && a.worst <= a.threshold;
        }));
        in.attributes = attributes;
        in.seen = health::seen(h.key);
        in.temperatureC = h.temperatureC;
        in.ssd = d.rotationRate == 0;
        const health::Verdict v = health::ata(in);
        h.state = v.state;
        h.summary = v.summary;
        h.reasons = v.reasons;
        h.lifeLeft = v.lifeLeft;
        return h;
    }
    const smart::NvmeHealth n = smart::nvmeHealth(r.log);
    h.nvme = true;
    if (n.temperatureC > -1)
        h.temperatureC = n.temperatureC;
    h.powerOnHours = n.powerOnHours;
    h.criticalWarnings = n.criticalWarnings;
    h.percentUsed = n.percentUsed;
    smart::nvmeTemperatureLimits(r.identify, &h.warningTempC, &h.criticalTempC);
    h.nvmeSelftest = smart::nvmeCanSelftest(r.identify);
    const smart::NvmeSelftest test = smart::nvmeSelftest(r.selftest);
    h.selftestStatus = test.status;
    h.selftestPercentRemaining = test.percentRemaining;
    health::NvmeInput in;
    in.criticalWarnings = n.criticalWarnings;
    in.availableSpare = n.availableSpare;
    in.spareThreshold = n.spareThreshold;
    in.percentUsed = n.percentUsed;
    in.mediaErrors = qint64(n.mediaErrors);
    in.temperatureC = h.temperatureC;
    in.warningTempC = h.warningTempC;
    const health::Verdict v = health::nvme(in);
    h.state = v.state;
    h.summary = v.summary;
    h.reasons = v.reasons;
    h.lifeLeft = h.percentUsed >= 0 ? qMax(0, 100 - h.percentUsed) : -1;
    return h;
}

// The file a virtual disk (a .vhd or .vhdx Windows opened) is kept in, by the volume it's on and
// where on it.
QString imageOf(HANDLE disk)
{
    QByteArray buffer(16384, '\0');
    auto *info = reinterpret_cast<STORAGE_DEPENDENCY_INFO *>(buffer.data());
    info->Version = STORAGE_DEPENDENCY_INFO_VERSION_2;
    ULONG used = 0;
    const auto flags = GET_STORAGE_DEPENDENCY_FLAG(GET_STORAGE_DEPENDENCY_FLAG_HOST_VOLUMES | GET_STORAGE_DEPENDENCY_FLAG_DISK_HANDLE);
    if (GetStorageDependencyInformation(disk, flags, ULONG(buffer.size()), info, &used) != ERROR_SUCCESS || info->NumberEntries == 0)
        return {};
    const STORAGE_DEPENDENCY_INFO_TYPE_2 &e = info->Version2Entries[0];
    if (!e.HostVolumeName || !e.DependentVolumeRelativePath)
        return {};
    // Windows names the volume without the \\?\ and the closing backslash its letter is looked up by.
    QString volume = QString::fromWCharArray(e.HostVolumeName);
    if (volume.startsWith(QLatin1String("Volume{")))
        volume.prepend(QStringLiteral("\\\\?\\"));
    if (!volume.endsWith(QLatin1Char('\\')))
        volume += QLatin1Char('\\');
    QString root = volume;
    wchar_t names[1024] = {};
    DWORD length = 0;
    if (GetVolumePathNamesForVolumeNameW(win::wide(volume).c_str(), names, 1024, &length) && names[0])
        root = QString::fromWCharArray(names);
    QString relative = QString::fromWCharArray(e.DependentVolumeRelativePath);
    while (relative.startsWith(QLatin1Char('\\')))
        relative.remove(0, 1);
    return QDir::fromNativeSeparators(root + relative);
}

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
    // What DiskAnvil found at the start of partitions Windows can't read, so a refresh
    // doesn't read every drive again (or wake a sleeping one). By drive, offset and size.
    QHash<QString, fsprobe::Result> probed;
    // Each partition's volume, \\?\Volume{...}\, for the ones Windows mounted.
    QHash<QString, QString> volumeNames;
    // DiskAnvil's own long jobs (wiping), shown like UDisks' ones; how to stop each, what to say
    // when it's been stopped, and the threads doing them.
    QVector<Job> jobs;
    QHash<QString, std::shared_ptr<std::atomic<bool>>> stops;
    // What each drive's SMART or health log said last, by disk; and the ones to read again soon.
    QHash<QString, RawHealth> health;
    QSet<QString> healthStale;
    QHash<QString, QString> stopMessages;
    QHash<QString, erasure::Record> erasures; // wipes under way, by drive: their records
    QList<QThread *> workers;

    ~Windows()
    {
        // A wipe still going is stopped, and waited for: its thread reports back to UDisks.
        for (const auto &stop : std::as_const(stops))
            *stop = true;
        for (QThread *t : std::as_const(workers))
            t->wait(10000);
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
        m_error = tr("DiskAnvil needs to run as administrator to see and change drives. Right-click it and choose "
                     "\"Run as administrator\".");
    detectFilesystems();
    refresh();
}

void UDisks::detectFilesystems()
{
    // Windows formats these itself. FAT32 only up to 32 GB; the rest can't be made here.
    // Windows grows and shrinks NTFS (in use or not), not FAT or exFAT; chkdsk checks and repairs all three.
    const QString linuxOnly = tr("Windows can't make this; DiskAnvil Live can");
    const int both = OfflineShrink | OfflineGrow | OnlineShrink | OnlineGrow;
    m_filesystems = {
        {QStringLiteral("exfat"), tr("exFAT"), tr("USB drives, works everywhere"), 15, {}, true, 0, false, true, true},
        {QStringLiteral("vfat"), tr("FAT32"), tr("old devices, max 4 GB per file"), 11, {}, true, 0, false, true, true},
        {QStringLiteral("ntfs"), tr("NTFS"), tr("Windows"), 32, {}, true, both, true, true, true},
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
                d.sdCard = true;
                break;
            case BusTypeFileBackedVirtual:
                d.isLoop = true;
                break;
            default:
                break;
            }
        }
        d.canPowerOff = d.bus == QLatin1String("usb");
        // USB card readers name themselves ("Generic- SD/MMC", "Card Reader", "microSD").
        static const QRegularExpression reader(QStringLiteral("\\bSD(HC|XC)?\\b|SD/MMC|micro ?SD|card ?reader"), QRegularExpression::CaseInsensitiveOption);
        if (d.bus == QLatin1String("usb") && reader.match(d.model).hasMatch())
            d.sdCard = true;
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

        // Health, straight from the drive: SMART for an ATA one, the health log for an NVMe one.
        // Read again every ten minutes, or when asked; not on every refresh.
        if (!d.isLoop && d.bus != QLatin1String("usb") && d.bus != QLatin1String("sd")) {
            RawHealth &raw = m_windows->health[d.blockPath];
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            if (raw.when == 0 || now - raw.when > 600 || m_windows->healthStale.remove(d.blockPath)) {
                RawHealth fresh = readRawHealth(d.blockPath, d.bus == QLatin1String("nvme"));
                if (!fresh.asleep) {
                    raw = fresh;
                    raw.when = now;
                }
            }
            if (raw.read)
                d.health = healthFrom(raw, d);
            else if (raw.smartOff) {
                d.health.smartOff = true;
                d.health.summary = tr("SMART is turned off");
            }
        } else if (d.bus == QLatin1String("usb") && !d.removable) {
            d.health.usbNoData = true; // most USB boxes don't pass it through
        }
        health::noteSelftestStatus(d.health.key, d.health.selftestStatus);

        // The partition table takes read access.
        const win::Handle disk = win::openDevice(d.blockPath, GENERIC_READ);
        if (d.isLoop && disk.valid())
            d.backingFile = imageOf(disk.get());
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
                        // BitLocker that Windows can't read is locked; unlocked, it's NTFS to Windows.
                        v.encrypted = r.type == QLatin1String("BitLocker");
                    } else if (r.type == QLatin1String("BitLocker")) {
                        // Unlocked BitLocker: Windows reads the file system inside, the drive holds
                        // BitLocker. The volume is its own cleartext side, the way LUKS has one.
                        v.cleartextPath = v.objectPath;
                        v.cleartextDevice = v.device;
                        v.cleartextFsType = v.fsType;
                        v.cleartextHasFilesystem = v.hasFilesystem;
                        v.cleartextMountPoints = v.mountPoints;
                        v.fsType = r.type;
                        v.fsUsage = r.usage;
                        v.encrypted = true;
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
    m_jobs = m_testJobs + m_windows->jobs;
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
        emit operationFinished(false, failure + reasonAfter(failure) + tr("this isn't in the Windows version of DiskAnvil yet"));
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
                    emit operationFinished(false, failure + reasonAfter(failure) + error);
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
        QTimer::singleShot(0, this, [this, failure] { emit operationFinished(false, failure + reasonAfter(failure) + tr("Windows can't read what's on it")); });
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
        QTimer::singleShot(0, this, [this, failure] { emit operationFinished(false, failure + reasonAfter(failure) + tr("every drive letter is taken")); });
        return;
    }
    const QString root = QStringLiteral("%1:\\").arg(letter);
    const bool ok = SetVolumeMountPointW(win::wide(root).c_str(), win::wide(volumeName).c_str());
    const QString error = ok ? QString() : win::lastErrorText();
    QTimer::singleShot(0, this, [this, ok, name, root, failure, error] {
        refresh();
        emit operationFinished(ok, ok ? tr("Mounted %1 at %2").arg(name, root) : failure + reasonAfter(failure) + error);
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
        emit operationFinished(ok, ok ? tr("Renamed %1 to \"%2\"").arg(name, label) : failure + reasonAfter(failure) + error);
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
            emit operationFinished(false, failure + reasonAfter(failure) + why);
            emit deviceOpened(objectPath, -1);
        });
    };
    if (!disk)
        return fail(tr("it isn't there anymore"));
    const bool writes = mode == OpenMode::ReadWrite || mode == OpenMode::ReadWriteDirect || mode == OpenMode::BenchmarkWritable;
    if (writes) {
        if (disk->isSystem)
            return fail(disk->systemReason.isEmpty() ? tr("Windows is running from it") : disk->systemReason);
        const DWORD flags = FILE_FLAG_NO_BUFFERING | (mode == OpenMode::ReadWriteDirect ? FILE_FLAG_WRITE_THROUGH : 0);
        const bool whole = objectPath == disk->blockPath;
        bool offline = false;
        auto locks = std::make_shared<std::vector<win::Handle>>();
        win::Handle h;
        if (whole) {
            // A whole drive: offline, Windows lets go of everything on it and leaves it alone,
            // until it's back online when what was opened here is closed. USB sticks can't go
            // offline: their volumes are locked and taken out of use instead, for as long.
            offline = setOffline(disk->blockPath, true) && isOffline(disk->blockPath);
            if (!offline) {
                if (const QString busy = lockVolumes(disk->volumes, m_windows->volumeNames, *locks, true); !busy.isEmpty())
                    return fail(tr("something has files open on %1; close them and try again").arg(shortDevice(busy)));
            }
            h = win::openDevice(objectPath, GENERIC_READ | GENERIC_WRITE, flags);
            if (!h.valid()) {
                const QString why = win::lastErrorText();
                if (offline)
                    setOffline(disk->blockPath, false);
                return fail(why);
            }
        } else {
            // A partition: its volume locked and taken out of use for as long as it's open.
            QString path = m_windows->volumeNames.value(objectPath);
            if (path.endsWith(QLatin1Char('\\')))
                path.chop(1);
            h = win::openDevice(path.isEmpty() ? objectPath : path, GENERIC_READ | GENERIC_WRITE, flags);
            if (!h.valid())
                return fail(win::lastErrorText());
            if (!win::ioctl(h.get(), FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0) && GetLastError() != ERROR_INVALID_FUNCTION)
                return fail(tr("something has files open on it; close them and try again"));
            win::ioctl(h.get(), FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0);
        }
        const int fd = _open_osfhandle(intptr_t(h.get()), _O_RDWR | _O_BINARY);
        if (fd < 0) {
            if (offline)
                setOffline(disk->blockPath, false);
            return fail(tr("too many files open"));
        }
        h.release();
        // Closed (blockio::close), the drive comes back, and Windows reads what's on it now.
        blockio::whenClosed(fd, [this, path = disk->blockPath, offline, locks] {
            QMetaObject::invokeMethod(this, [this, path, offline, locks] {
                locks->clear();
                if (offline)
                    setOffline(path, false);
                forgetProbes(diskByPath(path));
                // Quietly: whoever wrote to it may be waiting for an answer of its own.
                if (const win::Handle disk = win::openDevice(path, GENERIC_READ | GENERIC_WRITE); disk.valid())
                    win::ioctl(disk.get(), IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0);
                refresh();
            }, Qt::QueuedConnection);
        });
        qCInfo(lcOps).noquote() << "open" << objectPath << "mode" << int(mode) << "for writing";
        QTimer::singleShot(0, this, [this, objectPath, fd] { emit deviceOpened(objectPath, fd); });
        return;
    }

    // Windows reads drives and partitions straight from the drive anyway, never through the
    // cache. Read (a backup, a clone's source) keeps what's on it still while it's read, the way
    // Linux unmounts it first: its volumes are locked for as long as it's open. Benchmark reads
    // it as it is.
    auto locks = std::make_shared<std::vector<win::Handle>>();
    if (mode == OpenMode::Read) {
        if (disk->isSystem)
            return fail(disk->systemReason.isEmpty() ? tr("Windows is running from it") : disk->systemReason);
        QVector<Volume> volumes;
        for (const Volume &v : disk->volumes) {
            if (objectPath == disk->blockPath || v.objectPath == objectPath)
                volumes << v;
        }
        if (const QString busy = lockVolumes(volumes, m_windows->volumeNames, *locks, false); !busy.isEmpty())
            return fail(tr("something has files open on %1; close them and try again").arg(shortDevice(busy)));
    }
    win::Handle h = win::openDevice(objectPath, GENERIC_READ, FILE_FLAG_NO_BUFFERING);
    if (!h.valid())
        return fail(win::lastErrorText());
    const int fd = _open_osfhandle(intptr_t(h.get()), _O_RDONLY | _O_BINARY);
    if (fd < 0)
        return fail(tr("too many files open"));
    h.release();
    if (!locks->empty()) {
        // Closed (blockio::close), the volumes are let go and Windows mounts them again.
        blockio::whenClosed(fd, [this, locks] {
            QMetaObject::invokeMethod(this, [this, locks] {
                locks->clear();
                refresh();
            }, Qt::QueuedConnection);
        });
    }
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
    const bool ok = h.valid() && win::ioctl(h.get(), IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0);
    const QString why = ok ? QString() : win::lastErrorText();
    m_debounce.start();
    // Answered like every operation (Clone and Restore wait for it), with nothing to say when it worked.
    QTimer::singleShot(0, this, [this, ok, why] {
        emit operationFinished(ok, ok ? QString() : tr("Couldn't re-read the partition table") + QStringLiteral(": ") + why);
    });
}

// Not on Windows yet. Each reports so, the way a failed operation does.

void UDisks::setPartitionTypeAndFlags(const Volume &volume, const QString &type, bool setFlags, quint64 flags)
{
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't change the type and flags of %1").arg(name);
    const Disk *disk = diskOf(volume);
    if (refuseSystem(disk, failure))
        return;
    auto refuse = [this, failure](const QString &why) {
        QTimer::singleShot(0, this, [this, failure, why] { emit operationFinished(false, failure + reasonAfter(failure) + why); });
    };
    if (!disk || volume.number == 0)
        return refuse(tr("it isn't a partition"));
    const bool mbr = disk->tableType == QLatin1String("dos");
    // The type, and an MBR partition's active flag, through Windows' own Set-Partition.
    QString script;
    QHash<QString, QString> values = {{QStringLiteral("DF_DISK"), QString::number(diskNumber(disk->blockPath))},
                                      {QStringLiteral("DF_PART"), QString::number(volume.number)}};
    const QString partition = QStringLiteral("Set-Partition -DiskNumber ([int]$env:DF_DISK) -PartitionNumber ([int]$env:DF_PART)");
    if (!type.isEmpty()) {
        bool hex = false;
        const uint code = QString(type).remove(QStringLiteral("0x")).toUInt(&hex, 16);
        if (mbr && (!hex || code > 0xFF))
            return refuse(tr("%1 isn't a partition type").arg(type));
        script += partition + (mbr ? QStringLiteral(" -MbrType ([byte]$env:DF_TYPE); ") : QStringLiteral(" -GptType $env:DF_TYPE; "));
        values.insert(QStringLiteral("DF_TYPE"), mbr ? QString::number(code) : QStringLiteral("{%1}").arg(type));
    }
    if (setFlags && mbr) {
        script += partition + QStringLiteral(" -IsActive ([bool][int]$env:DF_ACTIVE); ");
        values.insert(QStringLiteral("DF_ACTIVE"), (flags & 0x80) ? QStringLiteral("1") : QStringLiteral("0"));
    }
    const bool gptFlags = setFlags && !mbr;
    auto finish = [this, path = disk->blockPath, partitionPath = volume.objectPath, gptFlags, flags, name, failure, setFlags](bool ok, const QString &said) {
        QString why = said;
        if (ok && gptFlags) {
            // A GPT partition's attributes, set on the partition itself (its type, ID and name kept).
            const win::Handle h = win::openDevice(partitionPath, GENERIC_READ | GENERIC_WRITE);
            PARTITION_INFORMATION_EX info {};
            ok = h.valid() && win::ioctl(h.get(), IOCTL_DISK_GET_PARTITION_INFO_EX, nullptr, 0, &info, sizeof info)
              && info.PartitionStyle == PARTITION_STYLE_GPT;
            if (ok) {
                SET_PARTITION_INFORMATION_EX set {};
                set.PartitionStyle = PARTITION_STYLE_GPT;
                set.Gpt = info.Gpt;
                set.Gpt.Attributes = flags;
                ok = win::ioctl(h.get(), IOCTL_DISK_SET_PARTITION_INFO_EX, &set, sizeof set, nullptr, 0);
            }
            if (!ok)
                why = win::lastErrorText();
        }
        forgetProbes(diskByPath(path));
        refresh();
        emit operationFinished(ok, !ok ? failure + reasonAfter(failure) + why
                                   : setFlags ? tr("Changed the type and flags of %1").arg(name) : tr("Changed the type of %1").arg(name));
    };
    if (script.isEmpty()) {
        QTimer::singleShot(0, this, [finish] { finish(true, {}); });
        return;
    }
    storage(this, script, values, finish);
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
        emit operationFinished(ok, ok ? tr("Gave %1 a new ID").arg(name) : failure + reasonAfter(failure) + error);
    });
}

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
        emit operationFinished(ok, ok ? tr("Gave %1 new IDs").arg(name) : failure + reasonAfter(failure) + error);
    });
}

// What was read from a drive's partitions is read again after DiskAnvil changed them.
void UDisks::forgetProbes(const Disk *disk)
{
    if (!disk)
        return;
    const QString prefix = disk->drivePath + QLatin1Char('/');
    m_windows->probed.removeIf([&prefix](const QHash<QString, fsprobe::Result>::iterator it) { return it.key().startsWith(prefix); });
}

void UDisks::createPartitionTable(const Disk &disk, const QString &tableType)
{
    const QString name = shortDevice(disk.device);
    const QString failure = tr("Couldn't initialize %1").arg(name);
    if (refuseSystem(&disk, failure))
        return;
    const QString style = tableType == QLatin1String("gpt") ? QStringLiteral("GPT") : QStringLiteral("MBR");
    if (disk.removable) {
        // A USB stick: to Windows' Clear-Disk and Initialize-Disk a blank one is one partition
        // that's always there, so the empty table is written here (the way Rufus does it too).
        QString why;
        std::vector<win::Handle> locks;
        if (const QString busy = lockVolumes(disk.volumes, m_windows->volumeNames, locks, true); !busy.isEmpty()) {
            why = tr("something has files open on %1; close them and try again").arg(shortDevice(busy));
        } else if (win::Handle h = win::openDevice(disk.blockPath, GENERIC_READ | GENERIC_WRITE); !h.valid()) {
            why = win::lastErrorText();
        } else {
            // What was at both ends goes first (old tables, file systems' backups), as Clear-Disk does.
            constexpr DWORD kEnd = 1024 * 1024;
            blockio::Buffer zeros = blockio::alignedBuffer(kEnd);
            memset(zeros.get(), 0, kEnd);
            for (const quint64 at : {quint64(0), disk.size > 2 * kEnd ? disk.size - kEnd : quint64(0)}) {
                OVERLAPPED where = {};
                where.Offset = DWORD(at);
                where.OffsetHigh = DWORD(at >> 32);
                DWORD done = 0;
                if (!WriteFile(h.get(), zeros.get(), kEnd, &done, &where) && why.isEmpty())
                    why = win::lastErrorText();
            }
            CREATE_DISK create = {};
            if (style == QLatin1String("GPT")) {
                create.PartitionStyle = PARTITION_STYLE_GPT;
                create.Gpt.DiskId = QUuid::createUuid();
                create.Gpt.MaxPartitionCount = 128;
            } else {
                create.PartitionStyle = PARTITION_STYLE_MBR;
                create.Mbr.Signature = QRandomGenerator::system()->generate() | 1;
            }
            if (why.isEmpty() && !win::ioctl(h.get(), IOCTL_DISK_CREATE_DISK, &create, sizeof create, nullptr, 0))
                why = win::lastErrorText();
            win::ioctl(h.get(), IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0);
        }
        locks.clear();
        forgetProbes(diskByPath(disk.blockPath));
        QTimer::singleShot(0, this, [this, name, failure, style, why] {
            refresh();
            emit operationFinished(why.isEmpty(), why.isEmpty() ? tr("%1 now has an empty %2 partition table").arg(name, style) : failure + reasonAfter(failure) + why);
        });
        return;
    }
    // A drive with a table is emptied first, the way Disk Management's "clean" does.
    storage(this,
            QStringLiteral("$n = [int]$env:DF_DISK; $d = Get-Disk -Number $n; "
                           "if ($d.IsOffline) { Set-Disk -Number $n -IsOffline $false }; if ($d.IsReadOnly) { Set-Disk -Number $n -IsReadOnly $false }; "
                           "if ($d.PartitionStyle -ne 'RAW') { Clear-Disk -Number $n -RemoveData -RemoveOEM -Confirm:$false }; "
                           "Initialize-Disk -Number $n -PartitionStyle $env:DF_STYLE"),
            {{QStringLiteral("DF_DISK"), QString::number(diskNumber(disk.blockPath))}, {QStringLiteral("DF_STYLE"), style}},
            [this, path = disk.blockPath, name, failure, style](bool ok, const QString &said) {
                forgetProbes(diskByPath(path));
                refresh();
                // Windows puts its small reserved partition on every GPT drive it sets up.
                emit operationFinished(ok, !ok ? failure + reasonAfter(failure) + said
                                           : style == QLatin1String("GPT") ? tr("%1 now has a GPT partition table, with the small reserved partition Windows keeps on every GPT drive").arg(name)
                                                                           : tr("%1 now has an empty %2 partition table").arg(name, style));
            });
}

void UDisks::createPartition(const Disk &disk, quint64 offset, quint64 size, const QString &fsType, const QString &label,
                             const QString &passphrase, bool besideSystem)
{
    const QString name = shortDevice(disk.device);
    const QString failure = tr("Couldn't create a partition on %1").arg(name);
    if (!besideSystem && refuseSystem(&disk, failure))
        return;
    const FsType *fs = filesystem(fsType);
    const QString windows = windowsFs(fsType);
    auto refuse = [this, failure](const QString &why) {
        QTimer::singleShot(0, this, [this, failure, why] { emit operationFinished(false, failure + reasonAfter(failure) + why); });
    };
    if (!passphrase.isEmpty())
        return refuse(tr("Windows can't make LUKS encrypted partitions; DiskAnvil Live can"));
    if (!fsType.isEmpty() && windows.isEmpty())
        return refuse(onlyOnLive(fs ? fs->name : fsType));

    // 1 MiB aligned, as on Linux; the last MiB kept free for GPT's backup.
    constexpr quint64 MiB = 1024 * 1024;
    quint64 freeEnd = 0;
    for (const Span &s : diskSpans(disk)) {
        if (s.isFree() && offset >= s.offset && offset < s.offset + s.size)
            freeEnd = s.offset + s.size;
    }
    freeEnd = std::min<quint64>(freeEnd, disk.size > MiB ? disk.size - MiB : 0) / MiB * MiB;
    const quint64 start = std::max<quint64>((offset + MiB - 1) / MiB * MiB, MiB);
    const quint64 end = std::min<quint64>(start + (size + MiB - 1) / MiB * MiB, freeEnd);
    if (end <= start)
        return refuse(tr("not enough space"));
    // Windows won't make FAT32 this big: the partition is made bare, DiskAnvil formats it, and only
    // then does it get a letter (as raw, it would have Windows ask to format it).
    const bool ownFormat = windows == QLatin1String("FAT32") && end - start > kFat32Most;

    QString script = QStringLiteral("$p = New-Partition -DiskNumber ([int]$env:DF_DISK) -Offset ([uint64]$env:DF_OFFSET) -Size ([uint64]$env:DF_SIZE)");
    if (disk.tableType == QLatin1String("dos"))
        script += windows == QLatin1String("FAT32") ? QStringLiteral(" -MbrType FAT32") : QStringLiteral(" -MbrType IFS");
    script += QStringLiteral("; ");
    if (ownFormat)
        script += QStringLiteral("Write-Output $p.PartitionNumber");
    else if (!windows.isEmpty())
        script += label.isEmpty() ? QStringLiteral("$p | Format-Volume -FileSystem $env:DF_FS -Confirm:$false -Force | Out-Null; ")
                                  : QStringLiteral("$p | Format-Volume -FileSystem $env:DF_FS -NewFileSystemLabel $env:DF_LABEL -Confirm:$false -Force | Out-Null; ");
    if (!ownFormat)
        script += QStringLiteral("$p | Add-PartitionAccessPath -AssignDriveLetter -ErrorAction SilentlyContinue");
    storage(this, script,
            {{QStringLiteral("DF_DISK"), QString::number(diskNumber(disk.blockPath))}, {QStringLiteral("DF_OFFSET"), QString::number(start)},
             {QStringLiteral("DF_SIZE"), QString::number(end - start)}, {QStringLiteral("DF_FS"), windows}, {QStringLiteral("DF_LABEL"), label}},
            [this, path = disk.blockPath, name, failure, fsName = fs ? fs->name : fsType, start, end, ownFormat, label](bool ok, const QString &said) {
                forgetProbes(diskByPath(path));
                refresh();
                const QString done = tr("Created a %1 %2 partition on %3").arg(formatSize(end - start), fsName, name);
                if (!ok || !ownFormat) {
                    emit operationFinished(ok, ok ? done : failure + reasonAfter(failure) + said);
                    return;
                }
                const Disk *d = diskByPath(path);
                const Volume *made = nullptr;
                for (const Volume &v : d ? d->volumes : QVector<Volume>()) {
                    if (v.number == said.toInt())
                        made = &v;
                }
                if (!made) {
                    emit operationFinished(false, failure + reasonAfter(failure) + tr("the new partition didn't show up"));
                    return;
                }
                ownFat32(*made, label, [this, path, number = made->number, done, failure](bool formatted, const QString &error) {
                    storage(this, QStringLiteral("Add-PartitionAccessPath -DiskNumber ([int]$env:DF_DISK) -PartitionNumber ([int]$env:DF_PART) -AssignDriveLetter"),
                            {{QStringLiteral("DF_DISK"), QString::number(diskNumber(path))}, {QStringLiteral("DF_PART"), QString::number(number)}},
                            [this, path, done, failure, formatted, error](bool, const QString &) {
                                forgetProbes(diskByPath(path));
                                refresh();
                                emit operationFinished(formatted, formatted ? done : failure + reasonAfter(failure) + error);
                            });
                });
            });
}

void UDisks::deletePartition(const Volume &volume, bool besideSystem)
{
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't delete %1").arg(name);
    const Disk *disk = diskOf(volume);
    if (!(besideSystem && !volume.isSystem) && refuseSystem(disk, failure))
        return;
    if (!disk || volume.number == 0) {
        QTimer::singleShot(0, this, [this, failure] { emit operationFinished(false, failure + reasonAfter(failure) + tr("it isn't a partition")); });
        return;
    }
    storage(this, QStringLiteral("Remove-Partition -DiskNumber ([int]$env:DF_DISK) -PartitionNumber ([int]$env:DF_PART) -Confirm:$false"),
            {{QStringLiteral("DF_DISK"), QString::number(diskNumber(disk->blockPath))}, {QStringLiteral("DF_PART"), QString::number(volume.number)}},
            [this, path = disk->blockPath, name, failure](bool ok, const QString &said) {
                forgetProbes(diskByPath(path));
                refresh();
                emit operationFinished(ok, ok ? tr("Deleted %1").arg(name) : failure + reasonAfter(failure) + said);
            });
}

void UDisks::format(const Volume &volume, const QString &fsType, const QString &label, const QString &passphrase)
{
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't format %1").arg(name);
    const Disk *disk = diskOf(volume);
    if (refuseSystem(disk, failure))
        return;
    const FsType *fs = filesystem(fsType);
    const QString windows = windowsFs(fsType);
    auto refuse = [this, failure](const QString &why) {
        QTimer::singleShot(0, this, [this, failure, why] { emit operationFinished(false, failure + reasonAfter(failure) + why); });
    };
    if (!passphrase.isEmpty())
        return refuse(tr("Windows can't make LUKS encrypted partitions; DiskAnvil Live can"));
    if (windows.isEmpty())
        return refuse(onlyOnLive(fs ? fs->name : fsType));
    if (windows == QLatin1String("FAT32") && volume.size > kFat32Most) {
        // Windows won't make FAT32 this big; DiskAnvil's own formatter will.
        ownFat32(volume, label, [this, path = disk ? disk->blockPath : QString(), name, failure](bool ok, const QString &error) {
            forgetProbes(diskByPath(path));
            refresh();
            emit operationFinished(ok, ok ? tr("Formatted %1 as %2").arg(name, QStringLiteral("FAT32")) : failure + reasonAfter(failure) + error);
        });
        return;
    }
    // A partition by its number; a file system on the whole drive (no table) by its volume.
    const QString volumeName = m_windows->volumeNames.value(volume.objectPath);
    QString target;
    if (disk && volume.number > 0)
        target = QStringLiteral("(Get-Partition -DiskNumber ([int]$env:DF_DISK) -PartitionNumber ([int]$env:DF_PART))");
    else if (!volumeName.isEmpty())
        target = QStringLiteral("(Get-Volume -Path $env:DF_VOLUME)");
    else
        return refuse(tr("Windows can't reach it to format it"));
    const QString script = target + (label.isEmpty() ? QStringLiteral(" | Format-Volume -FileSystem $env:DF_FS -Confirm:$false -Force | Out-Null")
                                                     : QStringLiteral(" | Format-Volume -FileSystem $env:DF_FS -NewFileSystemLabel $env:DF_LABEL "
                                                                      "-Confirm:$false -Force | Out-Null"));
    storage(this, script,
            {{QStringLiteral("DF_DISK"), disk ? QString::number(diskNumber(disk->blockPath)) : QString()},
             {QStringLiteral("DF_PART"), QString::number(volume.number)}, {QStringLiteral("DF_VOLUME"), volumeName},
             {QStringLiteral("DF_FS"), windows}, {QStringLiteral("DF_LABEL"), label}},
            [this, path = disk ? disk->blockPath : QString(), name, failure, fsName = fs ? fs->name : fsType](bool ok, const QString &said) {
                forgetProbes(diskByPath(path));
                refresh();
                emit operationFinished(ok, ok ? tr("Formatted %1 as %2").arg(name, fsName) : failure + reasonAfter(failure) + said);
            });
}

// FAT32 made by DiskAnvil's own formatter (fat32format.cpp), where Windows won't: over 32 GB. The
// volume is locked and taken out of use, written through, and let go; Windows reads it afresh.
// It's on a thread of its own, since a big volume's FATs take a while to write.
void UDisks::ownFat32(const Volume &volume, const QString &label, const std::function<void(bool, const QString &)> &done)
{
    const Disk *disk = diskOf(volume);
    QString path = m_windows->volumeNames.value(volume.objectPath);
    if (path.endsWith(QLatin1Char('\\')))
        path.chop(1);
    if (path.isEmpty())
        path = volume.objectPath;
    fat32::Options options;
    options.label = label;
    options.sectorSize = disk ? std::max(512, disk->sectorSize) : 512;
    options.hiddenSectors = volume.offset / quint64(options.sectorSize);
    const bool mbr = disk && disk->tableType == QLatin1String("dos");
    QThread *thread = QThread::create([this, path, size = volume.size, options, done, mbr, number = volume.number, diskPath = disk ? disk->blockPath : QString()] {
        QString error;
        win::Handle h = win::openDevice(path, GENERIC_READ | GENERIC_WRITE);
        bool ok = h.valid();
        if (!ok)
            error = win::lastErrorText();
        else if (!win::ioctl(h.get(), FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0) && GetLastError() != ERROR_INVALID_FUNCTION) {
            ok = false; // (a partition with no volume on it can't be locked, nor needs to be)
            error = tr("something has files open on it; close them and try again");
        }
        if (ok) {
            win::ioctl(h.get(), FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0);
            const int fd = _open_osfhandle(intptr_t(h.release()), _O_RDWR | _O_BINARY);
            ok = fd >= 0 && fat32::format(fd, 0, size, options, &error);
            if (fd >= 0) {
                win::ioctl(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0);
                _close(fd);
            }
        }
        QMetaObject::invokeMethod(this, [this, ok, error, done, mbr, number, diskPath] {
            if (!ok || !mbr)
                return done(ok, error);
            // On an MBR drive, its type says FAT32 too, for cameras and old PCs that look.
            storage(this, QStringLiteral("Set-Partition -DiskNumber ([int]$env:DF_DISK) -PartitionNumber ([int]$env:DF_PART) -MbrType FAT32"),
                    {{QStringLiteral("DF_DISK"), QString::number(diskNumber(diskPath))}, {QStringLiteral("DF_PART"), QString::number(number)}},
                    [done](bool, const QString &) { done(true, {}); });
        }, Qt::QueuedConnection);
    });
    m_windows->workers << thread;
    connect(thread, &QThread::finished, this, [this, thread] {
        m_windows->workers.removeOne(thread);
        thread->deleteLater();
    });
    thread->start();
}

void UDisks::resizeLimitsThen(const Volume &volume, QObject *context, const std::function<void(const ResizeLimits &)> &then)
{
    ResizeLimits limits = resizeLimits(volume);
    const Disk *disk = diskOf(volume);
    if (!limits.possible || !disk || volume.number == 0) {
        QTimer::singleShot(0, context, [limits, then] { then(limits); });
        return;
    }
    // Windows' own answer: NTFS can't shrink past the files it can't move.
    storage(context,
            QStringLiteral("$s = Get-PartitionSupportedSize -DiskNumber ([int]$env:DF_DISK) -PartitionNumber ([int]$env:DF_PART); "
                           "Write-Output \"$($s.SizeMin) $($s.SizeMax)\""),
            {{QStringLiteral("DF_DISK"), QString::number(diskNumber(disk->blockPath))}, {QStringLiteral("DF_PART"), QString::number(volume.number)}},
            [limits, then, size = volume.size](bool ok, const QString &said) mutable {
                constexpr quint64 MiB = 1024 * 1024;
                const QStringList sizes = said.split(QLatin1Char(' '));
                if (ok && sizes.size() == 2) {
                    const quint64 least = (sizes[0].toULongLong() + MiB - 1) / MiB * MiB, most = sizes[1].toULongLong() / MiB * MiB;
                    limits.minSize = std::min(size, std::max(limits.minSize, least));
                    if (most >= size)
                        limits.maxSize = std::min(limits.maxSize, most);
                    limits.possible = limits.minSize < size || limits.maxSize > size;
                    if (!limits.possible)
                        limits.reason = QCoreApplication::translate("UDisks", "Windows can't move what's at its end, and there's no unallocated space after it");
                }
                then(limits);
            });
}

void UDisks::resize(const Volume &volume, quint64 newSize)
{
    constexpr quint64 MiB = 1024 * 1024;
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't resize %1").arg(name);
    const Disk *disk = diskOf(volume);
    if (refuseSystem(disk, failure))
        return;
    const ResizeLimits limits = resizeLimits(volume);
    newSize = newSize / MiB * MiB;
    if (!disk || !limits.possible || newSize < limits.minSize || newSize > limits.maxSize || newSize == volume.size) {
        const QString why = limits.possible ? tr("size out of range") : limits.reason;
        QTimer::singleShot(0, this, [this, failure, why] { emit operationFinished(false, failure + reasonAfter(failure) + why); });
        return;
    }
    // Windows moves NTFS's end along with the partition's, either way.
    storage(this, QStringLiteral("Resize-Partition -DiskNumber ([int]$env:DF_DISK) -PartitionNumber ([int]$env:DF_PART) -Size ([uint64]$env:DF_SIZE)"),
            {{QStringLiteral("DF_DISK"), QString::number(diskNumber(disk->blockPath))}, {QStringLiteral("DF_PART"), QString::number(volume.number)},
             {QStringLiteral("DF_SIZE"), QString::number(newSize)}},
            [this, path = disk->blockPath, name, failure, newSize](bool ok, const QString &said) {
                forgetProbes(diskByPath(path));
                refresh();
                emit operationFinished(ok, ok ? tr("Resized %1 to %2").arg(name, formatSize(newSize)) : failure + reasonAfter(failure) + said);
            });
}

// chkdsk, by its exit code (its words are in the console's own language): 0 nothing wrong, 1
// fixed, 2 tidied up (or would have been, asked to), 3 errors left.
void UDisks::check(const Volume &volume)
{
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't check %1").arg(name);
    if (refuseSystem(diskOf(volume), failure))
        return;
    const QString target = chkdskTarget(volume);
    if (target.isEmpty()) {
        QTimer::singleShot(0, this, [this, failure] { emit operationFinished(false, failure + reasonAfter(failure) + tr("no file system Windows can check")); });
        return;
    }
    auto *chkdsk = new QProcess(this);
    connect(chkdsk, &QProcess::finished, this, [this, chkdsk, id = volume.objectPath, name, failure](int code, QProcess::ExitStatus status) {
        chkdsk->deleteLater();
        if (status != QProcess::NormalExit) {
            emit operationFinished(false, failure);
            return;
        }
        const bool clean = code == 0;
        emit checkFinished(id, clean);
        emit operationFinished(true, clean ? tr("No problems found on %1").arg(name) : tr("%1 has errors").arg(name));
    });
    chkdsk->start(QStringLiteral("chkdsk.exe"), {target});
}

void UDisks::repair(const Volume &volume)
{
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't repair %1").arg(name);
    if (refuseSystem(diskOf(volume), failure))
        return;
    const QString target = chkdskTarget(volume);
    if (target.isEmpty()) {
        QTimer::singleShot(0, this, [this, failure] { emit operationFinished(false, failure + reasonAfter(failure) + tr("no file system Windows can check")); });
        return;
    }
    // /x takes it out of use first, as unmounting does on Linux.
    auto *chkdsk = new QProcess(this);
    connect(chkdsk, &QProcess::finished, this, [this, chkdsk, name, failure](int code, QProcess::ExitStatus status) {
        chkdsk->deleteLater();
        const bool repaired = status == QProcess::NormalExit && code >= 0 && code <= 2;
        refresh();
        emit operationFinished(repaired, repaired ? tr("Repaired %1").arg(name) : tr("Some errors on %1 couldn't be repaired").arg(name));
    });
    chkdsk->start(QStringLiteral("chkdsk.exe"), {target, QStringLiteral("/f"), QStringLiteral("/x")});
}

QString UDisks::chkdskTarget(const Volume &volume) const
{
    // Its letter, or its volume name without the closing backslash, for one without a letter.
    if (!volume.mountPoints.isEmpty() && volume.mountPoints.first().size() <= 3)
        return volume.mountPoints.first().left(2);
    QString name = m_windows->volumeNames.value(volume.objectPath);
    if (name.endsWith(QLatin1Char('\\')))
        name.chop(1);
    return name;
}

void UDisks::setMountAtStartup(const Volume &volume, bool)
{
    notOnWindows(tr("Couldn't change how %1 is mounted").arg(shortDevice(volume.device)));
}

QString UDisks::startupMountPoint(const Volume &)
{
    return {};
}

// BitLocker, through Windows' own Unlock-BitLocker. The password or recovery key goes in an
// environment variable, never on a command line others can see.
void UDisks::unlock(const Volume &volume, const QString &passphrase, const QVariantMap &)
{
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't unlock %1").arg(name);
    if (volume.fsType != QLatin1String("BitLocker"))
        return notOnWindows(failure);
    const QString letter = volume.mountPoints.value(0).left(2);
    if (letter.size() != 2 || letter[1] != QLatin1Char(':')) {
        QTimer::singleShot(0, this, [this, failure] {
            emit operationFinished(false, failure + reasonAfter(failure) + tr("it has no drive letter; mount it first"));
        });
        return;
    }
    static const QRegularExpression recoveryKey(QStringLiteral("^\\d{6}(-\\d{6}){7}$"));
    const bool recovery = recoveryKey.match(passphrase.trimmed()).hasMatch();
    const QString script = recovery ? QStringLiteral("Unlock-BitLocker -MountPoint $env:DF_MOUNT -RecoveryPassword $env:DF_SECRET | Out-Null")
                                    : QStringLiteral("Unlock-BitLocker -MountPoint $env:DF_MOUNT "
                                                     "-Password (ConvertTo-SecureString $env:DF_SECRET -AsPlainText -Force) | Out-Null");
    storage(this, script, {{QStringLiteral("DF_MOUNT"), letter}, {QStringLiteral("DF_SECRET"), recovery ? passphrase.trimmed() : passphrase}},
            [this, name, failure](bool ok, const QString &said) {
                m_windows->probed.clear(); // unlocked, it reads as NTFS now
                refresh();
                // 0x80310027: BitLocker's "the drive cannot be unlocked with the key provided".
                emit operationFinished(ok, ok ? tr("Unlocked %1").arg(name)
                                              : failure + reasonAfter(failure)
                                                    + (said.contains(QLatin1String("0x80310027")) ? tr("that isn't its password or recovery key") : said));
            });
}

// BitLocker's own Lock-BitLocker; something with files open on it is left alone, as on Linux.
void UDisks::lock(const Volume &volume)
{
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't lock %1").arg(name);
    const QString letter = volume.mountPoints.value(0).left(2);
    QString refusal;
    if (volume.fsType != QLatin1String("BitLocker") || volume.cleartextPath.isEmpty())
        refusal = tr("it isn't an unlocked BitLocker drive");
    else if (volume.isSystem)
        refusal = tr("Windows is running from it");
    else if (letter.size() != 2 || letter[1] != QLatin1Char(':'))
        refusal = tr("it has no drive letter; mount it first");
    if (!refusal.isEmpty()) {
        QTimer::singleShot(0, this, [this, failure, refusal] { emit operationFinished(false, failure + reasonAfter(failure) + refusal); });
        return;
    }
    storage(this, QStringLiteral("Lock-BitLocker -MountPoint $env:DF_MOUNT | Out-Null"), {{QStringLiteral("DF_MOUNT"), letter}},
            [this, name, failure](bool ok, const QString &said) {
                refresh();
                emit operationFinished(ok, ok ? tr("Locked %1").arg(name) : failure + reasonAfter(failure) + said);
            });
}

// BitLocker's password, replaced in one go (Win32_EncryptableVolume's ChangePassphrase, what
// manage-bde -changepassword uses: a drive has one password at most), or added when it has none.
// While the drive is unlocked, Windows doesn't ask for the old one; its recovery key keeps
// opening it either way.
void UDisks::changePassphrase(const Volume &volume, const QString &, const QString &newPassphrase)
{
    const QString name = shortDevice(volume.device);
    const QString failure = tr("Couldn't change the password of %1").arg(name);
    const QString letter = volume.mountPoints.value(0).left(2);
    QString refusal;
    if (volume.fsType != QLatin1String("BitLocker"))
        refusal = tr("it isn't a BitLocker drive");
    else if (volume.cleartextPath.isEmpty())
        refusal = tr("unlock it first");
    else if (volume.isSystem)
        refusal = tr("Windows starts from it, and opens it with the PC's own security chip (TPM), not a password");
    else if (letter.size() != 2 || letter[1] != QLatin1Char(':'))
        refusal = tr("it has no drive letter; mount it first");
    if (!refusal.isEmpty()) {
        QTimer::singleShot(0, this, [this, failure, refusal] { emit operationFinished(false, failure + reasonAfter(failure) + refusal); });
        return;
    }
    storage(this,
            QStringLiteral("$v = Get-CimInstance -Namespace root/CIMV2/Security/MicrosoftVolumeEncryption -ClassName Win32_EncryptableVolume "
                           "-Filter \"DriveLetter='$env:DF_MOUNT'\"; "
                           "$ids = @((Invoke-CimMethod -InputObject $v -MethodName GetKeyProtectors -Arguments @{KeyProtectorType = [uint32]8}).VolumeKeyProtectorID); "
                           "if ($ids.Count -eq 0) { Add-BitLockerKeyProtector -MountPoint $env:DF_MOUNT -PasswordProtector "
                           "-Password (ConvertTo-SecureString $env:DF_SECRET -AsPlainText -Force) | Out-Null } "
                           "else { $r = Invoke-CimMethod -InputObject $v -MethodName ChangePassphrase "
                           "-Arguments @{VolumeKeyProtectorID = $ids[0]; NewPassphrase = $env:DF_SECRET}; "
                           "if ($r.ReturnValue -ne 0) { throw ('BitLocker said 0x{0:X8}' -f $r.ReturnValue) } }"),
            {{QStringLiteral("DF_MOUNT"), letter}, {QStringLiteral("DF_SECRET"), newPassphrase}},
            [this, name, failure](bool ok, const QString &said) {
                emit operationFinished(ok, ok ? tr("Changed the password of %1").arg(name) : failure + reasonAfter(failure) + said);
            });
}

void UDisks::secureErase(const Disk &disk, EraseMethod, int)
{
    notOnWindows(tr("Couldn't erase %1").arg(shortDevice(disk.device)));
}

// Windows' own Safely Remove Hardware: it writes out what's waiting and lets go of the drive, or
// says what still uses it. It can wait on programs, so it runs off the window's thread.
void UDisks::powerOff(const Disk &disk)
{
    const QString name = shortDevice(disk.device);
    const QString failure = tr("Couldn't safely remove %1").arg(name);
    if (refuseSystem(&disk, failure))
        return;
    if (!disk.canPowerOff) {
        QTimer::singleShot(0, this, [this, failure] { emit operationFinished(false, failure + reasonAfter(failure) + tr("this drive can't be removed while the PC is on")); });
        return;
    }
    QThread *thread = QThread::create([this, instance = disk.drivePath, name, failure] {
        DEVINST node = 0;
        std::wstring id = win::wide(instance);
        bool ok = false;
        QString why = tr("Windows can't find it anymore");
        if (CM_Locate_DevNodeW(&node, id.data(), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
            // What's removed is the device the disk belongs to: the stick, or the USB box it's in.
            for (int i = 0; i < 6; ++i) {
                ULONG caps = 0, size = sizeof caps;
                if (CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_CAPABILITIES, nullptr, &caps, &size, 0) == CR_SUCCESS && (caps & CM_DEVCAP_REMOVABLE))
                    break;
                DEVINST parent = 0;
                if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS)
                    break;
                node = parent;
            }
            PNP_VETO_TYPE veto = PNP_VetoTypeUnknown;
            wchar_t vetoName[MAX_PATH] = {};
            ok = CM_Request_Device_EjectW(node, &veto, vetoName, MAX_PATH, 0) == CR_SUCCESS && veto == PNP_VetoTypeUnknown;
            why = veto == PNP_VetoOutstandingOpen ? tr("a program still has something open on it. Close it, and any File Explorer window showing "
                                                       "it, then try again")
                                                  : tr("Windows won't let go of it (%1)").arg(QString::fromWCharArray(vetoName));
        }
        QMetaObject::invokeMethod(this, [this, ok, name, failure, why] {
            refresh();
            emit operationFinished(ok, ok ? tr("%1 can be unplugged now").arg(name) : failure + reasonAfter(failure) + why);
        }, Qt::QueuedConnection);
    });
    m_windows->workers << thread;
    connect(thread, &QThread::finished, this, [this, thread] {
        m_windows->workers.removeOne(thread);
        thread->deleteLater();
    });
    thread->start();
}

// Removing it from Windows does all that's needed first, so it's ready as it is.
void UDisks::prepareUnplug(const Disk &, const std::function<void(bool ready)> &done)
{
    QTimer::singleShot(0, this, [done] { done(true); });
}

// Windows itself opens .vhd and .vhdx files as drives, the way File Explorer's "Mount" does.
void UDisks::openImage(const QString &path, bool readOnly)
{
    const QString name = QFileInfo(path).fileName();
    const QString failure = tr("Couldn't open %1").arg(name);
    const QString suffix = QFileInfo(path).suffix().toLower();
    QString refusal;
    if (suffix == QLatin1String("iso"))
        refusal = tr("Windows opens ISO images itself: double-click it in File Explorer and it gets a drive letter");
    else if (suffix != QLatin1String("vhd") && suffix != QLatin1String("vhdx"))
        refusal = tr("Windows only opens .vhd and .vhdx images as drives. Find Lost Files reads the others, and DiskAnvil Live opens them all");
    if (!refusal.isEmpty()) {
        QTimer::singleShot(0, this, [this, failure, refusal] { emit operationFinished(false, failure + reasonAfter(failure) + refusal); });
        return;
    }
    storage(this,
            QStringLiteral("Mount-DiskImage -ImagePath $env:DF_IMAGE -Access $env:DF_ACCESS | Out-Null; "
                           "Write-Output (Get-DiskImage -ImagePath $env:DF_IMAGE | Get-Disk).Number"),
            {{QStringLiteral("DF_IMAGE"), QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath())},
             {QStringLiteral("DF_ACCESS"), readOnly ? QStringLiteral("ReadOnly") : QStringLiteral("ReadWrite")}},
            [this, name, failure](bool ok, const QString &said) {
                bool numbered = false;
                const int number = said.toInt(&numbered);
                refresh();
                if (ok && numbered)
                    emit imageOpened(diskPath(DWORD(number)));
                emit operationFinished(ok, ok ? tr("Opened %1").arg(name) : failure + reasonAfter(failure) + said);
            });
}

void UDisks::openIso(const QString &path, QObject *context, const std::function<void(const QString &, bool, const QString &)> &done)
{
    const QString failure = tr("Couldn't open %1").arg(QFileInfo(path).fileName());
    storage(context,
            QStringLiteral("$here = 0; "
                           "if (-not (Get-DiskImage -ImagePath $env:DF_IMAGE).Attached) { "
                           "  Mount-DiskImage -ImagePath $env:DF_IMAGE -Access ReadOnly -StorageType ISO | Out-Null; $here = 1 }; "
                           "$letter = $null; "
                           "for ($n = 0; $n -lt 20 -and -not $letter; $n++) { "
                           "  $letter = (Get-DiskImage -ImagePath $env:DF_IMAGE | Get-Volume -ErrorAction SilentlyContinue).DriveLetter; "
                           "  if (-not $letter) { Start-Sleep -Milliseconds 500 } }; "
                           "if (-not $letter) { if ($here) { Dismount-DiskImage -ImagePath $env:DF_IMAGE | Out-Null }; Write-Output 'none'; exit 0 }; "
                           "Write-Output \"$here$letter\""),
            {{QStringLiteral("DF_IMAGE"), QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath())}},
            [failure, done](bool ok, const QString &said) {
                if (!ok)
                    return done({}, false, failure + reasonAfter(failure) + said);
                if (said == QLatin1String("none") || said.size() != 2)
                    return done({}, false, failure + reasonAfter(failure) + tr("Windows gave it no drive letter"));
                done(said.mid(1) + QStringLiteral(":/"), said.startsWith(QLatin1Char('1')), {});
            });
}

void UDisks::closeIso(const QString &path, QObject *context, const std::function<void()> &done)
{
    storage(context, QStringLiteral("Dismount-DiskImage -ImagePath $env:DF_IMAGE | Out-Null"),
            {{QStringLiteral("DF_IMAGE"), QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath())}},
            [done](bool, const QString &) { done(); });
}

void UDisks::detachImage(const Disk &disk)
{
    const QString name = !disk.backingFile.isEmpty() ? QFileInfo(disk.backingFile).fileName() : shortDevice(disk.device);
    const QString failure = tr("Couldn't close %1").arg(name);
    if (!disk.isLoop) {
        QTimer::singleShot(0, this, [this, failure] { emit operationFinished(false, failure + reasonAfter(failure) + tr("not a disk image")); });
        return;
    }
    storage(this, QStringLiteral("Dismount-DiskImage -DevicePath $env:DF_DEVICE | Out-Null"),
            {{QStringLiteral("DF_DEVICE"), QStringLiteral("\\\\.\\PHYSICALDRIVE%1").arg(diskNumber(disk.blockPath))}},
            [this, name, failure](bool ok, const QString &said) {
                refresh();
                emit operationFinished(ok, ok ? tr("Closed %1").arg(name) : failure + reasonAfter(failure) + said);
            });
}

void UDisks::wipe(const Disk &disk)
{
    const QString name = shortDevice(disk.device);
    const QString failure = tr("Couldn't wipe %1").arg(name);
    if (refuseSystem(&disk, failure))
        return;
    const int number = diskNumber(disk.blockPath);
    m_windows->erasures.insert(disk.blockPath, erasureOf(disk, tr("Every sector written with zeros (Wipe Disk)"), true));
    // Offline, Windows lets go of everything on the drive, so all of it can be written.
    storage(this, QStringLiteral("Set-Disk -Number ([int]$env:DF_DISK) -IsOffline $true"), {{QStringLiteral("DF_DISK"), QString::number(number)}},
            [this, path = disk.blockPath, size = disk.size, number, name, failure](bool ok, const QString &said) {
                if (!ok) {
                    m_windows->erasures.remove(path);
                    emit operationFinished(false, failure + reasonAfter(failure) + said);
                    return;
                }
                wipeOffline(path, size, number, name, failure);
            });
}

void UDisks::wipeOffline(const QString &path, quint64 size, int number, const QString &name, const QString &failure)
{
    Job job;
    job.path = QStringLiteral("wipe:") + path;
    job.operation = QStringLiteral("format-erase"); // shown as Linux shows UDisks' wiping
    job.objects = {path};
    job.cancelable = true;
    job.progressValid = true;
    job.bytes = size;
    job.started = quint64(QDateTime::currentMSecsSinceEpoch()) * 1000;
    m_windows->jobs.push_back(job);
    auto stop = std::make_shared<std::atomic<bool>>(false);
    m_windows->stops.insert(job.path, stop);
    m_jobs = m_testJobs + m_windows->jobs;
    emit changed();

    QThread *thread = QThread::create([this, path, size, number, name, failure, stop, id = job.path] {
        QString error;
        const win::Handle drive = win::openDevice(path, GENERIC_READ | GENERIC_WRITE, FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH);
        constexpr DWORD kChunk = 4 * 1024 * 1024;
        void *zeros = VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); // page-aligned, and zeros
        quint64 done = 0;
        QElapsedTimer sinceShown;
        sinceShown.start();
        if (!drive.valid() || !zeros)
            error = win::lastErrorText();
        while (error.isEmpty() && done < size && !*stop) {
            const DWORD n = DWORD(std::min<quint64>(kChunk, size - done));
            OVERLAPPED at {};
            at.Offset = DWORD(done & 0xFFFFFFFF);
            at.OffsetHigh = DWORD(done >> 32);
            DWORD written = 0;
            if (!WriteFile(drive.get(), zeros, n, &written, &at) || written != n) {
                error = win::lastErrorText();
                break;
            }
            done += n;
            if (sinceShown.elapsed() > 500) {
                sinceShown.restart();
                QMetaObject::invokeMethod(this, [this, id, done] { showWipe(id, done); }, Qt::QueuedConnection);
            }
        }
        if (drive.valid())
            FlushFileBuffers(drive.get());
        if (zeros)
            VirtualFree(zeros, 0, MEM_RELEASE);
        const bool stopped = *stop;
        QMetaObject::invokeMethod(this, [this, id, path, number, name, failure, error, stopped] {
            const QString stoppedMessage = m_windows->stopMessages.take(id);
            m_windows->stops.remove(id);
            m_windows->jobs.removeIf([&id](const Job &j) { return j.path == id; });
            // Back online, it reads as the empty drive it is now.
            storage(this, QStringLiteral("Set-Disk -Number ([int]$env:DF_DISK) -IsOffline $false"), {{QStringLiteral("DF_DISK"), QString::number(number)}},
                    [this, path, name, failure, error, stopped, stoppedMessage](bool, const QString &) {
                        refresh();
                        erasure::Record record = m_windows->erasures.take(path);
                        if (!error.isEmpty())
                            emit operationFinished(false, failure + reasonAfter(failure) + error);
                        else if (stopped)
                            emit operationFinished(true, stoppedMessage.isEmpty() ? tr("Stopped wiping %1.").arg(name) : stoppedMessage);
                        else {
                            record.finished = QDateTime::currentDateTime();
                            emit erased(record);
                            emit operationFinished(true, tr("%1 has been wiped").arg(name));
                        }
                    });
        }, Qt::QueuedConnection);
    });
    m_windows->workers << thread;
    connect(thread, &QThread::finished, this, [this, thread] {
        m_windows->workers.removeOne(thread);
        thread->deleteLater();
    });
    thread->start();
}

void UDisks::showWipe(const QString &id, quint64 done)
{
    for (Job &j : m_windows->jobs) {
        if (j.path != id)
            continue;
        const quint64 now = quint64(QDateTime::currentMSecsSinceEpoch()) * 1000;
        j.progress = j.bytes ? double(done) / double(j.bytes) : 0;
        j.rate = now > j.started ? quint64(double(done) * 1e6 / double(now - j.started)) : 0;
    }
    m_jobs = m_testJobs + m_windows->jobs;
    emit changed();
}

void UDisks::cancelJob(const QString &jobPath, const QString &stoppedMessage)
{
    const auto stop = m_windows->stops.value(jobPath);
    if (!stop) {
        QTimer::singleShot(0, this, [this] { emit jobStopFailed(tr("It had already finished.")); });
        return;
    }
    m_windows->stopMessages.insert(jobPath, stoppedMessage);
    *stop = true;
}

void UDisks::smartUpdate(const Disk &disk)
{
    m_windows->healthStale.insert(disk.blockPath);
    refresh();
    const Disk *fresh = diskByPath(disk.blockPath);
    const bool ok = fresh && fresh->health.updated > 0;
    emit operationFinished(ok, ok ? tr("Read fresh health data from %1").arg(shortDevice(disk.device))
                                  : tr("Couldn't read health data from %1").arg(shortDevice(disk.device)));
}

void UDisks::refreshHealthQuietly(const Disk &disk)
{
    m_windows->healthStale.insert(disk.blockPath);
    m_debounce.start();
}

// A self-test runs in the drive itself ("off-line"), while it's in use: started and stopped with
// SMART's own command. NVMe ones aren't reached through Windows yet.
void UDisks::smartSelftest(const Disk &disk, const QString &type)
{
    const QString name = shortDevice(disk.device);
    const QString failure = tr("Couldn't start a self-test on %1").arg(name);
    static const QHash<QString, BYTE> kinds = {{QStringLiteral("short"), 1}, {QStringLiteral("extended"), 2}, {QStringLiteral("conveyance"), 3}};
    QString why;
    if (!kinds.contains(type) || (disk.health.nvme && type == QLatin1String("conveyance"))) {
        why = tr("there's no such test");
    } else if (disk.health.nvme) {
        const win::Handle drive = win::openDevice(disk.blockPath, GENERIC_READ | GENERIC_WRITE);
        if (!disk.health.nvmeSelftest)
            why = tr("the drive doesn't do self-tests");
        else if (!drive.valid() || !nvmeSelftestCommand(drive.get(), kinds.value(type)))
            why = win::lastErrorText();
    } else if (!smartSend(disk, EXECUTE_OFFLINE_DIAGS, kinds.value(type))) {
        why = win::lastErrorText();
    }
    finishSmart(disk, why.isEmpty(), why.isEmpty() ? tr("Self-test started on %1").arg(name) : failure + reasonAfter(failure) + why);
}

void UDisks::smartSelftestAbort(const Disk &disk)
{
    const QString name = shortDevice(disk.device);
    bool ok = false;
    if (disk.health.nvme) {
        const win::Handle drive = win::openDevice(disk.blockPath, GENERIC_READ | GENERIC_WRITE);
        ok = drive.valid() && nvmeSelftestCommand(drive.get(), 0xF);
    } else {
        ok = smartSend(disk, EXECUTE_OFFLINE_DIAGS, 0x7F);
    }
    finishSmart(disk, ok, ok ? tr("Stopped the self-test on %1").arg(name) : tr("Couldn't stop the self-test on %1").arg(name));
}

void UDisks::smartSetEnabled(const Disk &disk, bool enabled)
{
    const QString name = shortDevice(disk.device);
    const bool ok = !disk.health.nvme && smartSend(disk, enabled ? ENABLE_SMART : DISABLE_SMART, 0);
    finishSmart(disk, ok, !ok ? tr("Couldn't change SMART on %1").arg(name)
                          : enabled ? tr("SMART is on for %1").arg(name) : tr("SMART is off for %1").arg(name));
}

bool UDisks::smartSend(const Disk &disk, BYTE feature, BYTE sector)
{
    const win::Handle drive = win::openDevice(disk.blockPath, GENERIC_READ | GENERIC_WRITE);
    return drive.valid() && smartCommand(drive.get(), SMART_CMD, feature, sector, nullptr);
}

// The health read again in a moment (a test's progress shows up in it), and the result said.
void UDisks::finishSmart(const Disk &disk, bool ok, const QString &message)
{
    QTimer::singleShot(1500, this, [this, path = disk.blockPath] {
        m_windows->healthStale.insert(path);
        refresh();
    });
    QTimer::singleShot(0, this, [this, ok, message] { emit operationFinished(ok, message); });
}

QVector<SmartAttribute> UDisks::smartAttributes(const Disk &disk)
{
    const auto raw = m_windows->health.constFind(disk.blockPath);
    if (raw == m_windows->health.cend() || !raw->read)
        return {};
    if (raw->ata)
        return smart::ataAttributes(raw->data, raw->thresholds);
    // An NVMe drive's: the same rows Linux shows.
    const smart::NvmeHealth n = smart::nvmeHealth(raw->log);
    QVector<SmartAttribute> out;
    auto add = [&out](const QString &name, const QString &value) { out.push_back({0, name, -1, -1, -1, value, -1, false}); };
    add(tr("Available spare"), QStringLiteral("%1%").arg(n.availableSpare));
    add(tr("Spare threshold"), QStringLiteral("%1%").arg(n.spareThreshold));
    add(tr("Life used"), QStringLiteral("%1%").arg(n.percentUsed));
    add(tr("Data read"), formatSize(n.bytesRead));
    add(tr("Data written"), formatSize(n.bytesWritten));
    add(tr("Power cycles"), QStringLiteral("%L1").arg(n.powerCycles));
    add(tr("Unsafe shutdowns"), QStringLiteral("%L1").arg(n.unsafeShutdowns));
    add(tr("Media errors"), QStringLiteral("%L1").arg(n.mediaErrors));
    add(tr("Error log entries"), QStringLiteral("%L1").arg(n.errorLogEntries));
    add(tr("Minutes above warning temperature"), QStringLiteral("%L1").arg(n.warningTempMinutes));
    add(tr("Minutes above critical temperature"), QStringLiteral("%L1").arg(n.criticalTempMinutes));
    QStringList sensors;
    for (const double c : n.sensorsC)
        sensors << QStringLiteral("%1 °C").arg(qRound(c));
    if (!sensors.isEmpty())
        add(tr("Temperature sensors"), sensors.join(QStringLiteral(", ")));
    double warning = -1, critical = -1;
    smart::nvmeTemperatureLimits(raw->identify, &warning, &critical);
    if (warning > -1 && critical > -1)
        add(tr("Temperature limits"), tr("warning at %1 °C, critical at %2 °C").arg(qRound(warning)).arg(qRound(critical)));
    return out;
}

void UDisks::raidSyncAction(const Disk &array, const QString &)
{
    notOnWindows(tr("Couldn't check %1").arg(shortDevice(array.device)));
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


