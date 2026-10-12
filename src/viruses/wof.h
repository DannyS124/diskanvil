// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Windows can keep files compressed (WOF): its own with CompactOS (common on small drives, like
// cheap laptops' eMMC), and any others someone runs compact.exe /exe on (CompactGUI does that to
// games). Windows reads them as usual. Linux's ntfs3 reads them when it's built to (Arch's is) and
// otherwise says "Operation not supported" (Debian's); ntfs-3g shows them as links to "unsupported
// reparse tag 0x80000017" unless its system compression plugin is there.

#include <QDateTime>
#include <QString>

namespace wof {

// Whether `path` can't be read because Windows keeps it compressed (always false on Windows,
// which reads them).
bool compressed(const QString &path);
// When it was last changed (ntfs-3g's link has the file's time; QFileInfo would follow the link).
QDateTime changed(const QString &path);

// Whether `file` is in the Windows folder of the Windows at `root` (in any case): compressed files
// there are CompactOS's, so Windows' own. Elsewhere anyone may have compressed them.
bool inWindowsFolder(const QString &root, const QString &file);

} // namespace wof
