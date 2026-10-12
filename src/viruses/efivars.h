// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The PC's UEFI variables: Secure Boot's state and lists, and the boot entries. On Linux from
// efivarfs (/sys/firmware/efi/efivars), on Windows through the firmware API.

#include <QByteArray>
#include <QSet>
#include <QString>
#include <QVector>

#include <optional>

namespace efi {

inline constexpr char kGlobal[] = "8be4df61-93ca-11d2-aa0d-00e098032b8c";
inline constexpr char kSecurity[] = "d719b2cb-3d3a-4596-a3bc-dad00e67656f"; // db, dbx

// Where efivarfs is; the tests point it at a folder of their own (Linux).
extern QString varsDir;

// Whether the PC started with UEFI at all (not the old BIOS way).
bool available();
// A variable's value; nothing when it isn't there or can't be read.
std::optional<QByteArray> variable(const QString &name, const char *guid = kGlobal);

// Secure Boot's lists (EFI_SIGNATURE_LISTs): certificates (DER) and SHA-256 hashes.
struct Signatures {
    QVector<QByteArray> certificates;
    QSet<QByteArray> sha256;
};
Signatures signatures(const QByteArray &lists);

// What the firmware trusts and has banned: db and dbx. `known` is false when they can't be
// read (BIOS, a firmware that hides them, or no permission).
struct Trust {
    bool known = false;
    bool secureBoot = false;
    bool setupMode = false; // no platform key: anyone can change the lists
    Signatures allowed;     // db
    Signatures banned;      // dbx
    QVector<QByteArray> platformKeys; // PK: who may change the lists
};
Trust trust();

// A boot entry (Boot0003): its name, and the file it starts when that's on a disk.
struct BootEntry {
    int number = -1;
    bool active = true;
    QString description;
    QString partition; // the partition's GPT GUID, when it names one
    quint32 partitionNumber = 0;
    quint64 partitionStart = 0, partitionSize = 0; // in sectors
    QString path;      // "\EFI\Microsoft\Boot\bootmgfw.efi"
};
std::optional<BootEntry> parseLoadOption(int number, const QByteArray &option);
QVector<BootEntry> bootEntries(); // in BootOrder's order, then the rest

QString guidText(const char *raw16); // the usual text form of a GUID kept in Microsoft's byte order
QByteArray guidBytes(const QString &text); // and back: 16 bytes, or empty when it isn't a GUID
#ifdef Q_OS_WIN
// Writes a variable (non-volatile, seen at boot and while running); an empty value deletes it.
bool setVariable(const QString &name, const QByteArray &value, QString *error, const char *guid = kGlobal);
#endif

} // namespace efi
