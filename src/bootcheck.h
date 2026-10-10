// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Scan for Viruses, its third layer: what starts the PC before Windows does. Bootkits hide
// there, where antivirus programs running inside Windows can't see them.
//
// - The UEFI boot programs on the EFI partition: who signed each one, whether this PC's
//   Secure Boot trusts that, and whether its list of banned files (dbx) names it.
// - The PC's boot entries: whether they start files that are there.
// - The old BIOS way: the boot code in a drive's first sector.
//
// A Windows boot manager that's been tampered with can be put back from the clean copy every
// Windows keeps (Windows\Boot\EFI), once that copy checks out.

#include "efivars.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace boot {

struct Finding {
    QString name;   // "bootmgfw.efi", "Boot0003", "The first sector"
    QString kind;   // in plain words: "Windows' boot manager"
    QString where;  // "\EFI\Microsoft\Boot\bootmgfw.efi"
    QString verdict;
    QStringList reasons; // why it looks wrong; empty when nothing does
    int severity = 0;
    QString file;      // the boot program here (on the mounted EFI partition)
    QString cleanCopy; // Windows' own copy, that can be put back over it; empty when there's none

    void flag(int level, const QString &reason);
};

struct Report {
    QVector<Finding> findings;
    QStringList notes;
};

// The boot programs on the EFI partition mounted at `esp`. `windows`, where that Windows is
// mounted (for its clean copies), can be empty.
Report checkEfiPartition(const QString &esp, const QString &windows, const efi::Trust &trust);

// The PC's boot entries that start something on the EFI partition `partition` (its GPT GUID),
// mounted at `esp`.
QVector<Finding> checkBootEntries(const QVector<efi::BootEntry> &entries, const QString &partition, const QString &esp);

// A drive's first sector: its boot code for the old BIOS way. `uefiDrive`: it has a GPT, so
// that code isn't what starts it.
Finding checkFirstSector(const QByteArray &sector, bool uefiDrive);

// The PC's firmware: Secure Boot's state and keys, and, from fwupd's report (`fwupdmgr
// security --json`, on Linux), how well the firmware itself is protected against being
// rewritten. Firmware can't be scanned like files; these are the doors malware uses.
QVector<Finding> checkFirmware(const efi::Trust &trust, const QByteArray &fwupdReport);

// Puts Windows' own copy back over `finding.file`, keeping the old one in `backup`.
bool putBack(const Finding &finding, const QString &backup, QString *message);

} // namespace boot
