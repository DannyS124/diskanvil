// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "writeprotect.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHash>

#ifdef Q_OS_LINUX
#include <cstring>
#include <scsi/sg.h>
#include <sys/ioctl.h>
#endif

#ifdef Q_OS_WIN
#include "winutil.h"

#include <QProcess>
#include <windows.h>
#include <winioctl.h>
#endif

namespace writeprotect {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("writeprotect", text);
}

QString shellQuote(const QString &s)
{
    QString q = s;
    q.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

} // namespace

QVector<Finding> diagnose(const State &s)
{
    QVector<Finding> out;
    // The drive itself first: nothing on the PC can get past it.
    if (s.drivesays == 1 || (s.cardSlot && s.kernelReadOnly)) {
        Finding f;
        if (s.sdCard) {
            f.title = tr("The card's lock switch is on");
            f.text = tr("SD cards have a small switch on their left side. Take the card out, slide the switch up (towards the "
                        "metal contacts), and put it back. If it's up already: the switch can be loose (a little tape over the "
                        "notch holds it), or the reader's sensor is stuck, so try another reader. A card that's locked in every "
                        "reader has locked itself because it's wearing out: copy everything off it and replace it.");
        } else {
            f.title = tr("The drive itself says it's write-protected");
            f.text = tr("If it has a small lock switch, slide it and plug it in again. If it hasn't, the stick has locked itself "
                        "to read-only: flash memory does this when it's wearing out, to keep what's on it safe. No program can "
                        "undo that (tools that claim to send the maker's private commands and often finish the stick off). "
                        "Everything on it can still be read: copy it off, then replace the stick.");
        }
        out << f;
        return out; // the rest can't matter until that's sorted
    }
    if (s.writeBlock) {
        Finding f;
        f.title = tr("DiskAnvil Live's write protection is holding it");
        f.text = tr("Write-Protect Every Drive is on, and this drive isn't one it lets through. Allow writing to just this "
                    "one, or switch it off in Drives and Partitions (Action menu).");
        f.fix = Fix::AllowWriteBlock;
        f.fixLabel = tr("Allow Writing to It");
        out << f;
    }
    if (s.policyWriteProtect) {
        Finding f;
        f.title = tr("Windows is set to write-protect every USB drive");
        f.text = tr("A setting in the registry (StorageDevicePolicies, WriteProtect) makes Windows refuse to write to any USB "
                    "drive. Some \"fix\" tools and some malware leave it on. Switched off, the drive needs to be unplugged "
                    "and plugged in again.");
        f.fix = Fix::ClearPolicy;
        f.fixLabel = tr("Switch It Off");
        out << f;
    }
    if (!s.policyDenyWrite.isEmpty()) {
        Finding f;
        f.title = tr("This PC's administrator doesn't allow writing to removable drives");
        f.text = tr("A group policy says so (%1). On a work or school PC, ask whoever looks after it. On your own PC: "
                    "gpedit.msc, Computer Configuration, Administrative Templates, System, Removable Storage Access, and "
                    "set \"Removable Disks: Deny write access\" to Not Configured (and BitLocker's \"Deny write access to "
                    "removable drives not protected by BitLocker\" under Windows Components, BitLocker Drive Encryption).")
                     .arg(s.policyDenyWrite.join(QStringLiteral(", ")));
        out << f;
    }
    if (s.diskAttributeReadOnly) {
        Finding f;
        f.title = tr("Windows has the drive marked read-only");
        f.text = tr("Its read-only mark is on (what diskpart's \"attributes disk set readonly\" does). Taking it off changes "
                    "nothing on the drive.");
        f.fix = Fix::ClearDiskAttribute;
        f.fixLabel = tr("Take the Mark Off");
        out << f;
    }
    // Linux's flag: set by a program (blockdev --setro, a udev rule), since the drive says it's writable.
    const bool kernelPart = std::any_of(s.parts.cbegin(), s.parts.cend(), [](const Part &p) { return p.kernelReadOnly; });
    if (!s.writeBlock && (s.kernelReadOnly || kernelPart)) {
        Finding f;
        f.title = tr("Linux was told not to write to it");
        f.text = s.drivesays == 0 ? tr("The drive itself is writable, but a read-only flag is set on it in Linux (a program "
                                       "or a rule did it: blockdev --setro). Clearing it takes the admin password.")
                                  : tr("A read-only flag is set on it in Linux. When a program or a rule set it (blockdev --setro), "
                                       "clearing it is the fix; it takes the admin password. If the flag comes back, it's the "
                                       "drive's own switch or the drive locked itself.");
        f.fix = Fix::ClearKernelFlag;
        f.fixLabel = tr("Clear the Flag");
        out << f;
    }
    const bool blockedAbove = !out.isEmpty();
    for (int i = 0; i < s.parts.size(); ++i) {
        const Part &p = s.parts[i];
        if (p.attributeReadOnly) {
            Finding f;
            f.part = i;
            f.title = tr("%1 is marked read-only").arg(p.name);
            f.text = tr("Windows has this volume's read-only mark on (what diskpart's \"attributes volume set readonly\" does). "
                        "Taking it off changes nothing in its files.");
            f.fix = Fix::ClearVolumeAttribute;
            f.fixLabel = tr("Take the Mark Off");
            out << f;
            continue;
        }
        if (!p.mountedReadOnly || blockedAbove || s.windows)
            continue;
        Finding f;
        f.part = i;
        f.title = tr("%1 is open read-only").arg(p.name);
        const QString fs = p.fsType.toLower();
        if (fs == QLatin1String("hfsplus")) {
            f.text = tr("It's a Mac drive (HFS+). Linux only writes to one whose journal is off; a Mac can switch it off, or "
                        "copy what's needed off it.");
        } else if (fs == QLatin1String("ntfs") || fs == QLatin1String("ntfs3")) {
            f.text = tr("Windows didn't shut down fully (Fast Startup, or it's hibernated), or it found errors, so Linux only "
                        "reads it. Start Windows and hold Shift while clicking Shut down; then it opens normally. Opening it "
                        "again for writing may work if it only had errors.");
            f.fix = Fix::Reopen;
            f.fixLabel = tr("Open It Again for Writing");
        } else if (fs == QLatin1String("iso9660") || fs == QLatin1String("udf") || fs == QLatin1String("squashfs")) {
            f.text = tr("Its file system can only be read (%1: a CD's or a disc image's). To save to the stick, it has to be "
                        "formatted, which erases it.")
                         .arg(p.fsType);
            f.stops = true;
        } else {
            f.text = tr("Either it was opened that way, or Linux found errors in its file system and stopped writing to keep "
                        "it safe. Repairing it and opening it again is the fix.");
            f.fix = Fix::RepairAndReopen;
            f.fixLabel = tr("Repair and Open Again");
        }
        out << f;
    }
    return out;
}

