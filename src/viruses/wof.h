// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Windows can keep its own files compressed (CompactOS, "WOF": common on small drives, like cheap
// laptops' eMMC). Windows reads them as usual; Linux's NTFS drivers can't ("Operation not
// supported"). Windows only does that to the files Windows itself is made of.

#include <QString>

namespace wof {

// Whether reading `path` fails because Windows keeps it compressed (always false on Windows,
// which reads them).
bool compressed(const QString &path);

} // namespace wof
