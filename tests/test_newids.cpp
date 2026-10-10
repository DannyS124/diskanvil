// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// New IDs: telling copies apart from drives that share IDs on purpose, and fixing what a Linux
// on the copy names by the old IDs.

#include "testkit.h"

#include "../src/imagesource.h"
#include "../src/newids.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace {

QByteArray unpackImage(const QString &path)
{
    QString error;
    const std::unique_ptr<ImageSource> image = ImageSource::open(path, &error);
    QByteArray out;
    if (!image)
        return out;
    QByteArray buffer(1 << 20, Qt::Uninitialized);
    for (qint64 n; (n = image->read(buffer.data(), buffer.size())) > 0;)
        out += buffer.left(n);
    return out;
}

Volume volume(const QString &device, const QString &fsType, const QString &uuid, const QString &partUuid)
{
    Volume v;
    v.device = device;
    v.objectPath = device;
    v.fsType = fsType;
    v.uuid = uuid;
    v.partUuid = partUuid;
    v.hasFilesystem = !fsType.isEmpty();
    return v;
}

Disk disk(const QString &device, const QString &tableId, const QVector<Volume> &volumes)
{
    Disk d;
    d.device = device;
    d.blockPath = device;
    d.tableType = QStringLiteral("gpt");
    d.tableId = tableId;
    d.volumes = volumes;
    return d;
}

void clashes()
{
    const QString diskId = QStringLiteral("1b6f3c2a-0000-4e2b-9c4d-aaaaaaaaaaaa");
    const Disk original = disk(QStringLiteral("/dev/sda"), diskId,
                               {volume(QStringLiteral("/dev/sda1"), QStringLiteral("vfat"), QStringLiteral("ABCD-1234"), QStringLiteral("p1-guid")),
                                volume(QStringLiteral("/dev/sda2"), QStringLiteral("ext4"), QStringLiteral("e4e4e4e4-1111-2222-3333-444444444444"),
                                       QStringLiteral("p2-guid"))});
    Disk copy = original;
    copy.device = copy.blockPath = QStringLiteral("/dev/sdb");
    for (Volume &v : copy.volumes)
        v.device.replace(QLatin1String("sda"), QLatin1String("sdb"));
    const Disk other = disk(QStringLiteral("/dev/sdc"), QStringLiteral("99999999-0000-4e2b-9c4d-bbbbbbbbbbbb"),
                            {volume(QStringLiteral("/dev/sdc1"), QStringLiteral("ntfs"), QStringLiteral("0123456789ABCDEF"), QStringLiteral("p9-guid"))});

    QVector<newids::Clash> found = newids::clashes({original, copy, other});
    report(found.size() == 1 && found[0].first == QLatin1String("/dev/sda") && found[0].second == QLatin1String("/dev/sdb")
               && found[0].ids.contains(QLatin1String("the disk ID")) && found[0].ids.size() == 4,
           QStringLiteral("new IDs: a clone and its original are found, with what they share"),
           found.isEmpty() ? QString() : found[0].ids.join(QStringLiteral(", ")));

    // Shared on purpose: RAID members, a Btrfs over two drives, two sticks from one ISO.
    Disk raidA = disk(QStringLiteral("/dev/sdd"), QStringLiteral("d1"),
                      {volume(QStringLiteral("/dev/sdd1"), QStringLiteral("linux_raid_member"), QStringLiteral("raid-uuid"), QStringLiteral("rd1"))});
    Disk raidB = disk(QStringLiteral("/dev/sde"), QStringLiteral("d2"),
                      {volume(QStringLiteral("/dev/sde1"), QStringLiteral("linux_raid_member"), QStringLiteral("raid-uuid"), QStringLiteral("rd2"))});
    Disk btrfsA = disk(QStringLiteral("/dev/sdf"), QStringLiteral("d3"),
                       {volume(QStringLiteral("/dev/sdf1"), QStringLiteral("btrfs"), QStringLiteral("btrfs-uuid"), QStringLiteral("b1"))});
    Disk btrfsB = disk(QStringLiteral("/dev/sdg"), QStringLiteral("d4"),
                       {volume(QStringLiteral("/dev/sdg1"), QStringLiteral("btrfs"), QStringLiteral("btrfs-uuid"), QStringLiteral("b2"))});
    Disk isoA = disk(QStringLiteral("/dev/sdh"), QStringLiteral("5f8e2a10"),
                     {volume(QStringLiteral("/dev/sdh1"), QStringLiteral("iso9660"), QStringLiteral("2026-09-01-12-00-00-00"), QStringLiteral("5f8e2a10-01"))});
    Disk isoB = isoA;
    isoB.device = isoB.blockPath = QStringLiteral("/dev/sdi");
    found = newids::clashes({raidA, raidB, btrfsA, btrfsB, isoA, isoB});
    report(found.isEmpty(), QStringLiteral("new IDs: RAID, multi-drive Btrfs and sticks written from one ISO aren't called copies"),
           found.isEmpty() ? QString() : found[0].ids.join(QStringLiteral(", ")));

    const QString key = newids::clashKey(newids::clashes({original, copy}).value(0), {original, copy});
    copy.device = copy.blockPath = QStringLiteral("/dev/sdx"); // plugged in elsewhere next time
    report(key == newids::clashKey(newids::clashes({original, copy}).value(0), {original, copy}),
           QStringLiteral("new IDs: dismissing a pair holds when they're plugged in elsewhere"));
}

