// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "format.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QLocale>
#include <QStringList>

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("format", text);
}

} // namespace

QString formatSize(quint64 bytes)
{
    // 1024-based, like Windows
    return QLocale().formattedDataSize(qint64(bytes), 2, QLocale::DataSizeTraditionalFormat);
}

QString shortDevice(const QString &device)
{
    return device.section(QLatin1Char('/'), -1);
}

QString volumeTitle(const Volume &v)
{
    const QString name = !v.label.isEmpty() ? v.label : v.partName;
    const QString dev = shortDevice(v.device);
    return name.isEmpty() || name == dev ? dev : QStringLiteral("%1 (%2)").arg(name, dev);
}

QString volumeStatus(const Volume &v, bool brief)
{
    auto mounted = [brief](QStringList mps) {
        for (QString &mp : mps)
            mp = QDir::toNativeSeparators(mp); // E:\ on Windows
        if (brief && mps.size() > 1)
            return tr("Mounted at %1 (+%2)").arg(mps.first()).arg(mps.size() - 1);
        return tr("Mounted at %1").arg(mps.join(QStringLiteral(", ")));
    };

    QStringList parts;
    if (!v.raidMemberOf.isEmpty())
        parts << tr("RAID member of %1").arg(v.raidMemberOf);
    if (!v.lvmMemberOf.isEmpty())
        parts << tr("LVM: part of %1").arg(v.lvmMemberOf);
    if (v.lvmUnknown)
        parts << tr("LVM: install udisks2-lvm2 to see what's in it");
    if (v.isLv && !v.lvActive)
        parts << tr("Inactive logical volume");
    if (v.isEfi)
        parts << tr("EFI System");
    if (v.isSystem)
        parts << tr("System");

    if (v.isContainer)
        parts << tr("Extended partition");
    else if (v.fsType == QLatin1String("swap"))
        parts << (v.swapActive ? tr("Swap, in use") : tr("Swap, not in use"));
    else if (v.encrypted) {
        // UDisks calls what it can't recognize "crypto_unknown" when it's looking for VeraCrypt:
        // random-looking, so maybe a VeraCrypt volume (or just empty, or damaged).
        parts << (v.fsType == QLatin1String("BitLocker")        ? tr("BitLocker")
                  : v.fsType == QLatin1String("crypto_TCRYPT")  ? tr("VeraCrypt")
                  : v.fsType == QLatin1String("crypto_unknown") ? tr("Unknown, maybe VeraCrypt")
                                                                : tr("Encrypted"));
        if (v.cleartextPath.isEmpty())
            parts << tr("Locked");
        else if (v.cleartextMountPoints.isEmpty())
            parts << tr("Unlocked");
        else
            parts << mounted(v.cleartextMountPoints);
    } else if (!v.mountPoints.isEmpty())
        parts << mounted(v.mountPoints);
    else if (v.hasFilesystem)
        parts << tr("Not mounted");
    else if (v.fsType == QLatin1String("LVM2_member"))
        parts << tr("LVM member");
    else if (v.fsUsage == QLatin1String("raid"))
        parts << tr("RAID member");
    else if (v.fsType.isEmpty())
        parts << tr("No file system");
    else
        parts << v.fsType;

    if (!v.fstab.isEmpty() && !v.isSystem)
        parts << tr("Mounts at startup");
    return parts.join(QStringLiteral(" · "));
}

QString kernelName(const QString &device)
{
    const QString real = QFileInfo(device).canonicalFilePath();
    return QFileInfo(real.isEmpty() ? device : real).fileName();
}

QString raidLevelName(const QString &level)
{
    if (level.startsWith(QLatin1String("raid")))
        return QStringLiteral("RAID %1").arg(level.mid(4));
    if (level == QLatin1String("linear"))
        return tr("Linear");
    return level.isEmpty() ? tr("RAID") : level;
}

QString raidStatus(const Disk &d)
{
    if (!d.raidRunning)
        return tr("Stopped");
    const int percent = int(d.raidSyncDone * 100);
    if (d.raidSync == QLatin1String("recover") || d.raidSync == QLatin1String("resync"))
        return tr("Rebuilding: %1%").arg(percent);
    if (d.raidSync == QLatin1String("check") || d.raidSync == QLatin1String("repair"))
        return tr("Checking: %1%").arg(percent);
    if (d.raidSync == QLatin1String("reshape"))
        return tr("Reshaping: %1%").arg(percent);
    if (d.raidDegraded > 0)
        return tr("Degraded: %1 of %2 drives missing").arg(d.raidDegraded).arg(d.raidDevices);
    return QCoreApplication::translate("format", "%n drive(s), all there", nullptr, d.raidDevices);
}

