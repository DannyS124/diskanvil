// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "udisks.h"

#include <QString>
#include <QVector>

QString formatSize(quint64 bytes);
QString shortDevice(const QString &device);   // /dev/sda1 -> sda1
QString volumeTitle(const Volume &v); // "LABEL (sda1)"
QString volumeStatus(const Volume &v, bool brief = false);
QString diskKind(const Disk &d); // "NVMe SSD", "USB", ...
QString tableName(const Disk &d);
QString partitionTypeName(const QString &type);
QString raidLevelName(const QString &level); // "raid1" -> "RAID 1"
// The kernel's name for a device: /dev/mapper/vg-data and /dev/vg/data are dm-0 in sysfs.
QString kernelName(const QString &device);
QString raidStatus(const Disk &d);           // "2 of 2 drives, all there", "Rebuilding: 34%"...

// What Type and Flags offers for a GPT or MBR ("dos") table.
struct PartitionTypeChoice {
    QString value; // GUID, or "0x83"
    QString name;
};
QVector<PartitionTypeChoice> partitionTypeChoices(const QString &tableType);
// A type typed in by hand; empty when it's fine. Never one that empties or breaks the entry.
QString partitionTypeProblem(const QString &tableType, const QString &value);
bool isBootPartitionType(const QString &type); // EFI System or BIOS boot
struct PartitionFlagChoice {
    quint64 bit;
    QString name;
    QString hint;
};
QVector<PartitionFlagChoice> partitionFlagChoices(const QString &tableType);
// The flags to set: the chosen ones, plus whatever else the partition already had.
quint64 mergedPartitionFlags(const QString &tableType, quint64 old, quint64 chosen);

// Drive names (labels, models, partition names) come from the drive, so whoever made it
// picked them. Control characters become spaces, and invisible ones (zero-width, or those
// that flip the text direction) are dropped, so a name can't hide or fake anything.
QString cleanName(const QString &name);
bool hasHiddenCharacters(const QString &text); // something cleanName() would change
