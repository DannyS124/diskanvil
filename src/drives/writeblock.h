// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// DiskAnvil Live's write protection: every drive but its own stick made read-only by the kernel,
// and drives plugged in later too, so nothing done can change them (evidence, a drive that mustn't
// be touched). The helper diskanvil-live-writeblock does it, as root; DiskAnvil switches it on and
// off and lets single drives be written (somewhere to save to). DiskAnvil Live only: on a PC its
// own running system can't be made read-only, and Windows writes to a drive the moment it's
// plugged in, before any program can stop it.

#include <QString>
#include <QStringList>

namespace writeblock {

bool available(); // DiskAnvil Live, with the helper
bool isOn();
QStringList allowed(); // drives let through ("sdb")

// The command that runs the helper as root: "on", "off", or "allow" and a drive.
QStringList command(const QStringList &args);

// The tests run the repo's copy, on a loop device, with its state in a folder of their own
// (DISKANVIL_WRITEBLOCK_STATE and DISKANVIL_WRITEBLOCK_ONLY are passed on to it).
inline QString helperForTest;

} // namespace writeblock
