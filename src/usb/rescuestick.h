// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "filecopy.h"
#include "isofs.h"

#include <QHash>
#include <QObject>

#include <atomic>
#include <functional>

// DiskForge Live images (the ISO rescue/build.sh makes) and the USB sticks made from them.
namespace rescue {

// /.disk/diskforge-live (/.disk/diskforge-rescue before the rename), on the image and on a stick:
//   DiskForge Live
//   version=0.5.0
//   built=2026-10-09
//   id=<build id>
struct Info {
    QString version;
    QString built;
    QString id;
    bool valid() const { return !version.isEmpty() && !id.isEmpty(); }
};
Info parseInfo(const QByteArray &text);

// sha256sum.txt: "<hash>  ./path" on each line. Paths come back without the "./".
QHash<QString, QByteArray> parseSums(const QByteArray &text);

// What's in an image file, read without mounting it.
struct Image {
    isofs::Listing listing;
    Info info;
    quint64 bytes = 0; // the files that go onto the stick
    QString error;     // set when it isn't a DiskForge Live image (or can't be read)
};
Image inspect(const QString &isoPath);

// Whether DiskForge itself is running from DiskForge Live. Everything works the same there,
// except what changes the running system itself (mounting at startup, cleaning it up, its
// snapshots and schedules): that system starts fresh from the stick every time.
bool runningInRescue();
// Running from the stick itself, not copied to memory ("Copy to memory" in the boot menu). Then
// the system needs the stick the whole time, which matters when the PC sleeps.
bool runningFromStick();
QString notInRescueReason(); // why those are off, for their tooltips
// The running rescue system's build ID (matches Info::id on its stick), or empty.
QString runningBuildId();
// For the tests: where runningInRescue() and runningFromStick() look.
extern QString rescueConfPath;
extern QString bootOptionsPath;

// On DiskForge Live: diskforge-live-rescan, through sudo. "scan" asks the disk controllers to
// look for drives again (for a drive plugged in while the PC is on), "detach sdX" lets go of a
// drive before it's unplugged. Not on a PC: there DiskForge doesn't run anything as root.
bool canRescanDrives();
void rescanDrives(const QStringList &args, QObject *context, const std::function<void(bool ok)> &done = {});
extern QString rescanHelperPath;

// A mounted stick (or any folder) with DiskForge Live on it: its info, and how many boots
// have left logs there.
Info stickInfo(const QString &root);
int logFolders(const QString &root);

// Copies a rescue image's files onto a mounted, empty FAT32 stick, with filecopy: each file
// is checked against the image's sha256sum.txt while it's copied, then read back from the
// stick and checked again. The source is the ISO file, or (from inside DiskForge Live) the
// folder of a running rescue stick. Runs in a worker thread.
class StickWriter : public QObject
{
    Q_OBJECT
public:
    // `source`: an ISO file, or a folder holding a rescue stick's files.
    StickWriter(const QString &source, const QString &stickRoot);
    void cancel();
    // GRUB's boot code for old BIOS PCs from the image (checked against its sha256sum.txt), for
    // writeBiosBoot() once the stick is unmounted. Empty when the image doesn't have it.
    QByteArray biosBoot() const { return m_biosBoot; }
    QByteArray biosCore() const { return m_biosCore; }

public slots:
    void run();

signals:
    void progress(const QString &phase, quint64 done, quint64 total);
    void finished(bool ok, const QString &message);

private:
    void finish(bool ok, const QString &message);

    QString m_source;
    QString m_root;
    QByteArray m_biosBoot, m_biosCore;
    std::atomic<bool> m_cancel{false};
    std::atomic<filecopy::Copier *> m_copier{nullptr};
};

// Makes a stick start old BIOS PCs too, the way grub-install does it: GRUB's boot code in the
// first 440 bytes of the first sector (the disk's ID and partition table stay), and its core
// image right after, in the space before the first partition. `fd` is the whole stick,
// writable, with an MBR partition table and 512-byte sectors (what Make a DiskForge Live USB makes).
bool writeBiosBoot(int fd, const QByteArray &bootImg, const QByteArray &coreImg, QString *error);

} // namespace rescue
