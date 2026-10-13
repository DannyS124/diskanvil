// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Write-Protected Drive: why a stick, card or drive won't let anything be saved to it, in plain
// words, and the fix where a program can fix it. What's looked at:
// - the drive itself: its "write protect" answer (a card's lock switch, a stick's switch, or a
//   stick that locked itself because its memory is wearing out: nothing undoes that);
// - the system: Linux's read-only flag for it, DiskAnvil Live's write protection, Windows' policy
//   that write-protects every USB drive, its read-only marks on a disk or a volume (diskpart's
//   "attributes"), and an administrator's "deny write access" policy;
// - the file system: opened read-only, often because Linux found errors in it and stopped writing.
// Gathering it is in state() (Linux and Windows); what it means is in diagnose(), which only
// looks at what it's given, so the tests can give it anything.

#include <QString>
#include <QStringList>
#include <QVector>

namespace writeprotect {

struct Part {
    QString device;     // /dev/sdb1, or on Windows "E:" (or the volume's path when it has no letter)
    QString objectPath; // UDisks object (Windows: the partition's own id), to repair and open it again
    QString name;       // its label, or the device
    QString fsType;
    bool kernelReadOnly = false;  // Linux's flag on the partition itself
    QString mountPoint;           // where it's open; empty when it isn't
    bool mountedReadOnly = false; // open read-only
    bool attributeReadOnly = false; // Windows: the volume marked read-only (diskpart "attributes volume")
    int number = 0;                 // Windows: its partition number
};

struct State {
    QString device; // /dev/sdb, \\.\PhysicalDrive2
    QString name;   // the drive, as people know it ("SanDisk Ultra")
    bool sdCard = false;        // a memory card, in a reader or a slot
    bool cardSlot = false;      // in the PC's own slot (Linux mmcblk): its lock switch is read straight
    bool kernelReadOnly = false;  // Linux won't write to the whole drive
    int drivesays = -1;           // the drive's own answer: 1 write-protected, 0 not, -1 couldn't ask
    bool writeBlock = false;      // DiskAnvil Live's write protection holds it
    bool policyWriteProtect = false; // Windows: StorageDevicePolicies\WriteProtect (every USB drive)
    QStringList policyDenyWrite;     // Windows: an administrator's policies that deny writing (their names)
    bool diskAttributeReadOnly = false; // Windows: the disk marked read-only (diskpart "attributes disk")
    int diskNumber = -1;                // Windows
    QVector<Part> parts;
    bool windows = false;
};

enum class Fix {
    None,
    AllowWriteBlock,   // DiskAnvil Live lets this drive be written
    ClearKernelFlag,   // blockdev --setrw, as root
    RepairAndReopen,   // close it, repair its file system, open it again
    Reopen,            // close it and open it again, for writing
    ClearPolicy,       // Windows: StorageDevicePolicies\WriteProtect back to 0
    ClearDiskAttribute,   // Windows: the disk's read-only mark taken off
    ClearVolumeAttribute, // Windows: a volume's
};

struct Finding {
    QString title; // short: "The card's lock switch is on"
    QString text;  // what it means and what to do, in plain words
    Fix fix = Fix::None;
    QString fixLabel; // the button: "Allow Writing to It"
    int part = -1;    // which of State::parts it's about; -1 the whole drive
    bool stops = true; // it alone stops writing (false: worth knowing)
};

// What stops writing to it, the cause nearest the drive first; empty when nothing does.
QVector<Finding> diagnose(const State &state);
// When nothing does: the other things that look like write protection.
QString nothingFound(const State &state);

// Linux: the drive's answer to MODE SENSE (the "write protect" bit), from an fd open on it;
// -1 when it isn't a SCSI or USB drive or didn't answer.
int askDrive(int fd);
// Linux: the flags and mounts read from /sys and /proc. `sysRoot` and `mountInfo` are for the
// tests ("/sys", "/proc/self/mountinfo").
void readLinux(State &state, const QString &sysRoot = QStringLiteral("/sys"), const QString &mountInfo = QStringLiteral("/proc/self/mountinfo"));
// The root command for ClearKernelFlag: blockdev --setrw on the drive and its partitions.
QStringList clearKernelFlagScript(const State &state);

#ifdef Q_OS_WIN
// Windows: the policies, the disk's read-only mark and its answer, and the volumes' marks
// (blocking; a PowerShell for the volumes).
void readWindows(State &state);
// The fixes, as administrator (DiskAnvil runs as one there). Empty when done, or Windows' words.
QString clearPolicy();
QString clearDiskAttribute(int diskNumber);
QString clearVolumeAttribute(int diskNumber, int partitionNumber);
#endif

} // namespace writeprotect