QString diskKind(const Disk &d)
{
    if (d.isLvm)
        return tr("LVM");
    if (d.isRaid)
        return raidLevelName(d.raidLevel);
    if (d.isLoop)
        return tr("Disk image");
    if (d.bus == QLatin1String("usb"))
        return tr("USB");
    if (d.bus == QLatin1String("nvme") || d.device.startsWith(QLatin1String("/dev/nvme")))
        return tr("NVMe SSD");
    if (d.bus == QLatin1String("sd") || d.device.startsWith(QLatin1String("/dev/mmcblk")))
        return tr("SD card");
    if (d.rotationRate > 0)
        return tr("HDD");
    if (d.rotationRate == 0)
        return tr("SSD");
    return tr("Disk");
}

QString tableName(const Disk &d)
{
    if (d.isLvm)
        return tr("Volume group");
    if (d.tableType == QLatin1String("gpt"))
        return tr("GPT");
    if (d.tableType == QLatin1String("dos"))
        return tr("MBR");
    if (!d.tableType.isEmpty())
        return d.tableType.toUpper();
    return d.volumes.isEmpty() ? tr("Not initialized") : tr("No partition table");
}

namespace {

const QHash<QString, const char *> &typeNames()
{
    static const QHash<QString, const char *> names = {
        {QStringLiteral("c12a7328-f81f-11d2-ba4b-00a0c93ec93b"), "EFI System"},
        {QStringLiteral("21686148-6449-6e6f-744e-656564454649"), "BIOS boot"},
        {QStringLiteral("bc13c2ff-59e6-4262-a352-b275fd6f7172"), "Linux extended boot"},
        {QStringLiteral("0fc63daf-8483-4772-8e79-3d69d8477de4"), "Linux filesystem"},
        {QStringLiteral("4f68bce3-e8cd-4db1-96e7-fbcaf984b709"), "Linux root (x86-64)"},
        {QStringLiteral("933ac7e1-2eb4-4f13-b844-0e14e2aef915"), "Linux /home"},
        {QStringLiteral("0657fd6d-a4ab-43c4-84e5-0933c84b4f4f"), "Linux swap"},
        {QStringLiteral("e6d6d379-f507-44c2-a23c-238f2a3df928"), "Linux LVM"},
        {QStringLiteral("ca7d7ccb-63ed-4c53-861c-1742536059cc"), "Linux LUKS"},
        {QStringLiteral("a19d880f-05fc-4d3b-a006-743f0f84911e"), "Linux RAID"},
        {QStringLiteral("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"), "Microsoft basic data"},
        {QStringLiteral("e3c9e316-0b5c-4db8-817d-f92df00215ae"), "Microsoft reserved"},
        {QStringLiteral("de94bba4-06d1-4d40-a16a-bfd50179d6ac"), "Windows recovery"},
        {QStringLiteral("0x07"), "NTFS / exFAT"},
        {QStringLiteral("0x0b"), "FAT32"},
        {QStringLiteral("0x0c"), "FAT32 (LBA)"},
        {QStringLiteral("0x05"), "Extended"},
        {QStringLiteral("0x0f"), "Extended (LBA)"},
        {QStringLiteral("0x82"), "Linux swap"},
        {QStringLiteral("0x83"), "Linux"},
        {QStringLiteral("0x8e"), "Linux LVM"},
        {QStringLiteral("0xef"), "EFI System"},
        {QStringLiteral("0xfd"), "Linux RAID"},
    };
    return names;
}

QString plainTypeName(const QString &type)
{
    const auto it = typeNames().constFind(type.toLower());
    return it == typeNames().constEnd() ? QString() : tr(*it);
}

} // namespace

QString partitionTypeName(const QString &type)
{
    const QString name = plainTypeName(type);
    return name.isEmpty() ? type : QStringLiteral("%1 (%2)").arg(name, type);
}

