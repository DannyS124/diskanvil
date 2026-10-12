// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A LUKS drive's header holds its keys, wrapped by each passphrase. If it's damaged (a stray
// write, a bad sector there, a "quick format"), nothing on the drive can be read again, whatever
// the passphrase. A copy of it, kept elsewhere, puts that right. The copy is the same bytes
// cryptsetup's luksHeaderBackup keeps (the LUKS1 header up to its data, or LUKS2's two headers
// and keyslots area), so either can restore the other's.

#include <QByteArray>
#include <QString>

namespace luksheader {

struct Info {
    int version = 0;       // 1 or 2; 0: not a LUKS header
    QString uuid;
    quint64 size = 0;      // the bytes a backup holds
    quint64 dataOffset = 0; // where the encrypted data starts (0: kept elsewhere)
    QString problem;
    bool valid() const { return version > 0 && size > 0 && problem.isEmpty(); }
};

// How much of the start has to be read to know the rest (LUKS2's header size), from its first
// 4096 bytes; 0 when it isn't LUKS.
quint64 needed(const QByteArray &first4k);
// From at least needed() bytes.
Info inspect(const QByteArray &head);

// The header's backup, read from the start of `fd` (a drive's, O_DIRECT or not, or a file's).
QByteArray read(int fd, Info *info);
// Writes a backup over the start of `fd`, and checks it reads back the same.
bool write(int fd, const QByteArray &backup, QString *error);

} // namespace luksheader
