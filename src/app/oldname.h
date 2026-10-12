// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// DiskAnvil was called DiskForge up to 0.5.1. What that kept is moved over the first time
// DiskAnvil starts: its settings (the add-ons' too), and its folders (the virus list, add-ons,
// partition layouts, logs, certificates of erasure). The old settings stay, for an older version.

#include <QStringList>

namespace oldname {

struct Moved {
    bool settings = false;
    QStringList folders; // where they are now
};

Moved moveOver();

} // namespace oldname
