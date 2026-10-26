// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tableconvert.h"

#include "format.h"

#include <QCoreApplication>
#include <QHash>
#include <QRandomGenerator>

#include <algorithm>

namespace tableconvert {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("tableconvert", text);
}

const QString kBasicData = QStringLiteral("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7");
const QString kLinux = QStringLiteral("0fc63daf-8483-4772-8e79-3d69d8477de4");
const QString kSwap = QStringLiteral("0657fd6d-a4ab-43c4-84e5-0933c84b4f4f");
const QString kLvm = QStringLiteral("e6d6d379-f507-44c2-a23c-238f2a3df928");
const QString kRaid = QStringLiteral("a19d880f-05fc-4d3b-a006-743f0f84911e");
const QString kEfi = QStringLiteral("c12a7328-f81f-11d2-ba4b-00a0c93ec93b");
const QString kRecovery = QStringLiteral("de94bba4-06d1-4d40-a16a-bfd50179d6ac");
const QString kReserved = QStringLiteral("e3c9e316-0b5c-4db8-817d-f92df00215ae"); // Microsoft's, empty
const QString kBiosBoot = QStringLiteral("21686148-6449-6e6f-744e-656564454649"); // GRUB's on GPT
constexpr quint64 kLegacyBootable = 1ull << 2; // GPT's "start this the BIOS way" attribute

QString describe(const recover::Part &p)
{
    const QString what = !p.label.isEmpty() ? p.label : !p.fsType.isEmpty() ? p.fsType : QString();
    return what.isEmpty() ? tr("Partition %1 (%2)").arg(p.number).arg(formatSize(p.size))
                          : tr("Partition %1 (%2, %3)").arg(p.number).arg(formatSize(p.size)).arg(what);
}

} // namespace

QString gptTypeFor(const QString &mbrType)
{
    static const QHash<QString, QString> map = {
        {QStringLiteral("0x01"), kBasicData}, {QStringLiteral("0x04"), kBasicData}, {QStringLiteral("0x06"), kBasicData},
        {QStringLiteral("0x07"), kBasicData}, {QStringLiteral("0x0b"), kBasicData}, {QStringLiteral("0x0c"), kBasicData},
        {QStringLiteral("0x0e"), kBasicData}, {QStringLiteral("0x17"), kBasicData}, {QStringLiteral("0x1b"), kBasicData},
        {QStringLiteral("0x1c"), kBasicData}, {QStringLiteral("0x83"), kLinux},     {QStringLiteral("0x82"), kSwap},
        {QStringLiteral("0x8e"), kLvm},       {QStringLiteral("0xfd"), kRaid},      {QStringLiteral("0xef"), kEfi},
        {QStringLiteral("0x27"), kRecovery},
    };
    return map.value(mbrType.toLower());
}

QString mbrTypeFor(const QString &gptType, const QString &fsType)
{
    const QString t = gptType.toLower();
    if (t == kBasicData)
        return fsType == QLatin1String("vfat") ? QStringLiteral("0x0c") : QStringLiteral("0x07");
    if (t == kSwap)
        return QStringLiteral("0x82");
    if (t == kLvm)
        return QStringLiteral("0x8e");
    if (t == kRaid)
        return QStringLiteral("0xfd");
    if (t == kEfi)
        return QStringLiteral("0xef");
    if (t == kRecovery)
        return QStringLiteral("0x27");
    // Linux's own kinds (a plain file system, root, home, /srv, /var...): MBR has one for all.
    static const QStringList linuxKinds = {kLinux,
                                      QStringLiteral("4f68bce3-e8cd-4db1-96e7-fbcaf984b709"), QStringLiteral("44479540-f297-41b2-9af7-d131d5f0458a"),
                                      QStringLiteral("b921b045-1df0-41c3-af44-4c6f280d3fae"), QStringLiteral("933ac7e1-2eb4-4f13-b844-0e14e2aef915"),
                                      QStringLiteral("3b8f8425-20e0-4f3b-907f-1a25a76f98e8"), QStringLiteral("4d21b016-b534-45c2-a9fb-5c16e091fd2d"),
                                      QStringLiteral("7ec6f557-3bc5-4aca-b293-16ef5df639d1"), QStringLiteral("bc13c2ff-59e6-4262-a352-b275fd6f7172")};
    return linuxKinds.contains(t) ? QStringLiteral("0x83") : QString();
}

