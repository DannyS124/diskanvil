// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "filecopy.h"

#include <QString>
#include <QVector>

// Copying a Linux ISO's files onto a USB stick instead of writing the ISO as it is (Rufus calls
// it ISO mode). The stick keeps a normal FAT32 partition, so it stays usable for files, and it
// can get a persistence partition that keeps changes between starts. It only starts on UEFI
// PCs: the ISO's BIOS boot code doesn't come along.
//
// The boot menus usually find the ISO by its volume label (Fedora's CDLABEL=, Arch's
// archisolabel=, GRUB's search --label). On the stick that label is the FAT one, so those lines
// get it swapped, the same way Rufus does it.
namespace isomode {

enum class Persistence {
    None,
    Casper,   // Ubuntu and friends: an ext4 partition called casper-rw, "persistent" on the kernel line
    LiveBoot, // Debian live (and DiskForge Live): one called persistence, with persistence.conf
};

struct Analysis {
    bool efi = false;       // has EFI/BOOT/BOOTX64.EFI
    bool windows = false;   // a Windows ISO: Make a Windows USB does those
    QString writeAsIs;      // why this one has to be written as it is, or empty
    QString bigFile;        // a file of 4 GB or more, which FAT32 can't hold
    Persistence persistence = Persistence::None;
    QString isoLabel;
    QString fatLabel;       // what the stick gets called
    quint64 bytes = 0;      // all the files together
    bool canCopy() const { return efi && !windows && writeAsIs.isEmpty() && bigFile.isEmpty(); }
    // Why not, in a few words, for the dialog.
    QString whyNot() const;
};

Analysis analyse(const QVector<filecopy::Entry> &entries, const QString &isoLabel);

// An ISO's label made fit for FAT32: upper case, at most 11 characters, nothing FAT refuses.
QString fatLabel(const QString &isoLabel);
// Whether `path` is a boot menu that may need patching (*.cfg, loader/entries/*.conf).
bool isBootConfig(const QString &path);
// The label swapped on the lines that look for it, and with `persistence` the switch that
// turns it on added to the kernel lines. Everything else stays as it was, line endings too.
QByteArray patchConfig(const QByteArray &text, const QString &isoLabel, const QString &fatLabel, Persistence persistence);

QString persistenceLabel(Persistence persistence); // "casper-rw", "persistence"
QByteArray persistenceConf();                      // for live-boot: "/ union"

} // namespace isomode
