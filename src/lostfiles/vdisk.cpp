// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vdisk.h"

#include "lostraw.h"
#include "zstdseek.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QRegularExpression>
#include <QUuid>
#include <QtEndian>

#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <cstring>

namespace vdisk {

namespace {

using lost::raw::bytes;
using lost::raw::le16;
using lost::raw::le32;
using lost::raw::le64;

constexpr quint64 kMiB = 1024 * 1024;

QString tr(const char *text)
{
    return QCoreApplication::translate("vdisk", text);
}

quint32 be32(const uchar *p)
{
    return qFromBigEndian<quint32>(p);
}

quint64 be64(const uchar *p)
{
    return qFromBigEndian<quint64>(p);
}

bool powerOfTwo(quint64 n)
{
    return n != 0 && (n & (n - 1)) == 0;
}

// The file the disk is kept in: `len` bytes at `at`, zeros past its end.
QByteArray readAt(lost::Source &file, quint64 at, quint64 len)
{
    QByteArray out(qsizetype(len), '\0');
    if (at < file.size())
        file.read(at, out.data(), std::min(len, file.size() - at));
    return out;
}

// Whether a table said to be there fits in the file: a damaged header doesn't get to make it
// read (and keep) far more than the file holds.
bool within(const lost::Source &file, quint64 at, quint64 len)
{
    return at <= file.size() && len <= file.size() - at;
}

void readInto(lost::Source &file, quint64 at, char *buf, quint64 len)
{
    const quint64 n = at < file.size() ? std::min(len, file.size() - at) : 0;
    if (n > 0)
        file.read(at, buf, n);
    memset(buf + n, 0, len - n);
}

// A virtual disk: the bytes of the disk inside, worked out piece by piece.
class Disk : public lost::Layer
{
public:
    quint64 size() const override { return m_size; }

    bool read(quint64 offset, char *buf, quint64 len) override
    {
        while (len > 0) {
            const quint64 n = std::max<quint64>(1, std::min(len, readPart(offset, buf, len)));
            offset += n;
            buf += n;
            len -= n;
        }
        return true;
    }

protected:
    // As much from `at` as is kept in one piece of the file (at most `len`) into `buf`; returns
    // how much.
    virtual quint64 readPart(quint64 at, char *buf, quint64 len) = 0;

    quint64 m_size = 0;
};

// A few blocks kept after they were worked out (unpacked, or a table read), for the reads that
// come next: most come one after another.
class Cache
{
public:
    bool get(quint64 key, QByteArray *out) const
    {
        QMutexLocker locker(&m_lock);
        const auto it = m_blocks.constFind(key);
        if (it == m_blocks.cend())
            return false;
        *out = *it;
        return true;
    }

