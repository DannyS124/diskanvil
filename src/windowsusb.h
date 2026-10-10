// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

#include <functional>

// Make a Windows USB: a Windows 10 or 11 install stick from Microsoft's ISO. The stick gets a
// FAT32 partition with the ISO's files. FAT32 can't hold a file of 4 GB or more, and install.wim
// usually is one, so it's split into .swm parts (wimlib does that), which Windows Setup reads
// as they are. FAT32 is what every PC's firmware starts from, and Microsoft's own signed boot
// files are on it, so it works with Secure Boot on. (Rufus uses NTFS plus its UEFI:NTFS loader
// instead, which is signed with a certificate some newer PCs turn off.)
//
// The Windows 11 options go into an autounattend.xml at the top of the stick, which Setup reads
// by itself.
namespace windowsusb {

// What a WIM (install.wim, install.esd, boot.wim) says about itself, from the XML in its header.
struct WimInfo {
    QString name;    // the first image's: "Windows 11 Home"
    int images = 0;
    int build = 0;   // 26100
    QString arch;    // "amd64", "arm64", "x86"
    QString error;
};
WimInfo readWim(const QString &path);
// The same from the XML itself (UTF-16 or UTF-8), for the tests.
WimInfo parseWimXml(const QByteArray &xml);

// A Windows ISO, from its mounted files.
struct Info {
    bool windows = false;
    QString install;        // "sources/install.wim" or "sources/install.esd"
    quint64 installSize = 0;
    quint64 bytes = 0;      // all the files together
    WimInfo wim;
    bool needsSplit() const;
    bool windows11() const { return wim.build >= 22000; }
};
Info inspect(const QString &root);

struct Options {
    bool skipChecks = true;      // no TPM 2.0, Secure Boot or 4 GB RAM needed
    bool noOnlineAccount = true; // no Microsoft account needed
    QString localUser;           // made at setup, with no password until the first sign-in; empty: none
    bool skipPrivacy = true;     // the privacy questions answered "no"
    bool sameRegion = false;     // language format, keyboard and time zone like this PC
    QString inputLocale;         // "0409:00000409"
    QString userLocale;          // "en-US"
    QString timeZone;            // "Central Standard Time"
    bool noBitLocker = false;    // no automatic device encryption
    bool any() const;
};

// autounattend.xml for these options (`arch` as in WimInfo), or empty when none is on.
QByteArray unattendXml(const Options &options, const QString &arch);
// A Windows account name from what was typed: characters Windows refuses become "_", names
// it keeps for itself are refused (error says why).
QString userName(const QString &typed, QString *error);
// install.wim's parts (install.swm, install2.swm...) against install.wim itself, the way a split
// leaves them: each part's resources, still compressed, compared byte for byte with the same
// ones in the original (found by their SHA-1 in both tables), and every one of them in some
// part. The parts are read from the drive, not the cache. `progress` hears how far along it is
// and stops it by giving back false. Empty when they match, otherwise what's wrong.
QString checkParts(const QString &original, const QStringList &parts,
                   const std::function<bool(quint64 done, quint64 total)> &progress = {});

// This PC's region, in Windows' terms, into `options`.
void fillRegionFromThisPc(Options &options);
// An IANA time zone ("Europe/Berlin", or an old name like "US/Central" when `tzdataLinks`,
// the system's tzdata.zi, has it) as Windows names it, or empty.
QString windowsTimeZone(const QByteArray &ianaId, const QByteArray &tzdataLinks);
// An XKB keyboard layout ("us", "de") as a Windows input locale, or empty.
QString inputLocaleFor(const QString &xkbLayout);

// The part size install.wim is split into, in MiB: under FAT32's 4 GiB limit.
constexpr int kSplitMiB = 3800;
// install.wim this big or bigger gets split: FAT32's limit (the tests lower it).
extern quint64 splitFrom;

} // namespace windowsusb
