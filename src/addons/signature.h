// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Checks SSH signatures, the kind `ssh-keygen -Y sign` makes. Used for the online add-on
// list: it's signed with a key that stays on the maintainer's PC, so getting into the
// GitHub account isn't enough to change it.

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace signature {

// True if `armored` (-----BEGIN SSH SIGNATURE----- ...) signs `data` for namespace `ns`
// with one of `keys` (lines like "ssh-ed25519 AAAA... comment"). Ed25519 keys only.
// error says what didn't check out.
bool verify(const QByteArray &data, const QByteArray &armored, const QString &ns, const QStringList &keys, QString *error);

} // namespace signature
