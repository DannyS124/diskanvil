// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "newids.h"

#include "blockio.h"
#include "format.h"
#include "gpt.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSet>
#include <QtEndian>

namespace newids {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("newids", text);
}

// IDs that several drives share on purpose: the members of one RAID array, ZFS pool or
// Btrfs file system spread over drives. And sticks written from the same ISO are copies,
// but nothing goes wrong with them.
bool sharedOnPurpose(const QString &fsType)
{
    static const QSet<QString> kinds = {QStringLiteral("linux_raid_member"), QStringLiteral("zfs_member"), QStringLiteral("btrfs"),
                                        QStringLiteral("iso9660"), QStringLiteral("udf"), QStringLiteral("ddf_raid_member"),
                                        QStringLiteral("isw_raid_member")};
    return kinds.contains(fsType);
}

bool writtenFromAnImage(const Disk &d)
{
    for (const Volume &v : d.volumes) {
        if (v.fsType == QLatin1String("iso9660") || v.fsType == QLatin1String("udf"))
            return true;
    }
    return false;
}

} // namespace

QVector<Clash> clashes(const QVector<Disk> &disks)
{
    QVector<Clash> out;
    for (qsizetype i = 0; i < disks.size(); ++i) {
        for (qsizetype j = i + 1; j < disks.size(); ++j) {
            const Disk &a = disks[i], &b = disks[j];
            if (writtenFromAnImage(a) || writtenFromAnImage(b) || a.isRaid || b.isRaid)
                continue;
            Clash c;
            c.first = a.blockPath;
            c.second = b.blockPath;
            if (!a.tableId.isEmpty() && a.tableId.compare(b.tableId, Qt::CaseInsensitive) == 0)
                c.ids << tr("the disk ID");
            int partitions = 0;
            QStringList fileSystems;
            for (const Volume &v : a.volumes) {
                for (const Volume &w : b.volumes) {
                    if (!v.partUuid.isEmpty() && v.partUuid.compare(w.partUuid, Qt::CaseInsensitive) == 0)
                        ++partitions;
                    if (!v.uuid.isEmpty() && v.uuid.compare(w.uuid, Qt::CaseInsensitive) == 0 && !sharedOnPurpose(v.fsType))
                        fileSystems << tr("%1's file system (%2)").arg(shortDevice(v.device), v.fsType);
                }
            }
            if (partitions > 0)
                c.ids << QCoreApplication::translate("newids", "%n partition ID(s)", nullptr, partitions);
            c.ids << fileSystems;
            if (!c.ids.isEmpty())
                out << c;
        }
    }
    return out;
}