    void put(quint64 key, const QByteArray &data)
    {
        QMutexLocker locker(&m_lock);
        if (m_blocks.size() >= 64)
            m_blocks.clear();
        m_blocks.insert(key, data);
    }

private:
    mutable QMutex m_lock;
    QHash<quint64, QByteArray> m_blocks;
};

// zlib's own wrapping (raw = false) or plain deflate (raw = true), into exactly `size` bytes.
bool inflateTo(const QByteArray &packed, bool raw, quint64 size, QByteArray *out)
{
    out->resize(qsizetype(size));
    z_stream z{};
    if (inflateInit2(&z, raw ? -15 : 15) != Z_OK)
        return false;
    z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(packed.constData()));
    z.avail_in = uInt(packed.size());
    z.next_out = reinterpret_cast<Bytef *>(out->data());
    z.avail_out = uInt(size);
    const int result = inflate(&z, Z_FINISH);
    const bool ok = (result == Z_STREAM_END || result == Z_BUF_ERROR || result == Z_OK) && z.avail_out == 0;
    inflateEnd(&z);
    return ok;
}

// --- VirtualBox: a header, a map of blocks (1 MiB each, usually), the blocks.

class Vdi : public Disk
{
public:
    static std::unique_ptr<Vdi> open(std::shared_ptr<lost::Source> file, QString *problem)
    {
        const QByteArray h = readAt(*file, 0, 512);
        const uchar *p = bytes(h);
        if (le32(p + 0x40) != 0xBEDA107F)
            return nullptr;
        if (le32(p + 0x44) >> 16 != 1) {
            *problem = tr("It's from a version of VirtualBox whose disks DiskForge can't read.");
            return nullptr;
        }
        const quint32 type = le32(p + 0x4C);
        if (type == 3 || type == 4) {
            *problem = tr("It only holds what changed since another of VirtualBox's disks (a snapshot), so it can't be read on its own.");
            return nullptr;
        }
        auto d = std::make_unique<Vdi>();
        d->m_file = std::move(file);
        const quint64 blocksAt = le32(p + 0x154);
        d->m_dataAt = le32(p + 0x158);
        d->m_size = le64(p + 0x170);
        d->m_block = le32(p + 0x178);
        d->m_extra = le32(p + 0x17C);
        const quint64 blocks = le32(p + 0x180);
        if (!powerOfTwo(d->m_block) || d->m_block < 512 || blocks > (1u << 28) || blocks * d->m_block < d->m_size || !within(*d->m_file, blocksAt, blocks * 4)) {
            *problem = tr("Its header is damaged.");
            return nullptr;
        }
        const QByteArray map = readAt(*d->m_file, blocksAt, blocks * 4);
        d->m_map.resize(qsizetype(blocks));
        for (quint64 i = 0; i < blocks; ++i)
            d->m_map[qsizetype(i)] = le32(bytes(map) + i * 4);
        return d;
    }

protected:
    quint64 readPart(quint64 at, char *buf, quint64 len) override
    {
        const quint64 b = at / m_block, in = at % m_block, n = std::min(len, m_block - in);
        const quint32 where = b < quint64(m_map.size()) ? m_map[qsizetype(b)] : 0xFFFFFFFF;
        if (where >= 0xFFFFFFFE) // not written to, or written with zeros
            memset(buf, 0, n);
        else
            readInto(*m_file, m_dataAt + quint64(where) * (m_block + m_extra) + m_extra + in, buf, n);
        return n;
    }

private:
    std::shared_ptr<lost::Source> m_file;
    quint64 m_dataAt = 0, m_block = 0, m_extra = 0;
    QVector<quint32> m_map;
};

// --- Microsoft's VHD: a footer (and for a growing disk a header and a block table); big-endian.

class Vhd : public Disk
{
public:
    static std::unique_ptr<Vhd> open(std::shared_ptr<lost::Source> file, QString *problem)
    {
        const quint64 end = file->size();
        QByteArray footer = readAt(*file, end >= 512 ? end - 512 : 0, 512);
        if (!footer.startsWith("conectix"))
            footer = readAt(*file, 0, 512); // a growing disk keeps a copy at the start
        if (!footer.startsWith("conectix"))
            return nullptr;
        const uchar *f = bytes(footer);
        auto d = std::make_unique<Vhd>();
        d->m_file = std::move(file);
        d->m_size = be64(f + 0x30);
        const quint32 type = be32(f + 0x3C);
        if (type == 2) { // fixed: the disk, then the footer
            d->m_fixed = true;
            if (d->m_size > end) {
                *problem = tr("The file is shorter than the disk it holds: it was cut off.");
                return nullptr;
            }
            return d;
        }
        if (type != 3 && type != 4) {
            *problem = tr("Its header is damaged.");
            return nullptr;
        }
        const QByteArray header = readAt(*d->m_file, be64(f + 0x10), 1024);
        const uchar *h = bytes(header);
        if (!header.startsWith("cxsparse")) {
            *problem = tr("Its header is damaged.");
            return nullptr;
        }
        if (type == 4) {
            QString parent;
            for (int i = 0; i < 256 && qFromBigEndian<quint16>(h + 0x40 + 2 * i); ++i)
                parent += QChar(char16_t(qFromBigEndian<quint16>(h + 0x40 + 2 * i)));
            *problem = tr("It only holds what changed since another disk (%1), so it can't be read on its own.").arg(parent);
            return nullptr;
        }
        const quint64 tableAt = be64(h + 0x10);
        const quint64 entries = be32(h + 0x1C);
        d->m_block = be32(h + 0x20);
        if (!powerOfTwo(d->m_block) || d->m_block < 512 || entries > (1u << 28) || entries * d->m_block < d->m_size
            || !within(*d->m_file, tableAt, entries * 4)) {
            *problem = tr("Its header is damaged.");
            return nullptr;
        }
        // Each block starts with a bitmap of its sectors, rounded up to whole sectors.
        d->m_bitmap = ((d->m_block / 512 + 7) / 8 + 511) / 512 * 512;
        const QByteArray table = readAt(*d->m_file, tableAt, entries * 4);
        d->m_table.resize(qsizetype(entries));
        for (quint64 i = 0; i < entries; ++i)
            d->m_table[qsizetype(i)] = be32(bytes(table) + i * 4);
        return d;
    }

protected:
    quint64 readPart(quint64 at, char *buf, quint64 len) override
    {
        if (m_fixed) {
            readInto(*m_file, at, buf, len);
            return len;
        }
        const quint64 b = at / m_block, in = at % m_block, n = std::min(len, m_block - in);
        const quint32 sector = b < quint64(m_table.size()) ? m_table[qsizetype(b)] : 0xFFFFFFFF;
        if (sector == 0xFFFFFFFF)
            memset(buf, 0, n); // never written
        else
            readInto(*m_file, quint64(sector) * 512 + m_bitmap + in, buf, n);
        return n;
    }

private:
    std::shared_ptr<lost::Source> m_file;
    bool m_fixed = false;
    quint64 m_block = 0, m_bitmap = 0;
    QVector<quint32> m_table;
};

// --- Microsoft's VHDX: two headers and two region tables, each with a CRC-32C; metadata; a
// block table in which each entry says where a block is (in whole MiB) and whether it's there.

quint32 crc32c(const char *data, qsizetype size)
{
    static const auto table = [] {
        std::array<quint32, 256> t{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    quint32 crc = 0xFFFFFFFFu;
    for (qsizetype i = 0; i < size; ++i)
        crc = table[(crc ^ uchar(data[i])) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// A structure whose CRC-32C (over it with its own field zeroed) sits at offset 4.
bool checksumOk(QByteArray block)
{
    if (block.size() < 8)
        return false;
    const quint32 stored = le32(bytes(block) + 4);
    memset(block.data() + 4, 0, 4);
    return crc32c(block.constData(), block.size()) == stored;
}

QString guid(const uchar *p)
{
    return QUuid(le32(p), le16(p + 4), le16(p + 6), p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]).toString(QUuid::WithoutBraces).toUpper();
}

class Vhdx : public Disk
{
public:
    static std::unique_ptr<Vhdx> open(std::shared_ptr<lost::Source> file, QString *problem, QString *warning)
    {
        if (!readAt(*file, 0, 8).startsWith("vhdxfile"))
            return nullptr;
        // The newer of the two headers that check out.
        QByteArray header;
        quint64 newest = 0;
        for (const quint64 at : {64 * 1024, 128 * 1024}) {
            const QByteArray h = readAt(*file, at, 4096);
            if (h.startsWith("head") && checksumOk(h) && (header.isEmpty() || le64(bytes(h) + 8) > newest)) {
                header = h;
                newest = le64(bytes(h) + 8);
            }
        }
        QByteArray regions;
        for (const quint64 at : {192 * 1024, 256 * 1024}) {
            const QByteArray r = readAt(*file, at, 64 * 1024);
            if (regions.isEmpty() && r.startsWith("regi") && checksumOk(r))
                regions = r;
        }
        if (header.isEmpty() || regions.isEmpty()) {
            *problem = tr("Its headers are damaged.");
            return nullptr;
        }
        static const uchar noLog[16] = {};
        if (memcmp(bytes(header) + 48, noLog, 16) != 0 && warning)
            *warning = tr("It wasn't closed properly (the computer or program stopped while it was in use), so its last changes may be "
                          "missing here.");

        quint64 tableAt = 0, tableLength = 0, metaAt = 0, metaLength = 0;
        const uchar *r = bytes(regions);
        const quint32 count = std::min<quint32>(le32(r + 8), 2047);
        for (quint32 i = 0; i < count; ++i) {
            const uchar *e = r + 16 + i * 32;
            const QString id = guid(e);
            if (id == QLatin1String("2DC27766-F623-4200-9D64-115E9BFD4A08")) {
                tableAt = le64(e + 16);
                tableLength = le32(e + 24);
            } else if (id == QLatin1String("8B7CA206-4790-4B9A-B8FE-575F050F886E")) {
                metaAt = le64(e + 16);
                metaLength = le32(e + 24);
            } else if (le32(e + 28) & 1) {
                *problem = tr("It uses a part of the format DiskForge doesn't know yet.");
                return nullptr;
            }
        }
        const QByteArray meta = readAt(*file, metaAt, std::min<quint64>(metaLength, 1 * kMiB));
        if (!meta.startsWith("metadata") || tableLength == 0 || !within(*file, tableAt, tableLength)) {
            *problem = tr("Its headers are damaged.");
            return nullptr;
        }
        auto d = std::make_unique<Vhdx>();
        quint32 sectorSize = 512, flags = 0;
        const uchar *m = bytes(meta);
        const quint32 items = std::min<quint32>(le16(m + 10), 2047);
        for (quint32 i = 0; i < items; ++i) {
            const uchar *e = m + 32 + i * 32;
            const QString id = guid(e);
            const quint64 at = le32(e + 16), length = le32(e + 20);
            if (at + std::max<quint64>(length, 8) > quint64(meta.size()))
                continue;
            if (id == QLatin1String("CAA16737-FA36-4D43-B3B6-33F0AA44E76B")) {
                d->m_block = le32(m + at);
                flags = le32(m + at + 4);
            } else if (id == QLatin1String("2FA54224-CD1B-4876-B211-5DBED83BF4B8")) {
                d->m_size = le64(m + at);
            } else if (id == QLatin1String("8141BF1D-A96F-4709-BA47-F233A8FAAB5F")) {
                sectorSize = le32(m + at);
            }
        }
        if (flags & 2) {
            *problem = tr("It only holds what changed since another disk, so it can't be read on its own.");
            return nullptr;
        }
        if (!powerOfTwo(d->m_block) || d->m_block < kMiB || d->m_block > 256 * kMiB || (sectorSize != 512 && sectorSize != 4096) || d->m_size == 0) {
            *problem = tr("Its headers are damaged.");
            return nullptr;
        }
        // After every so many blocks comes an entry for a bitmap the table only uses for
        // differencing disks.
        d->m_chunk = (quint64(1) << 23) * sectorSize / d->m_block;
        const quint64 blocks = (d->m_size + d->m_block - 1) / d->m_block;
        const quint64 entries = std::min(blocks + blocks / d->m_chunk + 1, tableLength / 8);
        const QByteArray table = readAt(*file, tableAt, entries * 8);
        d->m_table.resize(qsizetype(entries));
        for (quint64 i = 0; i < entries; ++i)
            d->m_table[qsizetype(i)] = le64(bytes(table) + i * 8);
        d->m_file = std::move(file);
        return d;
    }

protected:
    quint64 readPart(quint64 at, char *buf, quint64 len) override
    {
        const quint64 b = at / m_block, in = at % m_block, n = std::min(len, m_block - in);
        const quint64 i = b + b / m_chunk;
        const quint64 entry = i < quint64(m_table.size()) ? m_table[qsizetype(i)] : 0;
        const int state = int(entry & 7);
        if (state == 6 || state == 7) // there (wholly, or partly for a differencing disk)
            readInto(*m_file, (entry >> 20) * kMiB + in, buf, n);
        else
            memset(buf, 0, n); // not there, or zeros, or trimmed
        return n;
    }

private:
    std::shared_ptr<lost::Source> m_file;
    quint64 m_block = 0, m_chunk = 1;
    QVector<quint64> m_table;
};

// --- QEMU's qcow2: a two-level table of clusters, some of them packed; big-endian.

class Qcow2 : public Disk
{
public:
    static std::unique_ptr<Qcow2> open(std::shared_ptr<lost::Source> file, QString *problem, QString *warning)
    {
        const QByteArray h = readAt(*file, 0, 512);
        const uchar *p = bytes(h);
        if (!h.startsWith("QFI\xfb"))
            return nullptr;
        const quint32 version = be32(p + 4);
        if (version != 2 && version != 3) {
            *problem = tr("It's from a version of QEMU whose disks DiskForge can't read.");
            return nullptr;
        }
        auto d = std::make_unique<Qcow2>();
        if (const quint64 backingAt = be64(p + 8); backingAt != 0) {
            const quint32 length = std::min<quint32>(be32(p + 16), 1023);
            *problem = tr("It only holds what changed since another disk (%1), so it can't be read on its own.")
                           .arg(QString::fromUtf8(readAt(*file, backingAt, length)));
            return nullptr;
        }
        d->m_bits = be32(p + 20);
        d->m_size = be64(p + 24);
        if (be32(p + 32) != 0) {
            *problem = tr("It's encrypted.");
            return nullptr;
        }
        const quint64 l1Size = be32(p + 36), l1At = be64(p + 40);
        if (version == 3) {
            const quint64 incompatible = be64(p + 72);
            if (incompatible & 4) {
                *problem = tr("Its data is kept in another file, which DiskForge doesn't read yet.");
                return nullptr;
            }
            if (incompatible & ~quint64(0x1f)) {
                *problem = tr("It uses a part of the format DiskForge doesn't know yet.");
                return nullptr;
            }
            d->m_extended = incompatible & 16;
            if ((incompatible & 8) && be32(p + 100) > 104)
                d->m_zstd = p[104] == 1;
            if ((incompatible & 3) && warning)
                *warning = tr("QEMU marked it as not closed properly, so its last changes may be missing here.");
        }
        if (d->m_bits < 9 || d->m_bits > 21 || l1Size > (1u << 26) || !within(*file, l1At, l1Size * 8)) {
            *problem = tr("Its header is damaged.");
            return nullptr;
        }
        d->m_cluster = quint64(1) << d->m_bits;
        d->m_perTable = d->m_cluster / (d->m_extended ? 16 : 8);
        const QByteArray l1 = readAt(*file, l1At, l1Size * 8);
        d->m_l1.resize(qsizetype(l1Size));
        for (quint64 i = 0; i < l1Size; ++i)
            d->m_l1[qsizetype(i)] = be64(bytes(l1) + i * 8) & 0x00FFFFFFFFFFFE00ull;
        d->m_file = std::move(file);
        return d;
    }

protected:
    quint64 readPart(quint64 at, char *buf, quint64 len) override
    {
        const quint64 cluster = at >> m_bits, in = at & (m_cluster - 1);
        quint64 n = std::min(len, m_cluster - in);
        const quint64 table = cluster / m_perTable, index = cluster % m_perTable;
        const quint64 tableAt = table < quint64(m_l1.size()) ? m_l1[qsizetype(table)] : 0;
        if (tableAt == 0) {
            memset(buf, 0, n);
            return n;
        }
        const int entrySize = m_extended ? 16 : 8;
        const QByteArray e = readAt(*m_file, tableAt + index * entrySize, entrySize);
        const quint64 entry = be64(bytes(e));
        if (entry & (quint64(1) << 62)) {
            // Packed: where, and how many more 512-byte sectors after the first one it takes.
            const int x = 62 - int(m_bits - 8);
            const quint64 from = entry & ((quint64(1) << x) - 1);
            const quint64 extra = (entry >> x) & ((quint64(1) << (62 - x)) - 1);
            QByteArray unpacked;
            if (!m_unpacked.get(cluster, &unpacked)) {
                const QByteArray packed = readAt(*m_file, from, (extra + 1) * 512 - (from & 511));
                bool ok = false;
                if (m_zstd) {
                    // The frame alone: what's after it is only padding to a whole sector.
                    unpacked.resize(qsizetype(m_cluster));
                    const size_t frame = ZSTD_findFrameCompressedSize(packed.constData(), size_t(packed.size()));
                    const size_t got = ZSTD_isError(frame) ? frame : ZSTD_decompress(unpacked.data(), size_t(m_cluster), packed.constData(), frame);
                    ok = !ZSTD_isError(got) && got == m_cluster;
                } else {
                    ok = inflateTo(packed, true, m_cluster, &unpacked);
                }
                if (!ok)
                    unpacked = QByteArray(qsizetype(m_cluster), '\0');
                m_unpacked.put(cluster, unpacked);
            }
            memcpy(buf, unpacked.constData() + in, n);
            return n;
        }
        const quint64 host = entry & 0x00FFFFFFFFFFFE00ull;
        if (m_extended) {
            // In 32 parts, each there, or zeros, or not written.
            const quint64 bitmap = be64(bytes(e) + 8);
            const quint64 part = m_cluster / 32, which = in / part;
            n = std::min(n, part - in % part);
            if (((bitmap >> (32 + which)) & 1) || !((bitmap >> which) & 1))
                memset(buf, 0, n);
            else
                readInto(*m_file, host + in, buf, n);
            return n;
        }
        if (host == 0 || (entry & 1))
            memset(buf, 0, n); // never written, or written with zeros
        else
            readInto(*m_file, host + in, buf, n);
        return n;
    }

private:
    std::shared_ptr<lost::Source> m_file;
    quint64 m_bits = 16, m_cluster = 65536, m_perTable = 8192;
    bool m_extended = false, m_zstd = false;
    QVector<quint64> m_l1;
    Cache m_unpacked;
};

// --- VMware's VMDK: a text descriptor naming the files the disk is in ("extents"), each a raw
// file or a sparse one: a header, a grain directory and grain tables, the grains (64 KiB
// usually), packed one by one in a stream-optimized disk.

class SparseExtent : public Disk
{
public:
    static std::unique_ptr<SparseExtent> open(std::shared_ptr<lost::Source> file, QString *problem, QString *warning)
    {
        QByteArray h = readAt(*file, 0, 512);
        const uchar *p = bytes(h);
        if (le32(p) != 0x564D444B) // "KDMV"
            return nullptr;
        if (le64(p + 0x38) == ~quint64(0)) {
            // Stream-optimized: the real header is the footer, two sectors from the end.
            h = file->size() >= 1536 ? readAt(*file, file->size() - 1024, 512) : QByteArray();
            p = bytes(h);
            if (h.size() < 512 || le32(p) != 0x564D444B) {
                *problem = tr("It was cut off: its end is missing.");
                return nullptr;
            }
        }
        auto d = std::make_unique<SparseExtent>();
        d->m_size = le64(p + 0x0C) * 512;
        d->m_grain = le64(p + 0x14) * 512;
        d->m_perTable = le32(p + 0x2C);
        const quint64 directoryAt = le64(p + 0x38) * 512;
        d->m_packed = (le32(p + 0x08) & (1u << 16)) && le16(p + 0x4D) == 1;
        if (p[0x48] && warning)
            *warning = tr("VMware marked it as not closed properly, so its last changes may be missing here.");
        if (!powerOfTwo(d->m_grain) || d->m_grain < 4096 || d->m_grain > 16 * kMiB || !powerOfTwo(d->m_perTable) || d->m_perTable > 65536
            || d->m_size == 0 || directoryAt == 0) {
            *problem = tr("Its header is damaged.");
            return nullptr;
        }
        const quint64 tables = (d->m_size / d->m_grain + d->m_perTable) / d->m_perTable;
        if (tables > (1u << 24) || !within(*file, directoryAt, tables * 4)) {
            *problem = tr("Its header is damaged.");
            return nullptr;
        }
        const QByteArray directory = readAt(*file, directoryAt, tables * 4);
        d->m_directory.resize(qsizetype(tables));
        for (quint64 i = 0; i < tables; ++i)
            d->m_directory[qsizetype(i)] = le32(bytes(directory) + i * 4);
        d->m_file = std::move(file);
        return d;
    }

    // The descriptor kept inside a sparse file, if there's one.
    static QString descriptor(lost::Source &file)
    {
        const QByteArray h = readAt(file, 0, 512);
        const quint64 at = le64(bytes(h) + 0x1C), length = std::min<quint64>(le64(bytes(h) + 0x24), 2048);
        return at && length ? QString::fromUtf8(readAt(file, at * 512, length * 512)) : QString();
    }

protected:
    quint64 readPart(quint64 at, char *buf, quint64 len) override
    {
        const quint64 grain = at / m_grain, in = at % m_grain, n = std::min(len, m_grain - in);
        const quint64 table = grain / m_perTable;
        const quint32 tableAt = table < quint64(m_directory.size()) ? m_directory[qsizetype(table)] : 0;
        quint32 sector = 0;
        if (tableAt != 0)
            sector = le32(bytes(readAt(*m_file, quint64(tableAt) * 512 + (grain % m_perTable) * 4, 4)));
        if (sector <= 1) { // never written (0), or zeros (1)
            memset(buf, 0, n);
            return n;
        }
        if (!m_packed) {
            readInto(*m_file, quint64(sector) * 512 + in, buf, n);
            return n;
        }
        // A packed grain: which sector of the disk it is, its packed size, then zlib's stream.
        QByteArray unpacked;
        if (!m_unpacked.get(grain, &unpacked)) {
            const QByteArray marker = readAt(*m_file, quint64(sector) * 512, 12);
            const quint32 packedSize = std::min<quint32>(le32(bytes(marker) + 8), quint32(2 * m_grain + 1024));
            if (!inflateTo(readAt(*m_file, quint64(sector) * 512 + 12, packedSize), false, m_grain, &unpacked))
                unpacked = QByteArray(qsizetype(m_grain), '\0');
            m_unpacked.put(grain, unpacked);
        }
        memcpy(buf, unpacked.constData() + in, n);
        return n;
    }

private:
    std::shared_ptr<lost::Source> m_file;
    quint64 m_grain = 65536, m_perTable = 512;
    bool m_packed = false;
    QVector<quint32> m_directory;
    Cache m_unpacked;
};

class Vmdk : public Disk
{
public:
    static std::unique_ptr<Vmdk> open(const QString &path, std::shared_ptr<lost::Source> file, QString *problem, QString *warning)
    {
        const QByteArray start = readAt(*file, 0, 512);
        auto d = std::make_unique<Vmdk>();
        QString text;
        if (le32(bytes(start)) == 0x564D444B) {
            text = SparseExtent::descriptor(*file);
            if (!readParent(text, problem))
                return nullptr;
            auto sparse = SparseExtent::open(file, problem, warning);
            if (!sparse)
                return nullptr;
            const quint64 size = sparse->size(); // before it's moved away
            d->add(size, std::move(sparse));
            return d;
        }
        if (file->size() > kMiB || !start.contains("# Disk DescriptorFile"))
            return nullptr;
        text = QString::fromUtf8(readAt(*file, 0, file->size()));
        if (!readParent(text, problem))
            return nullptr;
        // RW 2097152 FLAT "disk-flat.vmdk" 0 / RW 4192256 SPARSE "disk-s001.vmdk" / RW 1024 ZERO
        static const QRegularExpression extent(QStringLiteral("^\\s*(RW|RDONLY|NOACCESS)\\s+(\\d+)\\s+(\\w+)\\s*(?:\"([^\"]*)\"\\s*(\\d*))?"),
                                               QRegularExpression::MultilineOption);
        const QDir folder = QFileInfo(path).absoluteDir();
        for (auto it = extent.globalMatch(text); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            const quint64 size = m.captured(2).toULongLong() * 512;
            const QString kind = m.captured(3).toUpper();
            if (kind == QLatin1String("ZERO")) {
                d->add(size, nullptr);
                continue;
            }
            QString error;
            auto part = lost::Source::fromImage(folder.filePath(m.captured(4)), &error);
            if (!part) {
                *problem = tr("One of the files it's in is missing: %1.").arg(m.captured(4));
                return nullptr;
            }
            if (kind == QLatin1String("FLAT") || kind == QLatin1String("VMFS")) {
                d->add(size, nullptr, std::move(part), m.captured(5).toULongLong() * 512);
            } else if (kind == QLatin1String("SPARSE")) {
                auto sparse = SparseExtent::open(std::move(part), problem, warning);
                if (!sparse) {
                    if (problem->isEmpty())
                        *problem = tr("One of the files it's in is damaged: %1.").arg(m.captured(4));
                    return nullptr;
                }
                d->add(size, std::move(sparse));
            } else {
                *problem = tr("It's a kind of VMware disk DiskForge can't read yet (%1).").arg(kind);
                return nullptr;
            }
        }
        if (d->m_parts.isEmpty()) {
            *problem = tr("Its descriptor doesn't list any of the files the disk is in.");
            return nullptr;
        }
        return d;
    }

protected:
    quint64 readPart(quint64 at, char *buf, quint64 len) override
    {
        // The part `at` is in: the last one starting at or before it.
        const auto it = std::upper_bound(m_parts.cbegin(), m_parts.cend(), at, [](quint64 v, const Part &p) { return v < p.start; });
        if (it == m_parts.cbegin()) {
            memset(buf, 0, len);
            return len;
        }
        const Part &p = *(it - 1);
        const quint64 in = at - p.start;
        const quint64 n = in < p.size ? std::min(len, p.size - in) : len;
        if (in >= p.size)
            memset(buf, 0, n);
        else if (p.sparse)
            p.sparse->read(in, buf, n);
        else if (p.flat)
            readInto(*p.flat, p.offset + in, buf, n);
        else
            memset(buf, 0, n);
        return n;
    }

private:
    struct Part {
        quint64 start = 0, size = 0, offset = 0;
        std::shared_ptr<SparseExtent> sparse;
        std::shared_ptr<lost::Source> flat;
    };

    void add(quint64 size, std::unique_ptr<SparseExtent> sparse, std::shared_ptr<lost::Source> flat = {}, quint64 offset = 0)
    {
        m_parts.push_back({m_size, size, offset, std::move(sparse), std::move(flat)});
        m_size += size;
    }

    // A disk that's only the changes made since another one ("parentCID") isn't readable alone.
    static bool readParent(const QString &text, QString *problem)
    {
        static const QRegularExpression parent(QStringLiteral("^\\s*parentCID\\s*=\\s*(\\w+)"), QRegularExpression::MultilineOption);
        static const QRegularExpression hint(QStringLiteral("^\\s*parentFileNameHint\\s*=\\s*\"([^\"]*)\""), QRegularExpression::MultilineOption);
        const QRegularExpressionMatch m = parent.match(text);
        if (!m.hasMatch() || m.captured(1).compare(QLatin1String("ffffffff"), Qt::CaseInsensitive) == 0)
            return true;
        *problem = tr("It only holds what changed since another disk (%1), so it can't be read on its own.").arg(hint.match(text).captured(1));
        return false;
    }

    QVector<Part> m_parts;
};

} // namespace

// DiskForge's own backups (.img.zst, 0.5.2 on): zstd's seekable format, read a frame at a time.
class ZstdBackup : public lost::Layer
{
public:
    static std::unique_ptr<ZstdBackup> open(std::shared_ptr<lost::Source> file, QString *problem)
    {
        auto table = zstdseek::readTable(file->size(), [&file](quint64 offset, char *buf, quint64 len) { return file->read(offset, buf, len) == len; });
        if (!table) {
            *problem = tr("This backup was made in one piece, before DiskForge 0.5.2: restore it, or unpack it first (zstd -d) and open the "
                          ".img.");
            return nullptr;
        }
        auto z = std::make_unique<ZstdBackup>();
        z->m_file = std::move(file);
        z->m_frames = std::move(*table);
        z->m_size = z->m_frames.last().offset + z->m_frames.last().size;
        return z;
    }
    ~ZstdBackup() override { ZSTD_freeDCtx(m_ctx); }

    quint64 size() const override { return m_size; }

    bool read(quint64 offset, char *buf, quint64 len) override
    {
        QMutexLocker locker(&m_lock);
        // The frame holding `offset`: the last one starting at or before it.
        auto it = std::upper_bound(m_frames.cbegin(), m_frames.cend(), offset, [](quint64 at, const zstdseek::Frame &f) { return at < f.offset; });
        while (len > 0 && it != m_frames.cbegin()) {
            const zstdseek::Frame &f = *(it - 1);
            const QByteArray *data = frame(int(it - 1 - m_frames.cbegin()));
            if (!data || quint64(data->size()) != f.size)
                return false;
            const quint64 from = offset - f.offset, n = std::min<quint64>(len, f.size - from);
            memcpy(buf, data->constData() + from, n);
            offset += n;
            buf += n;
            len -= n;
            if (it == m_frames.cend())
                break;
            ++it;
        }
        return len == 0;
    }

private:
    // A frame, unpacked; the last few are kept.
    const QByteArray *frame(int index)
    {
        if (m_cache.contains(index))
            return &m_cache[index];
        const zstdseek::Frame &f = m_frames.at(index);
        QByteArray packed(qsizetype(f.compressedSize), '\0');
        if (m_file->read(f.compressedOffset, packed.data(), f.compressedSize) != f.compressedSize)
            return nullptr;
        QByteArray out(qsizetype(f.size), '\0');
        const size_t r = ZSTD_decompressDCtx(m_ctx, out.data(), size_t(out.size()), packed.constData(), size_t(packed.size()));
        if (ZSTD_isError(r) || r != f.size)
            return nullptr;
        if (m_order.size() >= 6)
            m_cache.remove(m_order.takeFirst());
        m_order << index;
        m_cache.insert(index, out);
        return &m_cache[index];
    }

    std::shared_ptr<lost::Source> m_file;
    QVector<zstdseek::Frame> m_frames;
    quint64 m_size = 0;
    ZSTD_DCtx *m_ctx = ZSTD_createDCtx();
    QHash<int, QByteArray> m_cache;
    QVector<int> m_order;
    QMutex m_lock;
};

QString formatOf(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray start = f.read(512);
    const uchar *p = bytes(start);
    if (start.startsWith("vhdxfile"))
        return QStringLiteral("VHDX");
    if (start.startsWith("QFI\xfb"))
        return QStringLiteral("QCOW2");
    if (start.size() >= 0x48 && le32(p + 0x40) == 0xBEDA107F)
        return QStringLiteral("VDI");
    if ((start.size() >= 4 && le32(p) == 0x564D444B) || start.contains("# Disk DescriptorFile"))
        return QStringLiteral("VMDK");
    if (start.startsWith("conectix"))
        return QStringLiteral("VHD");
    if (f.size() >= 512 && f.seek(f.size() - 512) && f.read(8) == "conectix")
        return QStringLiteral("VHD");
    if (start.startsWith("\x28\xB5\x2F\xFD"))
        return QStringLiteral("ZSTD");
    return {};
}

std::shared_ptr<lost::Source> open(const QString &path, QString *problem, QString *warning)
{
    QString ignored;
    QString &why = problem ? *problem : ignored;
    const QString format = formatOf(path);
    auto file = lost::Source::fromImage(path, &why);
    if (!file || format.isEmpty()) {
        if (why.isEmpty())
            why = tr("It isn't a virtual machine's disk.");
        return {};
    }
    std::unique_ptr<lost::Layer> disk;
    if (format == QLatin1String("VDI"))
        disk = Vdi::open(file, &why);
    else if (format == QLatin1String("VHD"))
        disk = Vhd::open(file, &why);
    else if (format == QLatin1String("VHDX"))
        disk = Vhdx::open(file, &why, warning);
    else if (format == QLatin1String("QCOW2"))
        disk = Qcow2::open(file, &why, warning);
    else if (format == QLatin1String("ZSTD"))
        disk = ZstdBackup::open(file, &why);
    else
        disk = Vmdk::open(path, file, &why, warning);
    if (!disk) {
        if (why.isEmpty())
            why = tr("It's damaged.");
        return {};
    }
    return lost::Source::fromLayer(std::move(disk));
}

} // namespace vdisk
