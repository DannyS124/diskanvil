// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Try Starting It: a drive, a clone or a disk image started in a virtual machine (QEMU), to see
// that it starts, before it's relied on. Nothing it does is kept: its writes go to a temporary
// file (QEMU's snapshot), and a drive is opened read-only.

#include <QString>
#include <QStringList>

class QObject;
class QProcess;

namespace testboot {

struct Options {
    QString file;        // a disk image
    int fd = -1;         // or a drive, opened read-only, handed to QEMU as it is (its own fd set)
    bool device = false; // a drive (a block device) rather than a file: QEMU reads those its own way
    QString format;      // "raw", "qcow2", "vmdk", "vpc" (VHD), "vhdx", "vdi"
    bool uefi = true;
    int memoryMiB = 2048;
    QString serialLog;   // where the serial port goes (the tests); empty: nowhere
    bool window = true;  // false: no window (the tests)
};

// The programs it needs, or empty when they aren't here.
QString qemu();
// UEFI's firmware for it (OVMF): its code, and the settings file to start from.
bool ovmf(QString *code, QString *vars);
bool kvm(); // whether the PC's own virtualization can be used (fast) or it's emulated (slow)
// Memory free for it now, MiB (what can be used without pushing other programs out); -1 not known.
qint64 freeMemoryMiB();
// What to give it by default: half of what's free, 1 to 4 GiB, in steps of 512 MiB.
int suggestedMemoryMiB(qint64 freeMiB);

// QEMU's format name for an image file, from what's in it.
QString formatOf(const QString &path);
// QEMU's arguments. `varsCopy`: a copy of the UEFI settings for this run.
QStringList arguments(const Options &o, const QString &ovmfCode, const QString &varsCopy, bool useKvm);

// Starts it; the process (owned by `parent`) cleans up its UEFI settings copy when it ends.
QProcess *start(const Options &o, QObject *parent, QString *error);

} // namespace testboot
