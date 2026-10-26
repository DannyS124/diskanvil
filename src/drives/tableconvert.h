// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Changing a drive's partition table between MBR and GPT without touching what's in the
// partitions: each one keeps its exact place and size, and only its kind is put the other
// table's way. Worked out here from the layout the drive has now (partrecover.h), checked, and
// written with Recover Partitions' own writer, which also keeps the old one to go back to.

#include "partrecover.h"

#include <QStringList>

namespace tableconvert {

struct Plan {
    recover::Layout to;   // the table it would get
    QStringList changes;  // what changes, a line for each partition
    QStringList warnings; // worth knowing before going ahead
    QString refusal;      // why it can't be done, or empty
};

// MBR to GPT. `bootsTheOldWay`: the drive starts a PC the BIOS way (boot code and a partition
// marked to start from), which stops working on GPT.
Plan toGpt(const recover::Layout &now, quint64 diskSize, int sectorSize, bool bootsTheOldWay);
// GPT to MBR: four partitions at most, all of them within the first 2 TiB. Windows' empty
// reserved partition and GRUB's BIOS boot partition have no place on MBR and are left out.
Plan toMbr(const recover::Layout &now, quint64 diskSize, int sectorSize);

// The kinds, either way (GPT's as lower-case GUIDs, MBR's as "0x83"). Empty when there's none.
QString gptTypeFor(const QString &mbrType);
QString mbrTypeFor(const QString &gptType, const QString &fsType);

} // namespace tableconvert