QString nothingFound(const State &s)
{
    Q_UNUSED(s)
    return tr("Nothing stops writing to it: the drive, the system and its file systems all allow it. When a program still "
              "says it's write-protected, it's usually one of these: it's full; a file over 4 GB doesn't fit on FAT32 (format "
              "it as exFAT); the file being replaced is marked read-only; or the folder belongs to another user.");
}

int askDrive(int fd)
{
#ifdef Q_OS_LINUX
    // MODE SENSE(6), every page; the header's device-specific byte has write protect as its top bit.
    unsigned char cdb[6] = {0x1a, 0x00, 0x3f, 0x00, 192, 0x00};
    unsigned char data[192] = {};
    unsigned char sense[32] = {};
    sg_io_hdr_t io;
    std::memset(&io, 0, sizeof io);
    io.interface_id = 'S';
    io.dxfer_direction = SG_DXFER_FROM_DEV;
    io.cmd_len = sizeof cdb;
    io.cmdp = cdb;
    io.dxfer_len = sizeof data;
    io.dxferp = data;
    io.mx_sb_len = sizeof sense;
    io.sbp = sense;
    io.timeout = 5000;
    if (fd < 0 || ::ioctl(fd, SG_IO, &io) != 0 || (io.info & SG_INFO_OK_MASK) != SG_INFO_OK)
        return -1;
    if (sizeof data - io.resid < 4)
        return -1;
    return (data[2] & 0x80) ? 1 : 0;
#else
    Q_UNUSED(fd)
    return -1;
#endif
}

