// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The PC's boot menu (UEFI): its entries, their order, and the one to start next time, changed.
// On Linux through efibootmgr (as root: DiskAnvil Live's sudo, or the admin password), on
// Windows by writing the firmware's variables (DiskAnvil runs as administrator there).

#include "efivars.h"

#include <QStringList>

namespace efiboot {

struct State {
    bool uefi = false;
    QVector<efi::BootEntry> entries; // in boot order, then the rest
    QVector<int> order;
    int next = -1;    // BootNext: started once, next time
    int current = -1; // BootCurrent: what started this time
};
State read();

QString hex4(int number); // "0003"
// An EFI_LOAD_OPTION starting `path` on a GPT partition (its number, start and size in
// sectors, and its GUID).
QByteArray loadOption(const QString &description, quint32 partitionNumber, quint64 start, quint64 size, const QString &partitionGuid,
                      const QString &path, bool active = true);
QByteArray orderValue(const QVector<int> &order);

// efibootmgr's commands for each change (Linux).
QStringList orderCommand(const QVector<int> &order);
QStringList nextCommand(int number);
QStringList activeCommand(int number, bool active);
QStringList removeCommand(int number);
QStringList createCommand(const QString &disk, int partitionNumber, const QString &path, const QString &label);

} // namespace efiboot
