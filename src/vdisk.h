// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Virtual machines' disk files, read as the disk inside them: VirtualBox's .vdi, VMware's .vmdk,
// Windows' and Hyper-V's .vhd and .vhdx, and QEMU's .qcow2. Our own readers, from each format's
// published description. Only ever read: the file isn't changed.

#include "lostsource.h"

#include <QString>

#include <memory>

namespace vdisk {

// "VDI", "VMDK", "VHD", "VHDX" or "QCOW2" for a virtual machine's disk file; empty for anything
// else (a plain image is just its bytes).
QString formatOf(const QString &path);

// The disk inside a virtual machine's disk file. Null, with `problem` saying why in plain words,
// when it can't be read (one that only holds the changes made to another disk, say). `warning`
// gets what's worth knowing that doesn't stop it: that it wasn't closed properly, say.
std::shared_ptr<lost::Source> open(const QString &path, QString *problem, QString *warning = nullptr);

} // namespace vdisk