void settings(const QString &dir)
{
    const QString root = dir + QStringLiteral("/linux");
    auto put = [&root](const QString &path, const QByteArray &data) {
        QDir().mkpath(QFileInfo(root + QLatin1Char('/') + path).absolutePath());
        QFile f(root + QLatin1Char('/') + path);
        if (f.open(QIODevice::WriteOnly))
            f.write(data);
    };
    put(QStringLiteral("etc/fstab"), "UUID=e4e4e4e4-1111-2222-3333-444444444444 / ext4 defaults 0 1\n"
                                     "UUID=ABCD-1234 /boot vfat umask=0077 0 2\n"
                                     "UUID=55555555-6666-7777-8888-999999999999 none swap defaults 0 0\n");
    put(QStringLiteral("boot/loader/entries/arch.conf"), "title Arch\nlinux /vmlinuz-linux\noptions root=PARTUUID=0a1b2c3d-p2 rw\n");
    put(QStringLiteral("boot/grub/grub.cfg"), "search --fs-uuid --set=root E4E4E4E4-1111-2222-3333-444444444444\n");
    put(QStringLiteral("etc/hostname"), "e4e4e4e4-1111-2222-3333-444444444444 isn't a setting this looks at\n");
    const QHash<QString, QString> renamed = {
        {QStringLiteral("e4e4e4e4-1111-2222-3333-444444444444"), QStringLiteral("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")},
        {QStringLiteral("ABCD-1234"), QStringLiteral("9F2E-77C1")},
        {QStringLiteral("0a1b2c3d-p2"), QStringLiteral("feedf00d-p2")},
    };
    QStringList files = newids::filesNaming(root, renamed.keys());
    files.sort();
    report(files == QStringList{root + QStringLiteral("/boot/grub/grub.cfg"), root + QStringLiteral("/boot/loader/entries/arch.conf"),
                                root + QStringLiteral("/etc/fstab")},
           QStringLiteral("new IDs: the Linux settings that name the old IDs are found"), files.join(QStringLiteral(", ")));
    QString error;
    const QStringList changed = newids::rewrite(files, renamed, &error);
    QFile fstab(root + QStringLiteral("/etc/fstab"));
    const QString text = fstab.open(QIODevice::ReadOnly) ? QString::fromUtf8(fstab.readAll()) : QString();
    QFile grub(root + QStringLiteral("/boot/grub/grub.cfg"));
    const QString menu = grub.open(QIODevice::ReadOnly) ? QString::fromUtf8(grub.readAll()) : QString();
    const QStringList kept = QDir(root + QStringLiteral("/etc")).entryList({QStringLiteral("fstab.diskforge-*")});
    report(changed.size() == 3 && error.isEmpty() && text.contains(QLatin1String("UUID=aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee /"))
               && text.contains(QLatin1String("UUID=9F2E-77C1 /boot")) && text.contains(QLatin1String("UUID=55555555-6666-7777-8888-999999999999 none swap"))
               && menu.contains(QLatin1String("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")) && kept.size() == 1,
           QStringLiteral("new IDs: they're rewritten with the new IDs, any case, and kept as they were"), error);
}

