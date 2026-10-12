// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Windows can keep files compressed (WOF): its own with CompactOS (common on small drives, like
// cheap laptops' eMMC), and any others someone runs compact.exe /exe on (CompactGUI does that to
// games). Windows reads them as usual. Linux's ntfs3 reads them when it's built to (Arch's is) and
// otherwise says "Operation not supported" (Debian's); ntfs-3g shows them as links to "unsupported
// reparse tag 0x80000017" unless its system compression plugin is there.

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QString>

#include <memory>

namespace lost {
class Source;
}

namespace wof {

// Whether `path` can't be read because Windows keeps it compressed (always false on Windows,
// which reads them).
bool compressed(const QString &path);
// When it was last changed (ntfs-3g's link has the file's time; QFileInfo would follow the link).
QDateTime changed(const QString &path);

// Whether `file` is in the Windows folder of the Windows at `root` (in any case): compressed files
// there are CompactOS's, so Windows' own. Elsewhere anyone may have compressed them.
bool inWindowsFolder(const QString &root, const QString &file);

// What Linux can't read, read by DiskAnvil itself: a compressed file's bytes, unpacked from the
// drive (opened read-only; through UDisks when DiskAnvil may not open it itself, which can ask
// for the password on a PC, not on DiskAnvil Live). It finds the file by the record number its
// driver gives as its inode number, and checks the name. Use one for a whole window or scan: it
// keeps the drives it opened until it goes. On Windows there's nothing to do (it fails).
class Reader
{
public:
    Reader();
    ~Reader();
    Reader(const Reader &) = delete;
    Reader &operator=(const Reader &) = delete;

    // How big it is unpacked; -1 when it can't be read.
    qint64 size(const QString &path, QString *why);
    // The file's bytes, when it isn't bigger than `most`.
    bool read(const QString &path, QByteArray *out, QString *why, quint64 most = 256ull * 1024 * 1024);
    // A copy of it at `to`, a new file, with its time.
    bool copy(const QString &path, const QString &to, QString *why);

private:
    std::shared_ptr<lost::Source> open(const QString &path, QString *why);

    QHash<QString, std::shared_ptr<lost::Source>> m_drives; // by device
    QHash<QString, QString> m_failed;                       // devices that wouldn't open, and why
};

// Whether UDisks may ask for the password when a Reader opens a drive: yes in the app, never in
// the self-tests.
void setInteractive(bool interactive);

} // namespace wof