void readLinux(State &s, const QString &sysRoot, const QString &mountInfo)
{
    const auto flag = [&sysRoot](const QString &device) {
        QFile f(sysRoot + QStringLiteral("/class/block/") + QFileInfo(device).fileName() + QStringLiteral("/ro"));
        return f.open(QIODevice::ReadOnly) && f.readAll().trimmed() == "1";
    };
    s.kernelReadOnly = flag(s.device);
    s.cardSlot = QFileInfo(s.device).fileName().startsWith(QLatin1String("mmcblk"));
    // Mounts: "id parent major:minor root point options ... - type source super-options".
    QHash<QString, QPair<QString, bool>> mounts;
    QFile f(mountInfo);
    if (f.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : f.readAll().split('\n')) {
            const QList<QByteArray> fields = line.split(' ');
            const int dash = int(fields.indexOf("-"));
            if (dash < 6 || fields.size() < dash + 4)
                continue;
            const QString source = QString::fromLocal8Bit(fields[dash + 2]);
            const bool ro = fields[5].split(',').contains("ro") || fields[dash + 3].split(',').contains("ro");
            QString point = QString::fromLocal8Bit(fields[4]);
            point.replace(QLatin1String("\\040"), QLatin1String(" "));
            if (!mounts.contains(source))
                mounts.insert(source, {point, ro});
        }
    }
    for (Part &p : s.parts) {
        p.kernelReadOnly = flag(p.device);
        const auto m = mounts.constFind(p.device);
        if (m != mounts.cend()) {
            p.mountPoint = m->first;
            p.mountedReadOnly = m->second;
        }
    }
}

QStringList clearKernelFlagScript(const State &s)
{
    QStringList devices{s.device};
    for (const Part &p : s.parts)
        if (p.device.startsWith(QLatin1String("/dev/")))
            devices << p.device;
    QStringList quoted;
    for (const QString &d : devices)
        quoted << shellQuote(d);
    return {QStringLiteral("sh"), QStringLiteral("-c"), QStringLiteral("blockdev --setrw ") + quoted.join(QLatin1Char(' '))};
}

#ifdef Q_OS_WIN
namespace {

DWORD regDword(HKEY root, const wchar_t *key, const wchar_t *value)
{
    DWORD data = 0, size = sizeof data;
    if (RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &data, &size) != ERROR_SUCCESS)
        return 0;
    return data;
}

} // namespace

