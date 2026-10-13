// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Self-encrypting drives (TCG Opal and its cousins): whether a drive encrypts in hardware, whether
// its locking is switched on, and whether it's locked right now, from the drive's own answer
// (Level 0 Discovery, read-only). And the way out for a second-hand drive locked with a password
// nobody has: a PSID revert, with the 32-character PSID printed on the drive's own label. It puts
// the drive back as it left the factory and erases everything on it (the encryption key is
// replaced, so what's there can never be read again). Only someone holding the drive can read
// the label: it's a reset, not a way in.
//
// Our own code, from the Trusted Computing Group's published specifications (TCG Storage
// Architecture Core, Opal SSC). It talks to the drive through SCSI SECURITY PROTOCOL IN/OUT (SATA
// drives through Linux's libata or Windows' own translation, SAS and USB bridges that pass it on)
// or NVMe Security Send/Receive.

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

namespace opal {

// What the drive says about itself.
struct Discovery {
    bool answered = false;     // the drive understood the question
    QString ssc;               // "Opal 2", "Opal 1", "Enterprise", "Pyrite 2", "Pyrite 1", "Opalite", "Ruby"; empty: none
    quint16 baseComId = 0;
    bool lockingSupported = false;
    bool lockingEnabled = false; // someone switched its locking on (a password was set)
    bool locked = false;         // a range is locked right now
    bool mediaEncryption = false; // it encrypts everything it stores
    bool mbrEnabled = false;      // it shows a small pre-boot disk until it's unlocked ("shadow MBR")
    bool mbrDone = false;
    bool blockSid = false;        // the firmware blocked taking ownership until the next power cycle
    bool psidPossible = false;    // a set of features that has a PSID revert
};
Discovery parseDiscovery(const QByteArray &level0);

// Talking to the drive: IF-SEND and IF-RECV for security protocol 1 and a ComID. `data` is a
// multiple of 512 bytes. False when the drive (or the way to it) refuses.
struct Transport {
    std::function<bool(quint16 comId, const QByteArray &data)> send;
    std::function<bool(quint16 comId, QByteArray *data, int length)> receive;
    std::function<bool(QByteArray *level0)> discover; // protocol 1, ComID 1
};

// Opens the drive (/dev/sdb, /dev/nvme0n1; Windows: \\.\PhysicalDrive1) and gives a transport for
// it. Needs root (Linux) or the administrator (Windows). Empty functions, with why, when it can't.
Transport open(const QString &device, QString *why);
void close(Transport &transport);

Discovery discover(Transport &t, QString *why);

// The PSID revert. Status 0 is done; otherwise the drive's status code, and `why` in words.
enum class RevertResult { Done, WrongPsid, NotSupported, Failed };
RevertResult revertWithPsid(Transport &t, const Discovery &d, const QString &psid, QString *why);

// A PSID as typed: letters and digits only, 32 of them (some labels split it with dashes or spaces).
QString cleanPsid(const QString &typed);

// The drive's state in plain words, for the window.
QStringList describe(const Discovery &d);

// What's put on the wire, for the tests: the session manager's StartSession, and a method call.
QByteArray startSessionPayload(quint32 hostSession, const QByteArray &psid);
QByteArray revertPayload();
QByteArray packet(quint16 comId, quint32 tsn, quint32 hsn, const QByteArray &payload);
// The SP's session number from SyncSession's answer; 0 when it isn't one.
quint32 spSessionFrom(const QByteArray &reply, quint8 *status);
// The status at the end of a method's answer (0 success); -1 when it can't be found.
int methodStatus(const QByteArray &reply);

// The hidden option the window runs as root: `diskanvil --drive-opal <device> status|revert`,
// the PSID on standard input. Prints "key=value" lines.
inline constexpr char kOption[] = "--drive-opal";
int runOption(const QString &device, const QString &what);
QString discoveryToLines(const Discovery &d);
Discovery discoveryFromLines(const QString &lines);

} // namespace opal