QString clashKey(const Clash &clash, const QVector<Disk> &disks)
{
    // The drives by what they are (not where they're plugged in), and what they share.
    QStringList parts;
    for (const Disk &d : disks) {
        if (d.blockPath == clash.first || d.blockPath == clash.second)
            parts << d.model + QLatin1Char('/') + d.serial + QLatin1Char('/') + d.tableId;
    }
    parts.sort();
    parts << clash.ids;
    return QString::fromLatin1(QCryptographicHash::hash(parts.join(QLatin1Char('|')).toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
}

bool giveDiskNewIds(int fd, bool gpt, int sectorSize, QString *error)
{
    if (gpt) {
        const gpt::Result r = gpt::relocateBackup(fd, sectorSize, true);
        if (!r.ok)
            *error = r.error;
        return r.ok;
    }
    // MBR: the signature at byte 440.
    const quint64 sector = quint64(sectorSize > 0 ? sectorSize : blockio::logicalSize(fd));
    blockio::Buffer mbr = blockio::alignedBuffer(std::max<size_t>(sector, blockio::kAlign));
    if (!mbr || !blockio::readAt(fd, mbr.get(), sector, 0)) {
        *error = tr("its first sector couldn't be read");
        return false;
    }
    quint32 signature = 0;
    while (signature == 0) // 0 means "none" to Windows
        signature = QRandomGenerator::system()->generate();
    qToLittleEndian(signature, mbr.get() + 440);
    if (!blockio::writeAt(fd, mbr.get(), sector, 0) || !blockio::flush(fd)) {
        *error = tr("its first sector couldn't be written");
        return false;
    }
    return true;
}

qsizetype bootBytes(const QString &fsType, int sectorSize)
{
    if (fsType == QLatin1String("vfat"))
        return 7 * qsizetype(sectorSize);  // FAT32 keeps a backup of its boot sector in sector 6
    if (fsType == QLatin1String("exfat"))
        return 24 * qsizetype(sectorSize); // the boot region and its backup
    return sectorSize;
}

bool setSerial(QByteArray &boot, const QString &fsType, const QString &serial, QString *error)
{
    bool ok = false;
    const quint64 value = QString(serial).remove(QLatin1Char('-')).toULongLong(&ok, 16);
    auto u16 = [&boot](qsizetype at) { return qFromLittleEndian<quint16>(boot.constData() + at); };
    if (!ok) {
        *error = tr("%1 isn't a serial number").arg(serial);
        return false;
    }
    if (fsType == QLatin1String("ntfs")) {
        if (boot.size() < 512 || boot.mid(3, 8) != "NTFS    ") {
            *error = tr("it isn't an NTFS boot sector");
            return false;
        }
        qToLittleEndian<quint64>(value, boot.data() + 0x48);
        return true;
    }
    if (fsType == QLatin1String("vfat")) {
        if (boot.size() < 512 || uchar(boot[510]) != 0x55 || uchar(boot[511]) != 0xAA) {
            *error = tr("it isn't a FAT boot sector");
            return false;
        }
        const qsizetype sector = u16(11);
        const bool fat32 = u16(0x16) == 0; // FAT32 says how big its FATs are elsewhere
        const qsizetype at = fat32 ? 0x43 : 0x27;
        qToLittleEndian<quint32>(quint32(value), boot.data() + at);
        const qsizetype backup = fat32 ? u16(0x32) : 0;
        if (backup > 0 && backup < 16 && sector >= 512 && (backup + 1) * sector <= boot.size() && boot.mid(backup * sector + 3, 8) == boot.mid(3, 8))
            qToLittleEndian<quint32>(quint32(value), boot.data() + backup * sector + at);
        return true;
    }
    if (fsType == QLatin1String("exfat")) {
        if (boot.size() < 512 || boot.mid(3, 8) != "EXFAT   ") {
            *error = tr("it isn't an exFAT boot sector");
            return false;
        }
        const int shift = uchar(boot[108]);
        const qsizetype sector = shift >= 9 && shift <= 12 ? qsizetype(1) << shift : 0;
        if (sector == 0 || boot.size() < 24 * sector) {
            *error = tr("its boot region isn't all there");
            return false;
        }
        for (const qsizetype region : {qsizetype(0), 12 * sector}) {
            char *b = boot.data() + region;
            qToLittleEndian<quint32>(quint32(value), b + 100);
            // The checksum over the first 11 sectors, leaving out the bytes that change while
            // it's in use (VolumeFlags, PercentInUse), fills sector 11.
            quint32 sum = 0;
            for (qsizetype i = 0; i < 11 * sector; ++i) {
                if (i == 106 || i == 107 || i == 112)
                    continue;
                sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + uchar(b[i]);
            }
            for (qsizetype i = 0; i < sector; i += 4)
                qToLittleEndian<quint32>(sum, b + 11 * sector + i);
        }
        return true;
    }
    *error = tr("%1 isn't a file system whose serial number this can change").arg(fsType);
    return false;
}

QStringList filesNaming(const QString &root, const QStringList &ids)
{
    QStringList candidates = {QStringLiteral("etc/fstab"),          QStringLiteral("etc/crypttab"),      QStringLiteral("etc/kernel/cmdline"),
                              QStringLiteral("etc/default/grub"),   QStringLiteral("boot/grub/grub.cfg"), QStringLiteral("boot/grub2/grub.cfg"),
                              QStringLiteral("grub/grub.cfg"),      QStringLiteral("grub2/grub.cfg"),    QStringLiteral("boot/refind_linux.conf"),
                              QStringLiteral("refind_linux.conf"),  QStringLiteral("etc/initramfs-tools/conf.d/resume")};
    for (const QString &entries : {QStringLiteral("boot/loader/entries"), QStringLiteral("loader/entries")}) {
        for (const QString &name : QDir(root + QLatin1Char('/') + entries).entryList({QStringLiteral("*.conf")}, QDir::Files))
            candidates << entries + QLatin1Char('/') + name;
    }
    QStringList out;
    for (const QString &relative : std::as_const(candidates)) {
        const QString path = root + QLatin1Char('/') + relative;
        QFile f(path);
        if (QFileInfo(path).isSymLink() || f.size() > 4 * 1024 * 1024 || !f.open(QIODevice::ReadOnly))
            continue;
        const QString text = QString::fromUtf8(f.readAll());
        for (const QString &id : ids) {
            if (!id.isEmpty() && text.contains(id, Qt::CaseInsensitive)) {
                out << path;
                break;
            }
        }
    }
    return out;
}

QStringList rewrite(const QStringList &files, const QHash<QString, QString> &renamed, QString *error)
{
    QStringList changed;
    const QString stamp = QStringLiteral(".diskforge-") + QDate::currentDate().toString(Qt::ISODate);
    for (const QString &path : files) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            *error = tr("%1 couldn't be read: %2").arg(path, f.errorString());
            continue;
        }
        const QByteArray before = f.readAll();
        f.close();
        QString text = QString::fromUtf8(before);
        for (auto it = renamed.cbegin(); it != renamed.cend(); ++it) {
            if (!it.key().isEmpty())
                text.replace(it.key(), it.value(), Qt::CaseInsensitive);
        }
        const QByteArray after = text.toUtf8();
        if (after == before)
            continue;
        // As it was, next to it; then the new one written beside and swapped in, so a
        // power cut leaves one whole file or the other.
        QFile::remove(path + stamp);
        if (!QFile::copy(path, path + stamp)) {
            *error = tr("%1 couldn't be backed up, so it wasn't changed").arg(path);
            continue;
        }
        QFile next(path + QStringLiteral(".diskforge-new"));
        if (!next.open(QIODevice::WriteOnly) || next.write(after) != after.size()) {
            *error = tr("%1 couldn't be written: %2").arg(path, next.errorString());
            next.remove();
            continue;
        }
        next.setPermissions(QFile::permissions(path));
        next.close();
        if (!QFile::remove(path) || !QFile::rename(next.fileName(), path)) {
            *error = tr("%1 couldn't be replaced").arg(path);
            continue;
        }
        changed << path;
    }
    return changed;
}

} // namespace newids