void readWindows(State &s)
{
    s.windows = true;
    s.policyWriteProtect = regDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\StorageDevicePolicies", L"WriteProtect") != 0;
    // Removable Disks (the class GUID of disk drives): Deny write access, or Deny all access.
    const wchar_t *removable = L"SOFTWARE\\Policies\\Microsoft\\Windows\\RemovableStorageDevices\\{53f5630d-b6bf-11d0-94f2-00a0c91efb8b}";
    if (regDword(HKEY_LOCAL_MACHINE, removable, L"Deny_Write") || regDword(HKEY_CURRENT_USER, removable, L"Deny_Write"))
        s.policyDenyWrite << tr("Removable Disks: Deny write access");
    if (regDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\RemovableStorageDevices", L"Deny_All"))
        s.policyDenyWrite << tr("All Removable Storage classes: Deny all access");
    if (regDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Policies\\Microsoft\\FVE", L"RDVDenyWriteAccess"))
        s.policyDenyWrite << tr("Deny write access to removable drives not protected by BitLocker");

    const win::Handle disk = win::openDevice(s.device, GENERIC_READ);
    if (disk.valid()) {
        GET_DISK_ATTRIBUTES attributes {};
        attributes.Version = sizeof attributes;
        if (win::ioctl(disk.get(), IOCTL_DISK_GET_DISK_ATTRIBUTES, nullptr, 0, &attributes, sizeof attributes))
            s.diskAttributeReadOnly = attributes.Attributes & DISK_ATTRIBUTE_READ_ONLY;
        const bool writable = win::ioctl(disk.get(), IOCTL_DISK_IS_WRITABLE, nullptr, 0, nullptr, 0);
        // The policy and the mark make Windows say "write-protected" too; only without them is it the drive.
        s.drivesays = writable ? 0 : (GetLastError() == ERROR_WRITE_PROTECT && !s.policyWriteProtect && !s.diskAttributeReadOnly ? 1 : -1);
    }
    // The volumes' read-only marks, from Windows' own cmdlet.
    if (s.diskNumber >= 0 && !s.parts.isEmpty()) {
        QProcess ps;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("DF_DISK"), QString::number(s.diskNumber));
        ps.setProcessEnvironment(env);
        const QString script = QStringLiteral("Get-Partition -DiskNumber ([int]$env:DF_DISK) | Where-Object IsReadOnly | ForEach-Object { $_.PartitionNumber }");
        ps.start(QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-Command"), script});
        if (ps.waitForFinished(30000)) {
            for (const QByteArray &line : ps.readAllStandardOutput().split('\n')) {
                bool ok = false;
                const int n = line.trimmed().toInt(&ok);
                for (Part &p : s.parts)
                    if (ok && p.number == n)
                        p.attributeReadOnly = true;
            }
        }
    }
}

QString clearPolicy()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\StorageDevicePolicies", 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return win::lastErrorText();
    DWORD zero = 0;
    const LSTATUS done = RegSetValueExW(key, L"WriteProtect", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&zero), sizeof zero);
    RegCloseKey(key);
    return done == ERROR_SUCCESS ? QString() : win::errorText(DWORD(done));
}

QString clearDiskAttribute(int diskNumber)
{
    const win::Handle disk = win::openDevice(QStringLiteral("\\\\.\\PhysicalDrive%1").arg(diskNumber), GENERIC_READ | GENERIC_WRITE);
    if (!disk.valid())
        return win::lastErrorText();
    SET_DISK_ATTRIBUTES set {};
    set.Version = sizeof set;
    set.Persist = TRUE;
    set.Attributes = 0;
    set.AttributesMask = DISK_ATTRIBUTE_READ_ONLY;
    if (!win::ioctl(disk.get(), IOCTL_DISK_SET_DISK_ATTRIBUTES, &set, sizeof set, nullptr, 0))
        return win::lastErrorText();
    win::ioctl(disk.get(), IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0);
    return {};
}

QString clearVolumeAttribute(int diskNumber, int partitionNumber)
{
    QProcess ps;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("DF_DISK"), QString::number(diskNumber));
    env.insert(QStringLiteral("DF_PART"), QString::number(partitionNumber));
    ps.setProcessEnvironment(env);
    ps.setProcessChannelMode(QProcess::MergedChannels);
    const QString script = QStringLiteral("$ErrorActionPreference = 'Stop'; Set-Partition -DiskNumber ([int]$env:DF_DISK) -PartitionNumber ([int]$env:DF_PART) -IsReadOnly $false");
    ps.start(QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-Command"), script});
    if (!ps.waitForFinished(60000))
        return tr("Windows didn't answer.");
    return ps.exitCode() == 0 ? QString() : win::windowsWords(QString::fromLocal8Bit(ps.readAll()).trimmed());
}
#endif

} // namespace writeprotect
