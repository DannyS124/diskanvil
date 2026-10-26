// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Find Lost Files' own reader for ext4 (and ext2 and ext3, which it grew from). It lists what the
// file system knows: every file with its name, folder and pieces; the names of deleted ones; and
// the files of an ext4 that was made again over this one (a format), from the old file records
// it leaves in the parts of the inode tables it hasn't used yet. Like the other readers, every
// number read is checked before it's used, every walk has a limit, and nothing is written.
//
// What deleting leaves behind on ext4: the folder entry is joined onto the one before it (its
// name stays in the gap until the folder is written again), and the file's record (inode) keeps
// its times but loses its size and where its pieces were. So a deleted file is a name and a date;
// its contents come back through the deep scan, or from older copies of its record in the journal.

#include "lostfs.h"

#include "applog.h"
#include "filetypes.h"
#include "lostraw.h"

#include <QCoreApplication>
#include <QHash>
#include <QSet>

#include <algorithm>

namespace lost {

namespace {

using raw::bytes;
using raw::le16;
using raw::le32;

QString tr(const char *text)
{
    return QCoreApplication::translate("lost::Ext4", text);
}

constexpr quint64 kMaxFolderBytes = 64 * 1024 * 1024;
constexpr int kMaxExtents = 200000;
constexpr int kMaxListed = 2000000;
constexpr int kMaxDepth = 64;
constexpr int kBatch = 500;
constexpr quint64 kMaxJournal = 512ull * 1024 * 1024;

constexpr quint32 kIncompatFiletype = 0x2;
constexpr quint32 kIncompatMetaBg = 0x10;
constexpr quint32 kIncompat64Bit = 0x80;
constexpr quint32 kFlagExtents = 0x80000;
constexpr quint32 kFlagInline = 0x10000000;
constexpr quint32 kFlagIndex = 0x1000; // a folder with a hashed index in its first block
constexpr quint16 kGroupInodesUnused = 0x1;
constexpr quint16 kGroupTableZeroed = 0x4;

struct DirEntry {
    quint32 inode = 0;
    QString name;
    bool isDir = false;
    bool deleted = false;
};

class Ext4
{
public:
    Ext4(Source &source, const Extent &where)
        : m_source(source)
        , m_where(where)
    {
    }

    quint64 blockSize = 0;
    quint64 blocks = 0;
    quint32 inodes = 0, inodesPerGroup = 0, inodeSize = 0, groups = 0;
    quint32 journalInode = 0; // the journal's own record, when it has one inside
    QString label;

    bool open()
    {
        const QByteArray sb = raw::read(m_source, m_where.start + 1024, 1024);
        if (sb.size() < 1024)
            return false;
        const uchar *s = bytes(sb);
        if (le16(s + 0x38) != 0xEF53)
            return false;
        const quint32 logBlock = le32(s + 0x18);
        if (logBlock > 6)
            return false;
        blockSize = 1024ull << logBlock;
        inodes = le32(s);
        m_incompat = le32(s + 0x60);
        blocks = le32(s + 0x04) | ((m_incompat & kIncompat64Bit) ? quint64(le32(s + 0x150)) << 32 : 0);
        m_firstDataBlock = le32(s + 0x14);
        const quint32 blocksPerGroup = le32(s + 0x20);
        inodesPerGroup = le32(s + 0x28);
        inodeSize = le32(s + 0x4C) >= 1 ? le16(s + 0x58) : 128;
        m_descSize = (m_incompat & kIncompat64Bit) ? std::max<quint32>(32, le16(s + 0xFE)) : 32;
        if (blocks == 0 || blocksPerGroup == 0 || inodesPerGroup == 0 || inodeSize < 128 || inodeSize > blockSize || (inodeSize & (inodeSize - 1))
            || m_descSize > 1024 || m_firstDataBlock >= blocks || blocks * blockSize > m_where.length + blockSize || (m_incompat & kIncompatMetaBg))
            return false;
        groups = quint32((blocks - m_firstDataBlock + blocksPerGroup - 1) / blocksPerGroup);
        if (groups == 0 || quint64(groups) * inodesPerGroup < inodes || groups > 1u << 24)
            return false;
        if ((le32(s + 0x5C) & 0x4) && le32(s + 0xE0) > 0 && le32(s + 0xE0) <= inodes) // has_journal
            journalInode = le32(s + 0xE0);
        label = QString::fromUtf8(reinterpret_cast<const char *>(s + 0x78), 16);
        label.truncate(label.indexOf(QChar(0)) < 0 ? 16 : label.indexOf(QChar(0)));
        m_descriptors = raw::read(m_source, m_where.start + (m_firstDataBlock + 1) * blockSize, quint64(groups) * m_descSize);
        return quint64(m_descriptors.size()) == quint64(groups) * m_descSize;
    }