Plan toGpt(const recover::Layout &now, quint64 diskSize, int sectorSize, bool bootsTheOldWay)
{
    Plan plan;
    if (now.table != QLatin1String("dos")) {
        plan.refusal = tr("It has a GPT partition table already.");
        return plan;
    }
    QVector<recover::Part> parts;
    bool logical = false;
    for (const recover::Part &p : now.parts) {
        if (p.container) {
            plan.changes << tr("The extended partition held the ones inside it: they become partitions of their own, the same "
                               "as the others.");
            continue;
        }
        logical = logical || p.logical;
        parts << p;
    }
    std::sort(parts.begin(), parts.end(), [](const recover::Part &a, const recover::Part &b) { return a.start < b.start; });
    plan.to.table = QStringLiteral("gpt");
    plan.to.diskSize = diskSize;
    int number = 0;
    for (recover::Part p : std::as_const(parts)) {
        QString type = gptTypeFor(p.type);
        if (type.isEmpty()) {
            type = kBasicData;
            plan.warnings << tr("%1 is of a kind (%2) GPT has no match for: it's marked as a basic data partition.").arg(describe(p), p.type);
        }
        const QString before = describe(p), was = partitionTypeName(p.type);
        const bool bootable = p.flags & 0x80;
        p.number = ++number;
        p.logical = false;
        p.guid.clear();
        p.name.clear();
        p.flags = bootable ? kLegacyBootable : 0;
        plan.changes << tr("%1: %2, now %3").arg(before, was, partitionTypeName(type));
        p.type = type;
        plan.to.parts << p;
    }
    if (plan.to.parts.isEmpty()) {
        plan.refusal = tr("There are no partitions on it to keep.");
        return plan;
    }
    if (const QString why = recover::problem(plan.to, diskSize, sectorSize); !why.isEmpty()) {
        plan.refusal = tr("GPT needs the first 34 sectors of the drive and the last 33 for its own tables, and a partition is "
                          "in them, so it can't be changed without moving a partition. (%1)").arg(why);
        return plan;
    }
    if (logical)
        plan.warnings << tr("The partitions inside the extended one get new numbers (sda5 can become sda4): a Linux that finds its "
                            "partitions by number instead of by ID needs /etc/fstab changed to match.");
    if (bootsTheOldWay)
        plan.warnings << tr("This drive starts a PC the old (BIOS) way. On GPT it starts only with UEFI: Windows on it would need its "
                            "boot files made again for UEFI (with an EFI partition), and Linux's GRUB put back for UEFI. The files "
                            "on it aren't affected.");
    return plan;
}

Plan toMbr(const recover::Layout &now, quint64 diskSize, int sectorSize)
{
    Plan plan;
    if (now.table != QLatin1String("gpt")) {
        plan.refusal = tr("It has an MBR partition table already.");
        return plan;
    }
    QVector<recover::Part> parts = now.parts;
    std::sort(parts.begin(), parts.end(), [](const recover::Part &a, const recover::Part &b) { return a.start < b.start; });
    plan.to.table = QStringLiteral("dos");
    plan.to.diskSize = diskSize;
    // A new disk signature (MBR has no room for GPT's ID).
    plan.to.diskId = QStringLiteral("%1").arg(QRandomGenerator::global()->bounded(1u, 0xFFFFFFFFu), 8, 16, QLatin1Char('0'));
    const quint64 sector = quint64(std::max(sectorSize, 512));
    bool efi = false;
    int number = 0;
    for (recover::Part p : std::as_const(parts)) {
        const QString type = p.type.toLower();
        if (type == kReserved || type == kBiosBoot) {
            plan.changes << tr("%1: %2, left out: MBR has no such kind, and nothing is kept in it.").arg(describe(p), partitionTypeName(type));
            continue;
        }
        const QString mbr = mbrTypeFor(type, p.fsType);
        if (mbr.isEmpty()) {
            plan.refusal = tr("%1 is of a kind (%2) MBR has no match for.").arg(describe(p), partitionTypeName(type));
            return plan;
        }
        if ((p.start + p.size) / sector > 0xFFFFFFFFull) {
            plan.refusal = tr("%1 goes past the first 2 TiB of the drive, which is as far as MBR can reach.").arg(describe(p));
            return plan;
        }
        efi = efi || type == kEfi;
        const QString before = describe(p);
        p.number = ++number;
        p.guid.clear();
        p.name.clear();
        p.flags = (p.flags & kLegacyBootable) ? 0x80 : 0;
        p.type = mbr;
        plan.changes << tr("%1: %2, now %3").arg(before, partitionTypeName(type), partitionTypeName(mbr));
        plan.to.parts << p;
    }
    if (plan.to.parts.size() > 4) {
        plan.refusal = tr("MBR holds four partitions, and this drive has %1 to keep.").arg(plan.to.parts.size());
        return plan;
    }
    if (plan.to.parts.isEmpty()) {
        plan.refusal = tr("There are no partitions on it to keep.");
        return plan;
    }
    if (const QString why = recover::problem(plan.to, diskSize, sectorSize); !why.isEmpty()) {
        plan.refusal = why;
        return plan;
    }
    if (efi)
        plan.warnings << tr("It has an EFI partition: a system on it that starts with UEFI may not start from an MBR drive "
                            "(Windows doesn't). The files on it aren't affected.");
    return plan;
}

} // namespace tableconvert
