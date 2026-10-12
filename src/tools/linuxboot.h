// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Repair Linux Boot (DiskAnvil Live): a Linux that doesn't start any more because its boot
// loader is gone or broken (Windows' updates overwrite it, a firmware reset drops its boot entry,
// a failed update leaves GRUB half done). Its own tools put it back, run inside it (a chroot):
// GRUB (grub-install and its menu), GRUB 2 the Fedora and openSUSE way (shim's boot entry back,
// and its menu), or systemd-boot (bootctl install).

#include "udisks.h"

#include <QString>
#include <QStringList>

namespace linuxboot {

struct FstabEntry {
    QString what;  // "UUID=...", "PARTUUID=...", "LABEL=...", "/dev/sda1"
    QString where; // "/boot/efi"
    QString type;
    QString options;
};
QVector<FstabEntry> parseFstab(const QByteArray &fstab);
// The device a "what" names, from what's known of the drives; empty when it isn't here.
QString deviceFor(const QString &what, const QVector<Disk> &disks);

struct Install {
    enum class Loader { Unknown, Grub, Grub2, SystemdBoot };
    QString name;        // "Debian GNU/Linux 13 (trixie)"
    QString id;          // "debian"
    QString rootDevice;  // /dev/sda2
    QString subvolume;   // Btrfs: the root's subvolume ("@"), empty for the top
    QString bootDevice;  // a /boot of its own, from its fstab
    QString espDevice;   // its EFI partition, from its fstab
    QString espMount;    // where it mounts that: /boot/efi, /efi or /boot
    QString disk;        // the drive the root is on (/dev/sda), for BIOS boot code
    int espPartition = 0;
    QString espDisk;
    Loader loader = Loader::Unknown;
    QStringList problems; // what's missing to repair it
};

// A Linux install whose root is the folder `root` (where its volume is mounted, plus its
// subvolume's folder for Btrfs). `rootDevice`/`subvolume`: what it is.
Install inspect(const QString &root, const QString &rootDevice, const QString &subvolume, const QVector<Disk> &disks);
// The subvolume folders a Btrfs top level might hold a root in.
QStringList rootSubvolumes(const QString &top);

// The script that repairs it, run as root: mounts it at /mnt/diskanvil-repair, runs its own
// tools in a chroot, unmounts it all again.
QString repairScript(const Install &install, bool uefi);
// What it does, in plain words.
QStringList plan(const Install &install, bool uefi);

} // namespace linuxboot