// Serial numbers written into real file systems (the Find Lost Files test images: made by
// mkfs, and one by Windows), then checked by blkid and each file system's own checker.
void serials(const QString &dir)
{
    struct Image {
        const char *file;
        const char *fsType;
        const char *serial;
        QStringList checker;
    };
    const QVector<Image> images = {
        {"fat12.img.zst", "vfat", "1A2B-3C4D", {QStringLiteral("fsck.vfat"), QStringLiteral("-n")}},
        {"fat32.img.zst", "vfat", "5E6F-7081", {QStringLiteral("fsck.vfat"), QStringLiteral("-n")}},
        {"exfat.img.zst", "exfat", "9ABC-DEF0", {QStringLiteral("fsck.exfat"), QStringLiteral("-n")}},
        {"ntfs-windows.img.zst", "ntfs", "0123456789ABCDEF", {QStringLiteral("ntfsfix"), QStringLiteral("-n")}},
    };
    for (const Image &image : images) {
        const QByteArray original = unpackImage(QStringLiteral(SOURCE_DIR "/tests/data/lostfs/") + QLatin1String(image.file));
        QByteArray bytes = original;
        const QString fs = QLatin1String(image.fsType);
        QByteArray boot = bytes.left(newids::bootBytes(fs, 512));
        QString error;
        bool ok = !bytes.isEmpty() && newids::setSerial(boot, fs, QLatin1String(image.serial), &error);
        bytes.replace(0, boot.size(), boot);
        if (ok && fs == QLatin1String("ntfs")) {
            // NTFS's copy of its boot sector, at the very end.
            QByteArray last = bytes.right(512);
            ok = newids::setSerial(last, fs, QLatin1String(image.serial), &error);
            bytes.replace(bytes.size() - 512, 512, last);
        }
        const QString path = dir + QLatin1Char('/') + QLatin1String(image.file).chopped(4);
        QFile f(path);
        ok = ok && f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
        f.close();
        const QString seen = sh(QStringLiteral("blkid"), {QStringLiteral("-p"), QStringLiteral("-o"), QStringLiteral("value"), QStringLiteral("-s"),
                                                          QStringLiteral("UUID"), path}).trimmed();
        int code = -1;
        QString checked = QStringLiteral("(no %1 here)").arg(image.checker.first());
        if (!QStandardPaths::findExecutable(image.checker.first()).isEmpty())
            checked = sh(image.checker.first(), image.checker.mid(1) + QStringList{path}, &code).simplified().right(120);
        int differing = 0;
        for (qsizetype i = 0; i < bytes.size() && i < original.size(); ++i)
            differing += bytes[i] != original[i];
        report(ok && seen.compare(QLatin1String(image.serial), Qt::CaseInsensitive) == 0 && (code == 0 || checked.startsWith(QLatin1Char('(')))
                   && differing <= (fs == QLatin1String("exfat") ? 2 * (4 + 512) : 40), // exFAT: its checksum sectors too
               QStringLiteral("new IDs: a new serial number in %1, and the file system's checker is happy").arg(QLatin1String(image.file)),
               QStringLiteral("%1 | %2 bytes changed | %3 %4").arg(seen).arg(differing).arg(checked, error));
    }
    QByteArray junk(4096, 'x');
    QString error;
    report(!newids::setSerial(junk, QStringLiteral("ntfs"), QStringLiteral("0123456789ABCDEF"), &error) && !error.isEmpty(),
           QStringLiteral("new IDs: something that isn't the file system is left alone"));
}

} // namespace

void newIdsTests()
{
    QTemporaryDir dir;
    clashes();
    settings(dir.path());
    serials(dir.path());
}
