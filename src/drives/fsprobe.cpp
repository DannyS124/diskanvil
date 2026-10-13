// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fsprobe.h"

#include <QtEndian>

#include <cstring>
#include <limits>

namespace fsprobe {

namespace {

bool has(const QByteArray &head, int offset, const char *magic, int length)
{
    return offset >= 0 && head.size() >= offset + length && std::memcmp(head.constData() + offset, magic, size_t(length)) == 0;
}

quint16 le16(const QByteArray &head, int offset)
{
    return head.size() >= offset + 2 ? qFromLittleEndian<quint16>(head.constData() + offset) : 0;
}

quint32 le32(const QByteArray &head, int offset)
{
    return head.size() >= offset + 4 ? qFromLittleEndian<quint32>(head.constData() + offset) : 0;
}

quint64 le64(const QByteArray &head, int offset)
{
    return head.size() >= offset + 8 ? qFromLittleEndian<quint64>(head.constData() + offset) : 0;
}

// Text in a fixed-size field, up to the first zero, without the padding.
QString text(const QByteArray &head, int offset, int length, bool latin1 = false)
{
    if (head.size() < offset + length)
        return {};
    QByteArray field = head.mid(offset, length);
    const int zero = field.indexOf('\0');
    if (zero >= 0)
        field.truncate(zero);
    return (latin1 ? QString::fromLatin1(field) : QString::fromUtf8(field)).trimmed();
}

QString uuid(const QByteArray &head, int offset)
{
    if (head.size() < offset + 16)
        return {};
    const QByteArray hex = head.mid(offset, 16).toHex();
    if (hex == QByteArray(32, '0'))
        return {};
    return QString::fromLatin1(hex.left(8) + '-' + hex.mid(8, 4) + '-' + hex.mid(12, 4) + '-' + hex.mid(16, 4) + '-' + hex.mid(20));
}

QString fatSerial(quint32 serial)
{
    return QStringLiteral("%1-%2").arg(serial >> 16, 4, 16, QLatin1Char('0')).arg(serial & 0xffff, 4, 16, QLatin1Char('0')).toUpper();
}

Result make(const char *type, const char *usage, const QString &label = {}, const QString &id = {}, quint64 size = 0)
{
    return {QString::fromLatin1(type), QString::fromLatin1(usage), label, id, size};
}

quint32 be32(const QByteArray &head, int offset)
{
    return head.size() >= offset + 4 ? qFromBigEndian<quint32>(head.constData() + offset) : 0;
}

quint64 be64(const QByteArray &head, int offset)
{
    return head.size() >= offset + 8 ? qFromBigEndian<quint64>(head.constData() + offset) : 0;
}

// a * b, or 0 when it doesn't fit (a damaged header).
quint64 times(quint64 a, quint64 b)
{
    return b && a <= std::numeric_limits<quint64>::max() / b ? a * b : 0;
}

} // namespace

Result probe(const QByteArray &head)
{
    // Containers and members first: some of them leave an old file system's signature
    // behind them, and the outer thing is what counts.
    if (has(head, 0, "LUKS\xba\xbe", 6)) {
        // LUKS2 has a label; LUKS1 doesn't. The version is big-endian, like the rest of LUKS.
        const QString label = qFromBigEndian<quint16>(head.constData() + 6) == 2 ? text(head, 24, 48) : QString();
        return make("crypto_LUKS", "crypto", label, text(head, 168, 40, true));
    }
    if (has(head, 3, "-FVE-FS-", 8))
        return make("BitLocker", "crypto");
    if (has(head, 512 + 24, "LVM2 001", 8) && has(head, 512, "LABELONE", 8)) {
        // The physical volume header, where the label says: its ID (32 bytes), then the size.
        const quint32 at = le32(head, 512 + 20);
        return make("LVM2_member", "raid", {}, {}, at < 4096 ? le64(head, 512 + int(at) + 32) : 0);
    }
    // Linux RAID, version 1.2 metadata 4 KiB in (the usual one), 1.1 at the start.
    for (const int at : {4096, 0}) {
        if (le32(head, at) == 0xa92b4efc && le32(head, at + 4) == 1)
            return make("linux_raid_member", "raid", text(head, at + 32, 32), uuid(head, at + 16));
    }

    // bcachefs: its superblock 4 KiB in, with bcachefs' own magic, or bcache's from before it had
    // one (told from bcache, the block cache, by its version: bcache's never went past 4).
    if (has(head, 4096 + 24, "\xc6\x85\x73\xf6\x66\xce\x90\xa9\xd9\x6a\x60\xcf\x80\x3d\xf7\xef", 16)
        || (has(head, 4096 + 24, "\xc6\x85\x73\xf6\x4e\x1a\x45\xca\x82\x65\xf5\x7f\x48\xba\x6d\x81", 16) && le16(head, 4096 + 16) > 4))
        return make("bcachefs", "filesystem", text(head, 4096 + 72, 32), uuid(head, 4096 + 56));
    if (has(head, 65536 + 64, "_BHRfS_M", 8))
        return make("btrfs", "filesystem", text(head, 65536 + 0x12b, 256), uuid(head, 65536 + 32), le64(head, 65536 + 0x70));
    if (le16(head, 1024 + 0x38) == 0xef53) {
        const quint32 compat = le32(head, 1024 + 0x5c);
        const quint32 incompat = le32(head, 1024 + 0x60);
        // Extents, 64-bit or flex_bg: ext4. A journal without those: ext3.
        const char *type = (incompat & (0x40 | 0x80 | 0x200)) ? "ext4" : (compat & 0x4) ? "ext3" : "ext2";
        // Blocks of 1 KiB shifted by the log, the count's high half only with 64-bit.
        const quint32 log = le32(head, 1024 + 24);
        const quint64 blocks = le32(head, 1024 + 4) | ((incompat & 0x80) ? quint64(le32(head, 1024 + 0x150)) << 32 : 0);
        return make(type, "filesystem", text(head, 1024 + 0x78, 16), uuid(head, 1024 + 0x68), log < 7 ? times(blocks, 1024u << log) : 0);
    }
    if (has(head, 0, "XFSB", 4))
        return make("xfs", "filesystem", text(head, 108, 12), uuid(head, 32), times(be64(head, 8), be32(head, 4)));
    // Swap's signature sits at the end of the first page: 4 KiB on PCs, 64 KiB on some ARM.
    for (const int page : {4096, 8192, 16384, 65536}) {
        if (has(head, page - 10, "SWAPSPACE2", 10) || has(head, page - 10, "SWAP-SPACE", 10))
            return make("swap", "other", text(head, 1024 + 28, 16), uuid(head, 1024 + 12), times(quint64(le32(head, 1024 + 4)) + 1, quint64(page)));
    }

    // NTFS counts its sectors without the last one, where the boot sector's copy is.
    if (has(head, 3, "NTFS    ", 8))
        return make("ntfs", "filesystem", {}, QStringLiteral("%1").arg(le64(head, 0x48), 16, 16, QLatin1Char('0')).toUpper(),
                    times(le64(head, 0x28) + 1, le16(head, 11)));
    if (has(head, 3, "EXFAT   ", 8)) {
        const int shift = head.size() > 108 ? uchar(head[108]) : 0;
        return make("exfat", "filesystem", {}, fatSerial(le32(head, 0x64)), shift >= 9 && shift <= 12 ? times(le64(head, 72), 1u << shift) : 0);
    }
    if (has(head, 3, "ReFS", 4))
        return make("refs", "filesystem");
    if (le16(head, 510) == 0xaa55) {
        // FAT32 says so at 0x52, FAT12/16 at 0x36; the label in the boot sector is only a copy,
        // but a good enough one here.
        // Its sectors: the 16-bit count, or the 32-bit one when that's 0.
        const quint64 fatSize = times(le16(head, 19) ? le16(head, 19) : le32(head, 32), le16(head, 11));
        if (has(head, 0x52, "FAT32   ", 8)) {
            const QString label = text(head, 0x47, 11, true);
            return make("vfat", "filesystem", label == QLatin1String("NO NAME") ? QString() : label, fatSerial(le32(head, 0x43)), fatSize);
        }
        if (has(head, 0x36, "FAT1", 4) || has(head, 0x36, "FAT     ", 8)) {
            const QString label = text(head, 0x2b, 11, true);
            return make("vfat", "filesystem", label == QLatin1String("NO NAME") ? QString() : label, fatSerial(le32(head, 0x27)), fatSize);
        }
    }
    if (has(head, 32768 + 1, "CD001", 5))
        return make("iso9660", "filesystem", text(head, 32768 + 40, 32, true));
    if (has(head, 1024, "H+", 2) || has(head, 1024, "HX", 2))
        return make("hfsplus", "filesystem", {}, {}, times(be32(head, 1024 + 44), be32(head, 1024 + 40)));
    if (has(head, 32, "NXSB", 4))
        return make("apfs", "filesystem");
    return {};
}

} // namespace fsprobe