    bool hasFileTypes() const { return m_incompat & kIncompatFiletype; }

    // A group's inode table, inode bitmap and flags.
    quint64 inodeTable(quint32 group) const { return descriptor(group, 0x08, 0x28); }
    quint64 inodeBitmap(quint32 group) const { return descriptor(group, 0x04, 0x24); }
    quint16 groupFlags(quint32 group) const { return le16(bytes(m_descriptors) + quint64(group) * m_descSize + 0x12); }
    // How many records at the end of a group's table it hasn't used since it was made.
    quint32 neverUsed(quint32 group) const
    {
        const uchar *d = bytes(m_descriptors) + quint64(group) * m_descSize;
        return std::min<quint32>(inodesPerGroup, le16(d + 0x1C) | (m_descSize >= 64 ? quint32(le16(d + 0x32)) << 16 : 0));
    }

    QByteArray inode(quint32 n)
    {
        if (n == 0 || n > quint64(groups) * inodesPerGroup)
            return {};
        const quint32 group = (n - 1) / inodesPerGroup, index = (n - 1) % inodesPerGroup;
        const quint64 table = inodeTable(group);
        if (table == 0 || table >= blocks)
            return {};
        return raw::read(m_source, m_where.start + table * blockSize + quint64(index) * inodeSize, inodeSize);
    }

    bool inUse(quint32 n)
    {
        if (n == 0)
            return false;
        const quint32 group = (n - 1) / inodesPerGroup, index = (n - 1) % inodesPerGroup;
        if (group >= groups || (groupFlags(group) & kGroupInodesUnused))
            return false;
        auto it = m_bitmaps.find(group);
        if (it == m_bitmaps.end()) {
            if (m_bitmaps.size() > 4096)
                m_bitmaps.clear();
            const quint64 at = inodeBitmap(group);
            it = m_bitmaps.insert(group, at && at < blocks ? raw::read(m_source, m_where.start + at * blockSize, blockSize) : QByteArray());
        }
        return index / 8 < quint64(it->size()) && (bytes(*it)[index / 8] & (1u << (index % 8)));
    }

    static quint64 sizeOf(const uchar *i) { return le32(i + 0x04) | quint64(le32(i + 0x6C)) << 32; }
    static quint16 modeOf(const uchar *i) { return le16(i); }
    static bool isFile(const uchar *i) { return (modeOf(i) & 0xF000) == 0x8000; }
    static bool isFolder(const uchar *i) { return (modeOf(i) & 0xF000) == 0x4000; }
    static qint64 when(const uchar *i, int at) { return le32(i + at) ? qint64(le32(i + at)) * 1000 : 0; }
    qint64 created(const uchar *i) const
    {
        // In the bigger records only (crtime at 0x90, when the extra part reaches it).
        return inodeSize >= 0x98 && 0x80 + le16(i + 0x80) >= 0x98 ? when(i, 0x90) : 0;
    }

