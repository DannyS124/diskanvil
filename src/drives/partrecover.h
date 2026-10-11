// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Recover Partitions: partition layouts DiskForge remembers, and writing one back. Only the
// partition table is ever written, never what's in the partitions.
//
// Every time DiskForge sees a drive with partitions, it keeps the layout (up to 10
// different ones per drive, in ~/.local/share/diskforge/layouts). Putting an earlier one
// back undoes a deleted partition or a wiped table.

#include "gpt.h"
#include "udisks.h"

#include <QDateTime>
#include <QJsonObject>
#include <QVector>

namespace recover {

struct Part {
    int number = 0;
    quint64 start = 0; // bytes
    quint64 size = 0;
    QString type;      // GUID (GPT) or "0x83" (MBR)
    QString guid;      // the partition's own ID (GPT)
    QString name;      // GPT only
    quint64 flags = 0;
    bool container = false; // MBR extended partition
    bool logical = false;   // inside it
    QString fsType;    // shown, not written
    QString label;
};

struct Layout {
    QDateTime saved;
    QString table;   // "gpt" or "dos"
    quint64 diskSize = 0;
    QString diskId;  // the GPT disk ID, or the MBR disk signature (8 hex digits)
    QVector<Part> parts;
    QString fingerprint() const; // what would be written: same fingerprint, same table
};

QString diskId(const QString &device); // from udev's database, readable without root
Layout fromDisk(const Disk &disk);
Layout fromReport(const gpt::Report &report, bool backup); // a GPT as read from the drive

QJsonObject toJson(const Layout &layout);
bool fromJson(const QJsonObject &object, Layout *layout); // false for anything malformed

QString folder();
QVector<Layout> saved(const QString &driveKey); // newest first
// Keeps the layout if it differs from the newest one kept. Layouts without partitions
// aren't kept: a wiped table isn't something to go back to.
bool remember(const QString &driveKey, const Layout &layout);

// Why this layout can't be written to a drive of this size, or empty.
QString problem(const Layout &layout, quint64 diskSize, int sectorSize);
gpt::Result write(int fd, const Layout &layout, int sectorSize = 0); // 0 = ask the drive
// The drive as it would look with this layout, for the "after" map.
Disk preview(const Disk &disk, const Layout &layout);

} // namespace recover