QVector<PartitionTypeChoice> partitionTypeChoices(const QString &tableType)
{
    const QStringList gpt = {QStringLiteral("0fc63daf-8483-4772-8e79-3d69d8477de4"), QStringLiteral("4f68bce3-e8cd-4db1-96e7-fbcaf984b709"),
                             QStringLiteral("933ac7e1-2eb4-4f13-b844-0e14e2aef915"), QStringLiteral("0657fd6d-a4ab-43c4-84e5-0933c84b4f4f"),
                             QStringLiteral("e6d6d379-f507-44c2-a23c-238f2a3df928"), QStringLiteral("a19d880f-05fc-4d3b-a006-743f0f84911e"),
                             QStringLiteral("ca7d7ccb-63ed-4c53-861c-1742536059cc"), QStringLiteral("c12a7328-f81f-11d2-ba4b-00a0c93ec93b"),
                             QStringLiteral("21686148-6449-6e6f-744e-656564454649"), QStringLiteral("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"),
                             QStringLiteral("e3c9e316-0b5c-4db8-817d-f92df00215ae"), QStringLiteral("de94bba4-06d1-4d40-a16a-bfd50179d6ac")};
    const QStringList dos = {QStringLiteral("0x83"), QStringLiteral("0x82"), QStringLiteral("0x8e"), QStringLiteral("0xfd"),
                             QStringLiteral("0x07"), QStringLiteral("0x0c"), QStringLiteral("0xef")};
    QVector<PartitionTypeChoice> out;
    for (const QString &value : tableType == QLatin1String("gpt") ? gpt : tableType == QLatin1String("dos") ? dos : QStringList())
        out.push_back({value, plainTypeName(value)});
    return out;
}

QString partitionTypeProblem(const QString &tableType, const QString &value)
{
    if (tableType == QLatin1String("gpt")) {
        static const QRegularExpression guid(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
        if (!guid.match(value).hasMatch())
            return tr("A GPT type looks like 0fc63daf-8483-4772-8e79-3d69d8477de4.");
        if (value == QLatin1String("00000000-0000-0000-0000-000000000000"))
            return tr("All zeros marks an unused entry: the partition would be gone.");
        return {};
    }
    if (tableType == QLatin1String("dos")) {
        static const QRegularExpression byte(QStringLiteral("^0x[0-9a-f]{2}$"));
        if (!byte.match(value).hasMatch())
            return tr("An MBR type is two hex digits, like 0x83.");
        if (value == QLatin1String("0x00"))
            return tr("0x00 marks an unused entry: the partition would be gone.");
        if (value == QLatin1String("0x05") || value == QLatin1String("0x0f") || value == QLatin1String("0x85"))
            return tr("That type marks an extended partition, which needs its own table inside.");
        return {};
    }
    return tr("Only GPT and MBR partitions have a type.");
}

bool isBootPartitionType(const QString &type)
{
    return type == QLatin1String("c12a7328-f81f-11d2-ba4b-00a0c93ec93b") || type == QLatin1String("21686148-6449-6e6f-744e-656564454649")
        || type == QLatin1String("0xef");
}

QVector<PartitionFlagChoice> partitionFlagChoices(const QString &tableType)
{
    if (tableType == QLatin1String("gpt")) {
        return {{quint64(1) << 0, tr("Required by the platform"), tr("The firmware or the system needs it; tools are meant to leave it alone.")},
                {quint64(1) << 2, tr("Legacy BIOS bootable"), tr("Old BIOS boot code looks for this.")},
                {quint64(1) << 60, tr("Read-only"), tr("Windows and systemd mount it read-only.")},
                {quint64(1) << 62, tr("Hidden"), tr("Windows doesn't show it.")},
                {quint64(1) << 63, tr("Don't mount automatically"), tr("Windows gives it no drive letter, and systemd doesn't mount it by itself.")}};
    }
    if (tableType == QLatin1String("dos"))
        return {{0x80, tr("Bootable (active)"), tr("Old BIOS boot code starts from the partition marked bootable.")}};
    return {};
}

quint64 mergedPartitionFlags(const QString &tableType, quint64 old, quint64 chosen)
{
    quint64 shown = 0;
    for (const PartitionFlagChoice &f : partitionFlagChoices(tableType))
        shown |= f.bit;
    return (old & ~shown) | (chosen & shown);
}

namespace {

enum class Hidden { No, Space, Drop };

Hidden hidden(char32_t c)
{
    switch (QChar::category(c)) {
    case QChar::Other_Control:
    case QChar::Separator_Line:
    case QChar::Separator_Paragraph:
        return Hidden::Space;
    case QChar::Other_Format: // zero-width characters, text direction overrides
    case QChar::Other_Surrogate:
        return Hidden::Drop;
    default:
        return Hidden::No;
    }
}

} // namespace

QString cleanName(const QString &name)
{
    if (!hasHiddenCharacters(name))
        return name;
    QString out;
    for (const char32_t c : name.toUcs4()) {
        switch (hidden(c)) {
        case Hidden::No:
            out += QString::fromUcs4(&c, 1);
            break;
        case Hidden::Space:
            out += QLatin1Char(' ');
            break;
        case Hidden::Drop:
            break;
        }
    }
    return out;
}

bool hasHiddenCharacters(const QString &text)
{
    for (const char32_t c : text.toUcs4()) {
        if (hidden(c) != Hidden::No)
            return true;
    }
    return false;
}
