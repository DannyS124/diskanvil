// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Windows' product keys, for reinstalling it: the one a PC's maker put in its firmware (the
// ACPI MSDM table, on PCs that came with Windows 8 or later), and the one a Windows was
// installed with (kept, encoded, in its registry). Many installs show a "generic" key there:
// those are activated by a digital licence tied to the PC (or a Microsoft account), and
// reinstalling the same edition activates by itself.

#include <QByteArray>
#include <QString>

namespace productkey {

// The key in an MSDM table (its whole bytes), or empty.
QString fromMsdm(const QByteArray &table);
// The key encoded in a DigitalProductId registry value, or empty.
QString decode(const QByteArray &digitalProductId);
// Microsoft's published generic keys, which install an edition without activating it.
bool isGeneric(const QString &key);

struct Installed {
    QString edition;   // "Windows 11 Pro"
    QString key;
    QString problem;   // why it couldn't be read
};
// From a Windows on a drive: its SOFTWARE registry file.
Installed fromHive(const QString &softwareHive);

#ifdef Q_OS_WIN
// The running PC's: its firmware's MSDM table, and the running Windows'.
QString firmwareKey();
Installed running();
#endif

} // namespace productkey