    // Where a file's bytes are, in order: from its extent tree (ext4) or its block map (ext2 and
    // ext3). Gaps (sparse parts, and space set aside but never written) read as zeros. False
    // when the record doesn't add up.
    bool extentsOf(const uchar *i, quint64 size, QVector<Extent> *out)
    {
        out->clear();
        QVector<std::pair<quint64, Extent>> runs; // logical block, where
        bool ok = (le32(i + 0x20) & kFlagExtents) ? extentTree(i + 0x28, 60, 0, &runs) : blockMap(i + 0x28, &runs);
        if (!ok)
            return false;
        std::sort(runs.begin(), runs.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        quint64 logical = 0, left = size;
        for (const auto &[block, e] : std::as_const(runs)) {
            if (left == 0)
                break;
            const quint64 at = block * blockSize;
            if (at < logical)
                return false; // overlapping pieces: not to be trusted
            if (at > logical) {
                const quint64 gap = std::min(at - logical, left);
                append(out, {Extent::kZeros, gap});
                left -= gap;
                logical += gap;
            }
            const quint64 n = std::min(e.length, left);
            append(out, {e.start, n});
            left -= n;
            logical += e.length;
        }
        if (left > 0)
            append(out, {Extent::kZeros, left});
        return true;
    }

    // A small file kept inside its own record (inline data): its bytes, when it's one of those.
    static bool inlineData(const uchar *i, quint64 size, QByteArray *data)
    {
        if (!(le32(i + 0x20) & kFlagInline) || size > 60)
            return false;
        *data = QByteArray(reinterpret_cast<const char *>(i + 0x28), qsizetype(size));
        return true;
    }

    QByteArray readExtents(const QVector<Extent> &extents, quint64 most)
    {
        QByteArray out;
        for (const Extent &e : extents) {
            if (quint64(out.size()) >= most)
                break;
            const quint64 n = std::min(e.length, most - quint64(out.size()));
            out += e.start == Extent::kZeros ? QByteArray(qsizetype(n), '\0') : raw::read(m_source, e.start, n);
        }
        return out;
    }

    // A folder's entries. With `deleted`, also the ones in the gaps between them: deleting joins
    // an entry onto the one before, and its name stays there until the folder is written again.
    QVector<DirEntry> parseFolder(const QByteArray &data, bool indexed, bool deleted) const
    {
        QVector<DirEntry> out;
        for (qsizetype block = 0; block + qsizetype(blockSize) <= data.size(); block += qsizetype(blockSize)) {
            const uchar *b = bytes(data) + block;
            for (quint32 pos = 0; pos + 8 <= blockSize;) {
                const quint16 length = le16(b + pos + 4);
                if (length < 8 || length % 4 || pos + length > blockSize)
                    break;
                DirEntry e;
                const quint32 used = entryAt(b + pos, length, &e);
                if (used && e.inode)
                    out << e;
                // The first block of an indexed folder keeps its index in ".."'s gap: not names.
                const bool index = indexed && block == 0 && e.name == QLatin1String("..");
                if (deleted && used && !index) {
                    for (quint32 gap = pos + used; gap + 12 <= pos + length; gap += 4) {
                        DirEntry old;
                        const quint32 got = entryAt(b + gap, pos + length - gap, &old);
                        if (got && old.inode && old.name != QLatin1String(".") && old.name != QLatin1String("..")) {
                            old.deleted = true;
                            out << old;
                            gap += got - 4;
                        }
                    }
                }
                pos += length;
            }
        }
        return out;
    }

private:
    quint64 descriptor(quint32 group, int lo, int hi) const
    {
        const uchar *d = bytes(m_descriptors) + quint64(group) * m_descSize;
        return le32(d + lo) | (m_descSize >= 64 ? quint64(le32(d + hi)) << 32 : 0);
    }

    static void append(QVector<Extent> *out, const Extent &e)
    {
        if (e.length == 0)
            return;
        if (!out->isEmpty() && out->last().start != Extent::kZeros && e.start != Extent::kZeros && out->last().end() == e.start)
            out->last().length += e.length;
        else if (!out->isEmpty() && out->last().start == Extent::kZeros && e.start == Extent::kZeros)
            out->last().length += e.length;
        else
            out->push_back(e);
    }

    bool block(quint64 b) const { return b >= m_firstDataBlock && b < blocks; }

    bool extentTree(const uchar *node, quint32 room, int depth, QVector<std::pair<quint64, Extent>> *runs)
    {
        if (room < 12 || le16(node) != 0xF30A || depth > 5)
            return false;
        const quint16 entries = le16(node + 2), levels = le16(node + 6);
        if (12 + quint32(entries) * 12 > room || levels > 5)
            return false;
        for (quint16 k = 0; k < entries; ++k) {
            const uchar *e = node + 12 + k * 12;
            if (levels == 0) {
                quint32 len = le16(e + 4);
                const bool unwritten = len > 32768;
                if (unwritten)
                    len -= 32768;
                const quint64 start = quint64(le16(e + 6)) << 32 | le32(e + 8);
                if (len == 0 || !block(start) || !block(start + len - 1) || runs->size() >= kMaxExtents)
                    return false;
                runs->push_back({le32(e), Extent{unwritten ? Extent::kZeros : m_where.start + start * blockSize, quint64(len) * blockSize}});
            } else {
                const quint64 child = quint64(le16(e + 8)) << 32 | le32(e + 4);
                if (!block(child))
                    return false;
                const QByteArray data = raw::read(m_source, m_where.start + child * blockSize, blockSize);
                if (quint64(data.size()) < blockSize || !extentTree(bytes(data), quint32(blockSize), depth + 1, runs))
                    return false;
            }
        }
        return true;
    }

    bool blockMap(const uchar *map, QVector<std::pair<quint64, Extent>> *runs)
    {
        const quint64 perBlock = blockSize / 4;
        quint64 logical = 0;
        auto add = [&](quint32 b) {
            if (b != 0) {
                if (!block(b) || runs->size() >= kMaxExtents)
                    return false;
                const quint64 at = m_where.start + quint64(b) * blockSize;
                if (!runs->isEmpty() && runs->last().second.end() == at && runs->last().first * blockSize + runs->last().second.length == logical * blockSize)
                    runs->last().second.length += blockSize;
                else
                    runs->push_back({logical, Extent{at, blockSize}});
            }
            ++logical;
            return true;
        };
        // Through `levels` of pointer blocks below `b`.
        std::function<bool(quint32, int)> walk = [&](quint32 b, int levels) {
            if (b == 0) {
                quint64 skip = 1;
                for (int k = 0; k < levels; ++k)
                    skip *= perBlock;
                logical += skip;
                return true;
            }
            if (!block(b))
                return false;
            const QByteArray data = raw::read(m_source, m_where.start + quint64(b) * blockSize, blockSize);
            if (quint64(data.size()) < blockSize)
                return false;
            for (quint64 k = 0; k < perBlock; ++k) {
                const quint32 next = le32(bytes(data) + k * 4);
                if (!(levels == 1 ? add(next) : walk(next, levels - 1)))
                    return false;
            }
            return true;
        };
        for (int k = 0; k < 12; ++k) {
            if (!add(le32(map + k * 4)))
                return false;
        }
        for (int k = 0; k < 3; ++k) {
            if (!walk(le32(map + 48 + k * 4), k + 1))
                return false;
        }
        return true;
    }

    // One folder entry at e, with `room` bytes for it. How many bytes it takes (0: not one).
    quint32 entryAt(const uchar *e, quint32 room, DirEntry *out) const
    {
        if (room < 8)
            return 0;
        const quint32 inode = le32(e);
        const quint32 nameLength = e[6];
        const quint32 used = (8 + nameLength + 3) & ~3u;
        if (nameLength == 0 || used > room || inode > quint64(groups) * inodesPerGroup)
            return 0;
        const uchar type = hasFileTypes() ? e[7] : 0;
        if (type > 7)
            return 0;
        for (quint32 k = 0; k < nameLength; ++k) {
            if (e[8 + k] == 0 || e[8 + k] == '/')
                return 0;
        }
        out->inode = inode;
        out->name = QString::fromUtf8(reinterpret_cast<const char *>(e + 8), int(nameLength));
        out->isDir = type == 2;
        return used;
    }

    Source &m_source;
    Extent m_where;
    quint32 m_incompat = 0, m_descSize = 32;
    quint64 m_firstDataBlock = 0;
    QByteArray m_descriptors;
    QHash<quint32, QByteArray> m_bitmaps;
};

class Lister
{
public:
    Lister(const Batch &out, Volume *volume)
        : m_out(out)
        , m_volume(volume)
    {
    }
    void add(Found &&f)
    {
        if (m_volume) {
            ++m_volume->files;
            if (f.deleted)
                ++m_volume->deleted;
        }
        m_batch.push_back(std::move(f));
        ++m_listed;
        if (m_batch.size() >= kBatch)
            flush();
    }
    void flush()
    {
        if (!m_batch.isEmpty())
            m_out(std::exchange(m_batch, {}));
    }
    bool full() const { return m_listed >= kMaxListed; }

private:
    const Batch &m_out;
    Volume *m_volume;
    QVector<Found> m_batch;
    int m_listed = 0;
};

// What ext4's journal (jbd2) still has: older copies of file records, from before files were
// deleted (with their size and pieces), and of folder blocks, with names. The journal is a ring
// of transactions: a descriptor block lists the file system blocks whose copies follow it.
struct Journal {
    QHash<quint32, QByteArray> records;     // file record -> its newest copy that still had a size
    QHash<quint32, QString> nameOf;         // from the folder blocks' copies
    QHash<quint32, quint32> parentOf;
};

quint32 be32(const uchar *p)
{
    return quint32(p[0]) << 24 | quint32(p[1]) << 16 | quint32(p[2]) << 8 | p[3];
}

Journal readJournal(Ext4 &fs)
{
    Journal j;
    if (!fs.journalInode)
        return j;
    const QByteArray record = fs.inode(fs.journalInode);
    QVector<Extent> extents;
    if (record.isEmpty() || !fs.extentsOf(bytes(record), std::min<quint64>(Ext4::sizeOf(bytes(record)), kMaxJournal), &extents))
        return j;
    const QByteArray log = fs.readExtents(extents, kMaxJournal);
    const qsizetype bs = qsizetype(fs.blockSize);
    if (log.size() < 2 * bs)
        return j;
    const uchar *sb = bytes(log);
    if (be32(sb) != 0xC03B3998 || (be32(sb + 4) != 3 && be32(sb + 4) != 4) || be32(sb + 0x0C) != fs.blockSize)
        return j;
    const quint32 maxlen = std::min<quint32>(be32(sb + 0x10), quint32(log.size() / bs)), first = be32(sb + 0x14);
    const quint32 incompat = be32(sb + 4) == 4 ? be32(sb + 0x28) : 0;
    const bool wide = incompat & 0x2, v3 = incompat & 0x10, v2 = incompat & 0x8;
    const int tagSize = v3 ? (wide ? 16 : 12) : 8 + (wide ? 4 : 0) + (v2 ? 2 : 0);
    if (first == 0 || first >= maxlen)
        return j;
    // Which file system blocks are inode tables, by group.
    const quint64 tableBlocks = (quint64(fs.inodesPerGroup) * fs.inodeSize + fs.blockSize - 1) / fs.blockSize;
    QHash<quint32, quint32> sequenceOf; // record -> the transaction its kept copy is from
    for (quint32 i = first; i < maxlen; ++i) {
        const uchar *d = sb + qsizetype(i) * bs;
        if (be32(d) != 0xC03B3998 || be32(d + 4) != 1)
            continue; // not a descriptor
        const quint32 sequence = be32(d + 8);
        quint32 next = i;
        for (qsizetype at = 12; at + tagSize <= bs;) {
            // Tag: the block (high half at 8 when 64-bit) and its flags (at 4, or 6 in the older kind).
            const quint64 block = be32(d + at) | (wide ? quint64(be32(d + at + 8)) << 32 : 0);
            const quint32 flags = v3 ? be32(d + at + 4) : quint32(d[at + 6]) << 8 | d[at + 7];
            at += tagSize + ((flags & 0x2) ? 0 : 16); // SAME_UUID, or a UUID follows
            next = next + 1 >= maxlen ? first : next + 1;
            QByteArray copy = log.mid(qsizetype(next) * bs, bs);
            if (flags & 0x1) // escaped: its first four bytes were the journal's magic
                memcpy(copy.data(), "\xC0\x3B\x39\x98", 4);
            if (!(flags & 0x4) && copy.size() == bs) {
                // An inode table block: each record in it, newest copy with a size kept.
                for (quint32 g = 0; g < fs.groups; ++g) {
                    const quint64 table = fs.inodeTable(g);
                    if (block < table || block >= table + tableBlocks)
                        continue;
                    const quint32 perBlock = quint32(fs.blockSize / fs.inodeSize);
                    for (quint32 k = 0; k < perBlock; ++k) {
                        const quint32 n = g * fs.inodesPerGroup + quint32(block - table) * perBlock + k + 1;
                        const uchar *ino = bytes(copy) + quint64(k) * fs.inodeSize;
                        if (!Ext4::isFile(ino) || Ext4::sizeOf(ino) == 0 || le16(ino + 0x1A) == 0)
                            continue;
                        if (j.records.contains(n) && qint32(sequence - sequenceOf.value(n)) < 0)
                            continue;
                        j.records.insert(n, QByteArray(reinterpret_cast<const char *>(ino), int(fs.inodeSize)));
                        sequenceOf.insert(n, sequence);
                    }
                    break;
                }
                // A folder's first block ("." and ".."): the names in it, and its parent.
                const QVector<DirEntry> entries = fs.parseFolder(copy, false, false);
                if (entries.size() >= 2 && entries[0].name == QLatin1String(".") && entries[1].name == QLatin1String("..")) {
                    for (const DirEntry &e : entries) {
                        if (e.name == QLatin1String(".") || e.name == QLatin1String(".."))
                            continue;
                        j.nameOf.insert(e.inode, cleanName(e.name));
                        j.parentOf.insert(e.inode, entries[0].inode);
                    }
                    if (entries[1].inode)
                        j.parentOf.insert(entries[0].inode, entries[1].inode);
                }
            }
            if (flags & 0x8) // the last tag
                break;
        }
    }
    return j;
}

// A file from its record. Deleted files whose record has been emptied are listed by name only.
Found fileFrom(Ext4 &fs, const uchar *i, const QString &name, const QString &folder, bool deleted)
{
    Found f;
    f.name = name;
    f.folder = folder;
    f.origin = Origin::FileSystem;
    f.deleted = deleted;
    f.type = filetypes::typeForName(name);
    f.modified = Ext4::when(i, 0x10);
    f.accessed = Ext4::when(i, 0x08);
    f.changed = Ext4::when(i, 0x0C);
    f.created = fs.created(i);
    if (deleted)
        f.deletedAt = Ext4::when(i, 0x14);
    f.size = Ext4::sizeOf(i);
    if (Ext4::inlineData(i, f.size, &f.data))
        return f;
    if (f.size == 0 && deleted) {
        f.nameOnly = true;
        f.note = tr("Linux keeps only the name and dates of a deleted file on ext4: what was in it may still be found by its contents.");
        return f;
    }
    if (!fs.extentsOf(i, f.size, &f.extents)) {
        f.extents.clear();
        f.nameOnly = true;
    }
    return f;
}

} // namespace

bool readExt4(Source &source, const Extent &where, const Batch &out, const std::atomic<bool> &cancel, Volume *volume)
{
    Ext4 fs(source, where);
    if (!fs.open())
        return false;
    const QString top = !fs.label.isEmpty() ? fs.label : tr("ext4 partition at %1").arg(where.start);
    if (volume) {
        volume->type = QStringLiteral("ext4");
        volume->label = top;
        volume->where = where;
        volume->clusterSize = fs.blockSize;
        volume->heap = where.start;
    }
    Lister lister(out, volume);
    QSet<quint32> reached;
    const Journal journal = readJournal(fs);

    // The tree, from the root folder (record 2).
    struct Todo {
        quint32 inode;
        QString path;
        bool deleted;
        int depth;
    };
    QVector<Todo> todo = {{2, top, false, 0}};
    reached.insert(2);
    QHash<quint32, QString> folderPaths = {{2, top}}; // the folders the tree has, for the journal's files
    while (!todo.isEmpty() && !cancel && !lister.full()) {
        const Todo folder = todo.takeLast();
        const QByteArray record = fs.inode(folder.inode);
        if (record.isEmpty() || !Ext4::isFolder(bytes(record)))
            continue;
        QVector<Extent> extents;
        const quint64 size = std::min<quint64>(Ext4::sizeOf(bytes(record)), kMaxFolderBytes);
        if (!fs.extentsOf(bytes(record), size, &extents))
            continue;
        const bool indexed = le32(bytes(record) + 0x20) & kFlagIndex;
        for (const DirEntry &e : fs.parseFolder(fs.readExtents(extents, size), indexed, true)) {
            if (cancel || lister.full())
                break;
            if (e.name == QLatin1String(".") || e.name == QLatin1String(".."))
                continue;
            QByteArray child = fs.inode(e.inode);
            if (child.isEmpty())
                continue;
            // A deleted file whose record lost its size and pieces: the journal's older copy, when
            // the record hasn't gone to another file since.
            bool fromJournal = false;
            if (e.deleted && !fs.inUse(e.inode) && Ext4::sizeOf(bytes(child)) == 0 && journal.records.contains(e.inode)) {
                child = journal.records.value(e.inode);
                fromJournal = true;
            }
            const uchar *c = bytes(child);
            const bool deleted = e.deleted || folder.deleted;
            // A deleted entry whose record went to another file since: only the name is its.
            const bool reused = e.deleted && fs.inUse(e.inode);
            if (!reused && Ext4::isFolder(c)) {
                if (!reached.contains(e.inode) && folder.depth < kMaxDepth) {
                    reached.insert(e.inode);
                    todo.push_back({e.inode, folder.path + QLatin1Char('/') + cleanName(e.name), deleted, folder.depth + 1});
                    if (!deleted)
                        folderPaths.insert(e.inode, folder.path + QLatin1Char('/') + cleanName(e.name));
                }
                continue;
            }
            if (e.isDir || (!reused && !Ext4::isFile(c) && !e.deleted))
                continue; // links, devices and the like
            if (!e.deleted)
                reached.insert(e.inode);
            if (reused) {
                Found f;
                f.name = e.name;
                f.folder = folder.path;
                f.origin = Origin::FileSystem;
                f.deleted = true;
                f.nameOnly = true;
                f.type = filetypes::typeForName(e.name);
                lister.add(std::move(f));
                continue;
            }
            Found f = fileFrom(fs, c, e.name, folder.path, deleted);
            if (fromJournal && !f.nameOnly) {
                reached.insert(e.inode);
                f.note = tr("Where its pieces are comes from ext4's journal, which kept a copy of its record from before it was deleted.");
            }
            lister.add(std::move(f));
        }
    }

    // Files the journal still has a record of whose folder entry is gone too: named from its
    // copies of folder blocks where it can.
    if (!journal.records.isEmpty() && !cancel) {
        const QString fromJournal = tr("%1, from the journal").arg(top);
        QHash<quint32, int> unnamed;
        // Up to a folder that's still there (the usual case: Linux wipes a deleted file's folder
        // entry, but the folder stays), else under "from the journal".
        const auto pathOf = [&](quint32 n) {
            QStringList parts;
            QSet<quint32> seen;
            for (quint32 c = journal.parentOf.value(n); c >= 2 && !seen.contains(c) && parts.size() < kMaxDepth; c = journal.parentOf.value(c)) {
                if (folderPaths.contains(c))
                    return parts.isEmpty() ? folderPaths.value(c) : folderPaths.value(c) + QLatin1Char('/') + parts.join(QLatin1Char('/'));
                if (c == 2)
                    break;
                seen.insert(c);
                if (!journal.nameOf.contains(c) && !unnamed.contains(c))
                    unnamed.insert(c, int(unnamed.size()) + 1);
                parts.prepend(journal.nameOf.contains(c) ? journal.nameOf.value(c) : tr("Unnamed folder %1").arg(unnamed.value(c)));
            }
            return parts.isEmpty() ? fromJournal : fromJournal + QLatin1Char('/') + parts.join(QLatin1Char('/'));
        };
        QList<quint32> numbers = journal.records.keys();
        std::sort(numbers.begin(), numbers.end());
        int nameless = 0;
        for (quint32 n : std::as_const(numbers)) {
            if (cancel || lister.full())
                break;
            if (reached.contains(n) || fs.inUse(n) || n == fs.journalInode)
                continue;
            const QString name = journal.nameOf.value(n);
            Found f = fileFrom(fs, bytes(journal.records[n]), name.isEmpty() ? tr("Nameless file %1").arg(++nameless) : name, pathOf(n), true);
            if (f.nameOnly)
                continue;
            if (name.isEmpty()) {
                f.type = -1;
                f.nameGuessed = true;
            }
            f.note = tr("Found in ext4's journal, which kept a copy of its record from before it was deleted.");
            lister.add(std::move(f));
        }
    }

    // An ext4 made again over an older one (a format) leaves the old records in the parts of the
    // inode tables it hasn't used yet (whole groups it marks as unused, and the end of the others),
    // until Linux gets round to zeroing them. Their files are whole there, pieces and all; their
    // names come from the old folders' records.
    QHash<quint32, QByteArray> oldFiles, oldFolders;
    for (quint32 g = 0; g < fs.groups && !cancel; ++g) {
        const quint16 flags = fs.groupFlags(g);
        if (flags & kGroupTableZeroed)
            continue;
        const quint32 from = (flags & kGroupInodesUnused) ? 0 : fs.inodesPerGroup - fs.neverUsed(g);
        const quint64 table = fs.inodeTable(g);
        if (from >= fs.inodesPerGroup || table == 0 || table >= fs.blocks)
            continue;
        const QByteArray records = raw::read(source, where.start + table * fs.blockSize + quint64(from) * fs.inodeSize,
                                             quint64(fs.inodesPerGroup - from) * fs.inodeSize);
        for (quint32 k = from; k < fs.inodesPerGroup && qsizetype((k - from + 1) * fs.inodeSize) <= records.size(); ++k) {
            const uchar *i = bytes(records) + quint64(k - from) * fs.inodeSize;
            const quint32 n = g * fs.inodesPerGroup + k + 1;
            if (le16(i + 0x1A) == 0 || le32(i + 0x14) != 0 || reached.contains(n))
                continue; // no links, or deleted before the format
            if (Ext4::isFile(i) && Ext4::sizeOf(i) > 0)
                oldFiles.insert(n, QByteArray(reinterpret_cast<const char *>(i), int(fs.inodeSize)));
            else if (Ext4::isFolder(i))
                oldFolders.insert(n, QByteArray(reinterpret_cast<const char *>(i), int(fs.inodeSize)));
        }
    }
    if (!oldFiles.isEmpty()) {
        QHash<quint32, QString> nameOf;
        QHash<quint32, quint32> parentOf;
        for (auto it = oldFolders.cbegin(); it != oldFolders.cend() && !cancel; ++it) {
            QVector<Extent> extents;
            const quint64 size = std::min<quint64>(Ext4::sizeOf(bytes(*it)), kMaxFolderBytes);
            if (!fs.extentsOf(bytes(*it), size, &extents))
                continue;
            for (const DirEntry &e : fs.parseFolder(fs.readExtents(extents, size), le32(bytes(*it) + 0x20) & kFlagIndex, false)) {
                if (e.name == QLatin1String("..")) {
                    parentOf.insert(it.key(), e.inode);
                } else if (e.name != QLatin1String(".")) {
                    nameOf.insert(e.inode, cleanName(e.name));
                    if (!parentOf.contains(e.inode))
                        parentOf.insert(e.inode, it.key());
                }
            }
        }
        const QString before = tr("%1, before it was formatted").arg(top);
        QHash<quint32, int> unnamed;
        const auto pathOf = [&](quint32 n) {
            QStringList parts;
            QSet<quint32> seen;
            for (quint32 c = parentOf.value(n); c > 2 && !seen.contains(c) && parts.size() < kMaxDepth; c = parentOf.value(c)) {
                seen.insert(c);
                if (!nameOf.contains(c) && !unnamed.contains(c))
                    unnamed.insert(c, int(unnamed.size()) + 1);
                parts.prepend(nameOf.contains(c) ? nameOf.value(c) : tr("Unnamed folder %1").arg(unnamed.value(c)));
            }
            return parts.isEmpty() ? before : before + QLatin1Char('/') + parts.join(QLatin1Char('/'));
        };
        QList<quint32> numbers = oldFiles.keys();
        std::sort(numbers.begin(), numbers.end());
        int nameless = 0;
        for (quint32 n : std::as_const(numbers)) {
            if (cancel || lister.full())
                break;
            const uchar *i = bytes(oldFiles[n]);
            QString name = nameOf.value(n);
            Found f = fileFrom(fs, i, name, pathOf(n), true);
            if (f.nameOnly)
                continue; // its record doesn't add up: not one after all
            if (name.isEmpty()) {
                // Named after what it starts with, so it opens in the right program.
                QString ext;
                if (!f.extents.isEmpty() && f.extents.first().start != Extent::kZeros) {
                    const QByteArray head = raw::read(source, f.extents.first().start, 512);
                    const QVector<int> kinds = head.size() >= 32 ? filetypes::candidates(bytes(head), int(head.size())) : QVector<int>();
                    if (!kinds.isEmpty())
                        f.type = kinds.first();
                    else if (head.size() >= 32 && filetypes::plainTextStart(bytes(head), int(head.size())))
                        f.type = filetypes::typeNumber(QStringLiteral("txt"));
                    if (f.type >= 0)
                        ext = QLatin1Char('.') + filetypes::types()[f.type].id;
                }
                f.name = tr("Nameless file %1").arg(++nameless) + ext;
            }
            f.note = tr("Found in a file record left from before the drive was formatted.");
            lister.add(std::move(f));
        }
        qCInfo(lcOps).noquote() << "Find Lost Files: ext4" << top << "has" << oldFiles.size() << "files from before a format," << nameOf.size() << "named";
    }
    lister.flush();
    return true;
}

} // namespace lost
