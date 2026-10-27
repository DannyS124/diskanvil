// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gpt.h"

#include "blockio.h"
#include "format.h"

#include <QObject>
#include <QRandomGenerator>
#include <QRegularExpression>

#include <algorithm>
#include <cstring>
#include <unistd.h>

namespace gpt {

namespace {

// All GPT fields are little-endian.
quint32 le32(const char *p)
{
    quint32 v;
    std::memcpy(&v, p, 4);
    return v;
}
quint64 le64(const char *p)
{
    quint64 v;
    std::memcpy(&v, p, 8);
    return v;
}
void put32(char *p, quint32 v) { std::memcpy(p, &v, 4); }
void put64(char *p, quint64 v) { std::memcpy(p, &v, 8); }

quint32 crc32(const char *data, size_t len)
{
    static quint32 table[256] = {};
    static bool ready = false;
    if (!ready) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    quint32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        c = table[(c ^ quint8(data[i])) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// Header field offsets (UEFI spec, table 5-5).
constexpr int kHeaderSize = 12, kHeaderCrc = 16, kMyLba = 24, kAlternateLba = 32, kLastUsable = 48, kDiskGuid = 56, kEntriesLba = 72, kEntryCount = 80, kEntrySize = 84, kEntriesCrc = 88;

quint32 headerCrc(char *header)
{
    const quint32 size = le32(header + kHeaderSize);
    char copy[512];
    std::memcpy(copy, header, size);
    put32(copy + kHeaderCrc, 0);
    return crc32(copy, size);
}

void randomGuid(char *p)
{
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(p), 4);
    p[7] = char((quint8(p[7]) & 0x0F) | 0x40); // version 4 (mixed-endian field)
    p[8] = char((quint8(p[8]) & 0x3F) | 0x80); // variant 1
}

} // namespace

bool isGpt(int fd, int sectorSize)
{
    const int sector = sectorSize > 0 ? sectorSize : blockio::logicalSize(fd);
    blockio::Buffer buf = blockio::alignedBuffer(size_t(std::max(sector, int(blockio::kAlign))));
    return buf && blockio::readAt(fd, buf.get(), quint64(sector), quint64(sector))
        && std::memcmp(buf.get(), "EFI PART", 8) == 0;
}

Result relocateBackup(int fd, int sectorSize, bool newGuids)
{
    Result r;
    const quint64 sector = quint64(sectorSize > 0 ? sectorSize : blockio::logicalSize(fd));
    const quint64 lastLba = blockio::deviceSize(fd) / sector - 1;

    blockio::Buffer header = blockio::alignedBuffer(std::max<size_t>(sector, blockio::kAlign));
    if (!header || !blockio::readAt(fd, header.get(), sector, sector)) {
        r.error = QObject::tr("Couldn't read the partition table");
        return r;
    }
    char *h = header.get();
    if (std::memcmp(h, "EFI PART", 8) != 0) {
        r.error = QObject::tr("Not a GPT disk");
        return r;
    }
    const quint32 hsize = le32(h + kHeaderSize);
    if (hsize < 92 || hsize > 512 || hsize > sector || headerCrc(h) != le32(h + kHeaderCrc)) {
        r.error = QObject::tr("The partition table header is damaged, so it was left alone");
        return r;
    }

    const quint64 entriesLba = le64(h + kEntriesLba);
    const quint32 count = le32(h + kEntryCount);
    const quint32 entrySize = le32(h + kEntrySize);
    if (entrySize < 128 || count == 0 || quint64(count) * entrySize > 16 * 1024 * 1024) {
        r.error = QObject::tr("The partition table is damaged, so it was left alone");
        return r;
    }
    const quint64 entriesBytes = quint64(count) * entrySize;
    const quint64 entriesSectors = (entriesBytes + sector - 1) / sector;
    if (lastLba < entriesSectors + 34 || lastLba >= quint64(1) << 48) {
        r.error = QObject::tr("The disk is too small for its partitions");
        return r;
    }
    blockio::Buffer entries = blockio::alignedBuffer(size_t(entriesSectors * sector));
    if (!entries || !blockio::readAt(fd, entries.get(), entriesSectors * sector, entriesLba * sector)
        || crc32(entries.get(), entriesBytes) != le32(h + kEntriesCrc)) {
        r.error = QObject::tr("The partition list is damaged, so it was left alone");
        return r;
    }

    // Every partition has to end before the new backup area.
    const quint64 oldBackupLba = le64(h + kAlternateLba);
    const quint64 newLastUsable = lastLba - entriesSectors - 1;
    quint64 lastEnd = 0;
    for (quint32 i = 0; i < count; ++i) {
        char *e = entries.get() + quint64(i) * entrySize;
        static const char zero[16] = {};
        if (std::memcmp(e, zero, 16) == 0)
            continue; // unused slot
        lastEnd = std::max(lastEnd, le64(e + 40));
        if (le64(e + 40) > newLastUsable) {
            r.error = QObject::tr("The disk is too small for its partitions");
            return r;
        }
        if (newGuids)
            randomGuid(e + 16);
    }
    if (newGuids)
        randomGuid(h + kDiskGuid);

    const quint32 entriesCrc = crc32(entries.get(), entriesBytes);
    put64(h + kAlternateLba, lastLba);
    put64(h + kLastUsable, newLastUsable);
    put32(h + kEntriesCrc, entriesCrc);
    put32(h + kHeaderCrc, headerCrc(h));

    blockio::Buffer backup = blockio::alignedBuffer(std::max<size_t>(sector, blockio::kAlign));
    std::memcpy(backup.get(), h, sector);
    char *b = backup.get();
    put64(b + kMyLba, lastLba);
    put64(b + kAlternateLba, 1);
    put64(b + kEntriesLba, lastLba - entriesSectors);
    put32(b + kHeaderCrc, headerCrc(b));

    // Backup first: if anything stops halfway, the primary still describes a valid disk.
    if (!blockio::writeAt(fd, entries.get(), entriesSectors * sector, (lastLba - entriesSectors) * sector)
        || !blockio::writeAt(fd, b, sector, lastLba * sector) || !blockio::flush(fd)) {
        r.error = QObject::tr("Couldn't write the backup partition table");
        return r;
    }
    if ((newGuids && !blockio::writeAt(fd, entries.get(), entriesSectors * sector, entriesLba * sector))
        || !blockio::writeAt(fd, h, sector, sector)) {
        r.error = QObject::tr("Couldn't write the partition table");
        return r;
    }

    // The old backup header now sits in free space in the middle of the disk; clear it
    // so nothing mistakes it for the real one later.
    if (oldBackupLba != lastLba && oldBackupLba > lastEnd && oldBackupLba < lastLba - entriesSectors) {
        blockio::Buffer zero = blockio::alignedBuffer(std::max<size_t>(sector, blockio::kAlign));
        std::memset(zero.get(), 0, sector);
        blockio::writeAt(fd, zero.get(), sector, oldBackupLba * sector);
    }

    // The protective MBR's single partition covers the whole disk (capped at 32 bits).
    blockio::Buffer mbr = blockio::alignedBuffer(std::max<size_t>(sector, blockio::kAlign));
    if (blockio::readAt(fd, mbr.get(), sector, 0) && quint8(mbr.get()[450]) == 0xEE) {
        put32(mbr.get() + 458, quint32(std::min<quint64>(lastLba, 0xFFFFFFFFu)));
        blockio::writeAt(fd, mbr.get(), sector, 0);
    }
    if (!blockio::flush(fd)) {
        r.error = QObject::tr("Couldn't write the partition table");
        return r;
    }
    r.ok = true;
    return r;
}

QString guidText(const char *p)
{
    const auto b = [p](int i) { return quint8(p[i]); };
    return QStringLiteral("%1-%2-%3-%4%5-%6%7%8%9%10%11")
        .arg(le32(p), 8, 16, QLatin1Char('0'))
        .arg(quint16(quint8(p[4]) | quint8(p[5]) << 8), 4, 16, QLatin1Char('0'))
        .arg(quint16(quint8(p[6]) | quint8(p[7]) << 8), 4, 16, QLatin1Char('0'))
        .arg(b(8), 2, 16, QLatin1Char('0'))
        .arg(b(9), 2, 16, QLatin1Char('0'))
        .arg(b(10), 2, 16, QLatin1Char('0'))
        .arg(b(11), 2, 16, QLatin1Char('0'))
        .arg(b(12), 2, 16, QLatin1Char('0'))
        .arg(b(13), 2, 16, QLatin1Char('0'))
        .arg(b(14), 2, 16, QLatin1Char('0'))
        .arg(b(15), 2, 16, QLatin1Char('0'));
}

bool Mbr::hybrid() const
{
    bool gpt = false, other = false;
    for (const MbrEntry &e : entries) {
        gpt = gpt || e.type == 0xEE;
        other = other || e.type != 0xEE;
    }
    return gpt && other;
}

Mbr parseMbr(const char *sector0)
{
    Mbr m;
    m.signature = quint8(sector0[510]) == 0x55 && quint8(sector0[511]) == 0xAA;
    for (int i = 0; i < 4; ++i) {
        const char *e = sector0 + 446 + i * 16;
        MbrEntry entry;
        entry.index = i + 1;
        entry.bootable = quint8(e[0]) == 0x80;
        entry.type = quint8(e[4]);
        entry.firstLba = le32(e + 8);
        entry.sectors = le32(e + 12);
        if (entry.type != 0)
            m.entries.push_back(entry);
    }
    return m;
}

Header parseHeader(const char *sector, int sectorSize, quint64 lba)
{
    Header h;
    h.lba = lba;
    h.present = std::memcmp(sector, "EFI PART", 8) == 0;
    if (!h.present)
        return h;
    h.revision = le32(sector + 8);
    h.headerSize = le32(sector + kHeaderSize);
    h.sizeOk = h.headerSize >= 92 && h.headerSize <= 512 && int(h.headerSize) <= sectorSize;
    if (h.sizeOk) {
        char copy[512];
        std::memcpy(copy, sector, h.headerSize);
        h.headerCrcOk = headerCrc(copy) == le32(sector + kHeaderCrc);
    }
    h.myLba = le64(sector + kMyLba);
    h.alternateLba = le64(sector + kAlternateLba);
    h.firstUsable = le64(sector + 40);
    h.lastUsable = le64(sector + kLastUsable);
    h.diskGuid = guidText(sector + kDiskGuid);
    h.entriesLba = le64(sector + kEntriesLba);
    h.entryCount = le32(sector + kEntryCount);
    h.entrySize = le32(sector + kEntrySize);
    h.entriesCrc = le32(sector + kEntriesCrc);
    return h;
}

void parseEntries(Header &h, const char *list, quint64 listBytes)
{
    const quint64 bytes = quint64(h.entryCount) * h.entrySize;
    if (h.entrySize < 128 || h.entryCount == 0 || bytes > listBytes || bytes > 16 * 1024 * 1024)
        return;
    h.entriesRead = true;
    h.entriesCrcOk = crc32(list, bytes) == h.entriesCrc;
    static const char zero[16] = {};
    for (quint32 i = 0; i < h.entryCount; ++i) {
        const char *e = list + quint64(i) * h.entrySize;
        if (std::memcmp(e, zero, 16) == 0)
            continue;
        Entry entry;
        entry.index = int(i) + 1;
        entry.type = guidText(e);
        entry.guid = guidText(e + 16);
        entry.firstLba = le64(e + 32);
        entry.lastLba = le64(e + 40);
        entry.attributes = le64(e + 48);
        char16_t name[36];
        std::memcpy(name, e + 56, sizeof(name));
        int length = 0;
        while (length < 36 && name[length] != 0)
            ++length;
        entry.name = cleanName(QString::fromUtf16(name, length));
        h.entries.push_back(entry);
    }
}

namespace {

// Reads a header at `lba` and, when it's sound, its partition list.
Header readHeader(int fd, quint64 sector, quint64 lastLba, quint64 lba)
{
    blockio::Buffer buf = blockio::alignedBuffer(std::max<size_t>(sector, blockio::kAlign));
    if (!buf || lba > lastLba || !blockio::readAt(fd, buf.get(), sector, lba * sector))
        return {};
    Header h = parseHeader(buf.get(), int(sector), lba);
    if (!h.present || !h.sizeOk)
        return h;
    const quint64 bytes = quint64(h.entryCount) * h.entrySize;
    if (h.entrySize < 128 || h.entryCount == 0 || bytes > 16 * 1024 * 1024)
        return h;
    const quint64 sectors = (bytes + sector - 1) / sector;
    if (h.entriesLba == 0 || h.entriesLba > lastLba || sectors > lastLba - h.entriesLba + 1)
        return h;
    blockio::Buffer list = blockio::alignedBuffer(size_t(sectors * sector));
    if (list && blockio::readAt(fd, list.get(), sectors * sector, h.entriesLba * sector))
        parseEntries(h, list.get(), sectors * sector);
    return h;
}

} // namespace

Report inspect(int fd, int sectorSize)
{
    Report r;
    r.sectorSize = sectorSize > 0 ? sectorSize : blockio::logicalSize(fd);
    const quint64 sector = quint64(std::max(r.sectorSize, 512));
    const quint64 size = blockio::deviceSize(fd);
    if (size < sector * 2) {
        r.problems << QObject::tr("The drive is too small to hold a partition table.");
        return r;
    }
    r.lastLba = size / sector - 1;
    blockio::Buffer first = blockio::alignedBuffer(std::max<size_t>(sector, blockio::kAlign));
    if (!first || !blockio::readAt(fd, first.get(), sector, 0)) {
        r.problems << QObject::tr("Couldn't read the start of the drive.");
        return r;
    }
    r.mbr = parseMbr(first.get());
    r.primary = readHeader(fd, sector, r.lastLba, 1);
    r.backup = readHeader(fd, sector, r.lastLba, r.lastLba);
    if (r.primary.valid() && r.primary.alternateLba != r.lastLba) {
        r.backupElsewhere = true;
        const Header there = readHeader(fd, sector, r.lastLba, r.primary.alternateLba);
        if (!r.backup.present && there.present)
            r.backup = there;
    }
    r.backupMatches = r.primary.valid() && r.backup.valid() && r.primary.diskGuid == r.backup.diskGuid
        && r.primary.entriesCrc == r.backup.entriesCrc && r.primary.entryCount == r.backup.entryCount;

    const bool anyGpt = r.primary.present || r.backup.present;
    if (!anyGpt && !r.mbr.signature)
        r.problems << QObject::tr("There's no partition table: the drive is empty, or its table was wiped.");
    if (!anyGpt)
        return r; // a plain MBR disk
    if (!r.primary.present)
        r.problems << QObject::tr("The main GPT header is missing.");
    else if (!r.primary.sizeOk || !r.primary.headerCrcOk)
        r.problems << QObject::tr("The main GPT header is damaged (its checksum doesn't match).");
    else if (!r.primary.entriesRead || !r.primary.entriesCrcOk)
        r.problems << QObject::tr("The main partition list is damaged (its checksum doesn't match).");
    if (!r.backup.present)
        r.problems << QObject::tr("The backup GPT header at the end of the drive is missing.");
    else if (!r.backup.valid())
        r.problems << QObject::tr("The backup GPT header or its partition list is damaged.");
    else if (r.primary.valid() && !r.backupMatches)
        r.problems << QObject::tr("The backup doesn't match the main table.");
    if (r.backupElsewhere)
        r.problems << QObject::tr("The backup isn't at the end of the drive; it was probably copied from a smaller one.");
    if (!r.mbr.signature || r.mbr.entries.isEmpty())
        r.problems << QObject::tr("There's no protective MBR, so older tools may think the drive is empty.");
    else if (r.mbr.hybrid())
        r.problems << QObject::tr("It has a hybrid MBR (GPT and MBR partitions at once). Some tools get confused by that.");
    else if (!r.mbr.protective())
        r.problems << QObject::tr("The MBR describes other partitions than the GPT.");
    return r;
}

bool guidBytes(const QString &text, char *out)
{
    static const QRegularExpression form(QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    if (!form.match(text).hasMatch())
        return false;
    const QByteArray hex = QString(text).remove(QLatin1Char('-')).toLatin1();
    const QByteArray raw = QByteArray::fromHex(hex);
    // The first three groups are stored little-endian, the rest as written.
    const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int i = 0; i < 16; ++i)
        out[i] = raw[order[i]];
    return true;
}

namespace {

void protectiveMbr(char *sector0, quint64 lastLba)
{
    // Boot code (0-439) is kept; the disk signature and the four entries are new.
    std::memset(sector0 + 440, 0, 72);
    char *e = sector0 + 446;
    e[1] = 0x00;
    e[2] = 0x02;
    e[3] = 0x00; // CHS 0/0/2
    e[4] = char(0xEE);
    e[5] = char(0xFF);
    e[6] = char(0xFF);
    e[7] = char(0xFF);
    put32(e + 8, 1);
    put32(e + 12, quint32(std::min<quint64>(lastLba, 0xFFFFFFFFu)));
    sector0[510] = 0x55;
    sector0[511] = char(0xAA);
}

bool zeroGuid(const char *p)
{
    static const char zero[16] = {};
    return std::memcmp(p, zero, 16) == 0;
}

} // namespace

Result writeTable(int fd, int sectorSize, const QString &diskGuid, const QVector<Entry> &entries)
{
    Result r;
    const quint64 sector = quint64(sectorSize > 0 ? sectorSize : blockio::logicalSize(fd));
    const quint64 size = blockio::deviceSize(fd);
    constexpr quint32 count = 128, entrySize = 128;
    const quint64 listBytes = quint64(count) * entrySize;
    const quint64 listSectors = (listBytes + sector - 1) / sector;
    if (sector < 512 || size / sector < 2 * listSectors + 8) {
        r.error = QObject::tr("The drive is too small for a GPT");
        return r;
    }
    const quint64 lastLba = size / sector - 1;
    const quint64 firstUsable = 2 + listSectors, lastUsable = lastLba - listSectors - 1;
    if (entries.size() > int(count)) {
        r.error = QObject::tr("Too many partitions for a GPT");
        return r;
    }

    blockio::Buffer list = blockio::alignedBuffer(size_t(listSectors * sector));
    if (!list) {
        r.error = QObject::tr("Out of memory");
        return r;
    }
    std::memset(list.get(), 0, listSectors * sector);
    QVector<bool> used(count, false);
    QVector<QPair<quint64, quint64>> spans;
    for (const Entry &e : entries) {
        if (e.firstLba < firstUsable || e.lastLba > lastUsable || e.firstLba > e.lastLba) {
            r.error = QObject::tr("Partition %1 doesn't fit on the drive").arg(e.index);
            return r;
        }
        for (const auto &[first, last] : std::as_const(spans)) {
            if (e.firstLba <= last && first <= e.lastLba) {
                r.error = QObject::tr("Partitions overlap");
                return r;
            }
        }
        spans.push_back({e.firstLba, e.lastLba});
        int slot = e.index - 1;
        if (slot < 0)
            slot = int(used.indexOf(false));
        if (slot < 0 || slot >= int(count) || used[slot]) {
            r.error = QObject::tr("Partition number %1 is taken or too high").arg(e.index);
            return r;
        }
        used[slot] = true;
        char *p = list.get() + quint64(slot) * entrySize;
        if (!guidBytes(e.type, p) || zeroGuid(p)) {
            r.error = QObject::tr("Partition %1 has no valid type").arg(e.index);
            return r;
        }
        if (e.guid.isEmpty() || !guidBytes(e.guid, p + 16) || zeroGuid(p + 16))
            randomGuid(p + 16);
        put64(p + 32, e.firstLba);
        put64(p + 40, e.lastLba);
        put64(p + 48, e.attributes);
        const QString name = e.name.left(36);
        std::memcpy(p + 56, name.utf16(), size_t(name.size()) * 2);
    }

    blockio::Buffer header = blockio::alignedBuffer(std::max<size_t>(sector, blockio::kAlign));
    std::memset(header.get(), 0, sector);
    char *h = header.get();
    std::memcpy(h, "EFI PART", 8);
    put32(h + 8, 0x00010000);
    put32(h + kHeaderSize, 92);
    put64(h + kMyLba, 1);
    put64(h + kAlternateLba, lastLba);
    put64(h + 40, firstUsable);
    put64(h + kLastUsable, lastUsable);
    if (diskGuid.isEmpty() || !guidBytes(diskGuid, h + kDiskGuid) || zeroGuid(h + kDiskGuid))
        randomGuid(h + kDiskGuid);
    put64(h + kEntriesLba, 2);
    put32(h + kEntryCount, count);
    put32(h + kEntrySize, entrySize);
    put32(h + kEntriesCrc, crc32(list.get(), listBytes));
    put32(h + kHeaderCrc, headerCrc(h));

    blockio::Buffer backup = blockio::alignedBuffer(std::max<size_t>(sector, blockio::kAlign));
    std::memcpy(backup.get(), h, sector);
    char *b = backup.get();
    put64(b + kMyLba, lastLba);
    put64(b + kAlternateLba, 1);
    put64(b + kEntriesLba, lastLba - listSectors);
    put32(b + kHeaderCrc, 0);
    put32(b + kHeaderCrc, headerCrc(b));

    blockio::Buffer mbr = blockio::alignedBuffer(std::max<size_t>(sector, blockio::kAlign));
    if (!blockio::readAt(fd, mbr.get(), sector, 0))
        std::memset(mbr.get(), 0, sector);
    protectiveMbr(mbr.get(), lastLba);

    if (!blockio::writeAt(fd, list.get(), listSectors * sector, (lastLba - listSectors) * sector)
        || !blockio::writeAt(fd, b, sector, lastLba * sector) || !blockio::flush(fd)
        || !blockio::writeAt(fd, list.get(), listSectors * sector, 2 * sector) || !blockio::writeAt(fd, h, sector, sector)
        || !blockio::writeAt(fd, mbr.get(), sector, 0) || !blockio::flush(fd)) {
        r.error = QObject::tr("Couldn't write the partition table");
        return r;
    }
    r.ok = true;
    return r;
}

Result restoreFromBackup(int fd, int sectorSize)
{
    Result r;
    const Report report = inspect(fd, sectorSize);
    const quint64 sector = quint64(std::max(report.sectorSize, 512));
    const Header &b = report.backup;
    if (!b.valid() || b.lba != report.lastLba) {
        r.error = QObject::tr("The backup copy isn't intact, so it can't be used");
        return r;
    }
    const quint64 listBytes = quint64(b.entryCount) * b.entrySize;
    const quint64 listSectors = (listBytes + sector - 1) / sector;
    if (b.firstUsable < 2 + listSectors || b.myLba != report.lastLba) {
        r.error = QObject::tr("The backup copy doesn't describe this drive, so it was left alone");
        return r;
    }
    blockio::Buffer header = blockio::alignedBuffer(std::max<size_t>(sector, blockio::kAlign));
    blockio::Buffer list = blockio::alignedBuffer(size_t(listSectors * sector));
    if (!header || !list || !blockio::readAt(fd, header.get(), sector, report.lastLba * sector)
        || !blockio::readAt(fd, list.get(), listSectors * sector, b.entriesLba * sector)) {
        r.error = QObject::tr("Couldn't read the backup copy");
        return r;
    }
    char *h = header.get();
    put64(h + kMyLba, 1);
    put64(h + kAlternateLba, report.lastLba);
    put64(h + kEntriesLba, 2);
    put32(h + kHeaderCrc, headerCrc(h));

    blockio::Buffer mbr = blockio::alignedBuffer(std::max<size_t>(sector, blockio::kAlign));
    const bool fixMbr = !report.mbr.signature || !report.mbr.protective();
    if (fixMbr) {
        if (!blockio::readAt(fd, mbr.get(), sector, 0))
            std::memset(mbr.get(), 0, sector);
        protectiveMbr(mbr.get(), report.lastLba);
    }
    if (!blockio::writeAt(fd, list.get(), listSectors * sector, 2 * sector) || !blockio::writeAt(fd, h, sector, sector)
        || (fixMbr && !blockio::writeAt(fd, mbr.get(), sector, 0)) || !blockio::flush(fd)) {
        r.error = QObject::tr("Couldn't write the partition table");
        return r;
    }
    r.ok = true;
    return r;
}

Result writeMbr(int fd, int sectorSize, quint32 diskSignature, const QVector<MbrPart> &parts)
{
    Result r;
    const quint64 sector = quint64(sectorSize > 0 ? sectorSize : blockio::logicalSize(fd));
    const quint64 total = blockio::deviceSize(fd) / sector;
    auto fail = [&r](const QString &why) {
        r.error = why;
        return r;
    };
    QVector<MbrPart> primary, logical;
    for (const MbrPart &p : parts)
        (p.logical ? logical : primary).push_back(p);
    const auto extended = [](const MbrPart &p) { return p.type == 0x05 || p.type == 0x0f || p.type == 0x85; };
    const auto ext = std::find_if(primary.cbegin(), primary.cend(), extended);
    if (primary.size() > 4 || std::count_if(primary.cbegin(), primary.cend(), extended) > 1)
        return fail(QObject::tr("An MBR holds four primary partitions, one of them extended"));
    if (!logical.isEmpty() && ext == primary.cend())
        return fail(QObject::tr("Logical partitions need an extended partition"));
    QVector<QPair<quint64, quint64>> spans;
    for (const MbrPart &p : primary) {
        if (p.type == 0 || p.firstLba == 0 || p.sectors == 0 || p.firstLba + p.sectors > total || p.firstLba > 0xFFFFFFFFu
            || p.sectors > 0xFFFFFFFFu)
            return fail(QObject::tr("A partition doesn't fit in an MBR on this drive"));
        for (const auto &[first, last] : std::as_const(spans)) {
            if (p.firstLba <= last && first <= p.firstLba + p.sectors - 1)
                return fail(QObject::tr("Partitions overlap"));
        }
        spans.push_back({p.firstLba, p.firstLba + p.sectors - 1});
    }
    // Every check before anything is written.
    bool taken[4] = {};
    for (const MbrPart &p : std::as_const(primary)) {
        if (p.slot >= 1 && p.slot <= 4 && !taken[p.slot - 1])
            taken[p.slot - 1] = true;
        else if (p.slot != 0)
            return fail(QObject::tr("Partition number %1 is taken or too high").arg(p.slot));
    }
    // Logical partitions in order; each needs its EBR in the gap before it.
    std::sort(logical.begin(), logical.end(), [](const MbrPart &a, const MbrPart &b) { return a.firstLba < b.firstLba; });
    QVector<quint64> ebrs;
    if (!logical.isEmpty()) {
        const quint64 extStart = ext->firstLba, extEnd = ext->firstLba + ext->sectors - 1;
        quint64 next = extStart;
        for (const MbrPart &l : std::as_const(logical)) {
            if (l.type == 0 || extended(l) || l.sectors == 0 || l.firstLba <= next || l.firstLba + l.sectors - 1 > extEnd)
                return fail(QObject::tr("A logical partition doesn't fit in the extended one"));
            ebrs.push_back(next);
            next = l.firstLba + l.sectors; // the next EBR goes right after this partition
        }
    }

    blockio::Buffer buf = blockio::alignedBuffer(std::max<size_t>(sector, blockio::kAlign));
    auto entry = [](char *e, const MbrPart &p, quint64 start, quint64 sectors) {
        e[0] = p.bootable ? char(0x80) : 0;
        e[1] = char(0xFE); // CHS: "use the LBA values"
        e[2] = char(0xFF);
        e[3] = char(0xFF);
        e[4] = char(p.type);
        e[5] = char(0xFE);
        e[6] = char(0xFF);
        e[7] = char(0xFF);
        put32(e + 8, quint32(start));
        put32(e + 12, quint32(sectors));
    };
    // EBRs first (from the last), the MBR itself last.
    for (int i = int(logical.size()) - 1; i >= 0; --i) {
        std::memset(buf.get(), 0, sector);
        const MbrPart &l = logical[i];
        entry(buf.get() + 446, l, l.firstLba - ebrs[i], l.sectors);
        if (i + 1 < logical.size()) {
            MbrPart link;
            link.type = 0x05;
            const quint64 nextEnd = logical[i + 1].firstLba + logical[i + 1].sectors;
            entry(buf.get() + 462, link, ebrs[i + 1] - ext->firstLba, nextEnd - ebrs[i + 1]);
        }
        buf.get()[510] = 0x55;
        buf.get()[511] = char(0xAA);
        if (!blockio::writeAt(fd, buf.get(), sector, ebrs[i] * sector))
            return fail(QObject::tr("Couldn't write the partition table"));
    }
    if (!blockio::readAt(fd, buf.get(), sector, 0))
        std::memset(buf.get(), 0, sector);
    std::memset(buf.get() + 440, 0, 72);
    put32(buf.get() + 440, diskSignature);
    for (const MbrPart &p : std::as_const(primary)) {
        int slot = p.slot - 1;
        if (slot < 0) {
            slot = int(std::find(std::begin(taken), std::end(taken), false) - std::begin(taken));
            taken[slot] = true;
        }
        entry(buf.get() + 446 + slot * 16, p, p.firstLba, p.sectors);
    }
    buf.get()[510] = 0x55;
    buf.get()[511] = char(0xAA);
    if (!blockio::writeAt(fd, buf.get(), sector, 0))
        return fail(QObject::tr("Couldn't write the partition table"));
    // A GPT that was on the drive goes: each header (sector 1, and the backup in the last one) and
    // the partition list it points to. Left there, even the list alone, Windows reads the drive as
    // GPT too and keeps its MBR partitions offline and hidden. Only a real GPT header's sectors,
    // and never one that's in a partition or an EBR.
    auto used = [&](quint64 first, quint64 count) {
        for (const MbrPart &p : parts) {
            if (first < p.firstLba + p.sectors && p.firstLba < first + count)
                return true;
        }
        for (const quint64 e : std::as_const(ebrs)) {
            if (e >= first && e < first + count)
                return true;
        }
        return first == 0;
    };
    for (const quint64 lba : {quint64(1), total - 1}) {
        if (used(lba, 1) || !blockio::readAt(fd, buf.get(), sector, lba * sector))
            continue;
        const Header old = parseHeader(buf.get(), int(sector), lba);
        if (!old.present)
            continue;
        const quint64 listLba = old.entriesLba;
        const quint64 listBytes = quint64(old.entryCount) * old.entrySize;
        const quint64 listSectors = (listBytes + sector - 1) / sector;
        QVector<QPair<quint64, quint64>> clear{{lba, 1}};
        if (listBytes > 0 && listBytes <= 1024 * 1024 && listLba > 0 && listLba + listSectors <= total && !used(listLba, listSectors))
            clear.push_back({listLba, listSectors});
        std::memset(buf.get(), 0, sector);
        for (const auto &[first, count] : std::as_const(clear)) {
            for (quint64 s = first; s < first + count; ++s) {
                if (!blockio::writeAt(fd, buf.get(), sector, s * sector))
                    return fail(QObject::tr("Couldn't clear the old GPT"));
            }
        }
    }
    if (!blockio::flush(fd))
        return fail(QObject::tr("Couldn't write the partition table"));
    r.ok = true;
    return r;
}

QString hexDump(const QByteArray &data, quint64 firstOffset)
{
    QString out;
    for (qsizetype line = 0; line < data.size(); line += 16) {
        QString hex, text;
        for (int i = 0; i < 16; ++i) {
            if (line + i < data.size()) {
                const quint8 c = quint8(data[line + i]);
                hex += QStringLiteral("%1 ").arg(c, 2, 16, QLatin1Char('0'));
                text += c >= 0x20 && c < 0x7f ? QChar(c) : QLatin1Char('.');
            } else {
                hex += QStringLiteral("   ");
            }
            if (i == 7)
                hex += QLatin1Char(' ');
        }
        out += QStringLiteral("%1  %2 |%3|\n").arg(firstOffset + quint64(line), 8, 16, QLatin1Char('0')).arg(hex, text);
    }
    return out;
}

} // namespace gpt
