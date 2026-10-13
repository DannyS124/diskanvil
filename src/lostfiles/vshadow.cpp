// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vshadow.h"

#include "lostraw.h"

#include <QCoreApplication>
#include <QHash>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <cstring>

namespace vss {

namespace {

using lost::raw::bytes;
using lost::raw::le16;
using lost::raw::le32;
using lost::raw::le64;

constexpr quint64 kBlock = 0x4000;      // everything comes in 16 KiB blocks
constexpr quint64 kVolumeHeader = 0x1e00; // VSS's header sits in NTFS's own first 8 KiB
constexpr int kHeader = 128;            // every block starts with this much about itself
constexpr int kMostBlocks = 1 << 20;    // in one list: 16 GiB of them, more than any store has
// "VSS" itself, a GUID: 3808876b-c176-4e48-b7ae-04046e6cc752.
constexpr uchar kIdentifier[16] = {0x6b, 0x87, 0x08, 0x38, 0x76, 0xc1, 0x48, 0x4e, 0xb7, 0xae, 0x04, 0x04, 0x6e, 0x6c, 0xc7, 0x52};

enum Record : quint32 { kVolume = 1, kCatalog = 2, kBlockList = 3, kStoreHeader = 4, kBitmap = 6 };
enum Flag : quint32 {
    kForwarder = 0x1, // the block is another block of the next newer copy (or of the volume now)
    kOverlay = 0x2,   // some of the block's 512-byte sectors, written to this copy after it was taken
    kNotUsed = 0x4,
};

QString tr(const char *text)
{
    return QCoreApplication::translate("vss", text);
}

QString guid(const uchar *p)
{
    return QUuid(le32(p), le16(p + 4), le16(p + 6), p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]).toString(QUuid::WithBraces).toUpper();
}

qint64 fileTime(quint64 t)
{
    return t == 0 ? 0 : qint64(t / 10000) - 11644473600000LL;
}

// Where a block of the copy really is.
struct Descriptor {
    quint64 original = 0; // the block of the volume it stands for
    quint64 relative = 0; // for a forwarder: the block of the next copy it is
    quint64 offset = 0;   // where Windows saved the block's old contents
    quint32 flags = 0;
    quint32 sectors = 0; // for an overlay: which of the block's 32 sectors it has
    int overlay = -1;
};

struct Store {
    QString key; // the GUID that ties the catalog's two entries for it together
    Copy copy;
    quint64 createdRaw = 0;
    quint64 listAt = 0, headerAt = 0, bitmapAt = 0, previousBitmapAt = 0;
    bool described = false, located = false;

    QVector<Descriptor> descriptors;
    QHash<quint64, int> forward; // block number -> descriptor
    QHash<quint64, int> reverse; // forwarders, by the block number they point at
    QByteArray unused, previousUnused; // bitmaps, one bit a block: not in use when the copy was taken
};

// A block VSS wrote for itself, if it's the kind expected.
bool header(const QByteArray &block, quint32 record)
{
    const uchar *p = bytes(block);
    // Version 1, and 2 for the volume's own header on newer Windows (Windows 11 24H2 on).
    const quint32 version = le32(p + 16);
    return block.size() >= kHeader && memcmp(p, kIdentifier, 16) == 0 && (version == 1 || version == 2) && le32(p + 20) == record;
}

class Reader
{
public:
    Reader(lost::Source &source, const lost::Extent &where)
        : m_source(source)
        , m_where(where)
    {
    }

    QByteArray read(quint64 at, quint64 len) const
    {
        if (at >= m_where.length || len > m_where.length - at)
            return {};
        return lost::raw::read(m_source, m_where.start + at, len);
    }

    // The catalog: what copies there are, and where each one's parts are. Oldest first.
    QVector<Store> stores(QString *problem) const
    {
        const QByteArray volume = read(kVolumeHeader, 512);
        if (!header(volume, kVolume))
            return {};
        quint64 at = le64(bytes(volume) + 48);
        QVector<Store> out;
        QHash<QString, int> byKey;
        QSet<quint64> seen;
        while (at != 0) {
            const QByteArray block = read(at, kBlock);
            if (seen.contains(at) || !header(block, kCatalog)) {
                *problem = tr("Windows' list of its earlier versions of the drive is damaged.");
                break;
            }
            seen.insert(at);
            const uchar *p = bytes(block);
            for (int e = kHeader; e + 128 <= int(kBlock); e += 128) {
                const quint64 type = le64(p + e);
                if (type != 2 && type != 3)
                    continue; // 0 and 1: unused
                const QString key = guid(p + e + 16);
                if (!byKey.contains(key)) {
                    byKey.insert(key, int(out.size()));
                    out.push_back({});
                    out.back().key = key;
                }
                Store &s = out[byKey[key]];
                if (type == 2) {
                    s.copy.size = le64(p + e + 8);
                    s.createdRaw = le64(p + e + 48);
                    s.copy.created = fileTime(s.createdRaw);
                    s.described = true;
                } else {
                    s.listAt = le64(p + e + 8);
                    s.headerAt = le64(p + e + 32);
                    s.bitmapAt = le64(p + e + 48);
                    s.previousBitmapAt = le64(p + e + 72);
                    s.located = true;
                }
            }
            at = le64(p + 40);
        }
        // Only copies kept on the volume itself (all of them since Vista) can be read.
        out.erase(std::remove_if(out.begin(), out.end(), [](const Store &s) { return !s.described || !s.located; }), out.end());
        std::stable_sort(out.begin(), out.end(), [](const Store &a, const Store &b) { return a.createdRaw < b.createdRaw; });
        for (Store &s : out) {
            if (s.copy.size == 0 || s.copy.size > m_where.length)
                s.copy.size = m_where.length; // a damaged size: the volume's own
            // The copy's own ID is in its store's header.
            const QByteArray info = read(s.headerAt, kBlock);
            s.copy.id = header(info, kStoreHeader) ? guid(bytes(info) + kHeader + 16) : s.key;
        }
        return out;
    }

    // A chain of blocks of one kind, each with a header saying where the next one is.
    template<typename Each>
    bool chain(quint64 at, quint32 record, Each each) const
    {
        QSet<quint64> seen;
        while (at != 0) {
            const QByteArray block = read(at, kBlock);
            if (seen.contains(at) || seen.size() >= kMostBlocks || !header(block, record))
                return false;
            seen.insert(at);
            each(block);
            at = le64(bytes(block) + 40);
        }
        return true;
    }

    // A store's list of saved blocks, added up the way Windows means it.
    bool load(Store &s) const
    {
        const bool listed = chain(s.listAt, kBlockList, [&s](const QByteArray &block) {
            const uchar *p = bytes(block);
            for (int d = kHeader; d + 32 <= int(kBlock); d += 32) {
                static const uchar empty[32] = {};
                if (memcmp(p + d, empty, 32) == 0)
                    continue;
                add(s, {le64(p + d), le64(p + d + 8), le64(p + d + 16), le32(p + d + 24), le32(p + d + 28), -1});
            }
        });
        auto bitmap = [this](quint64 at, QByteArray *bits) {
            return chain(at, kBitmap, [bits](const QByteArray &block) { *bits += block.mid(kHeader); });
        };
        return listed && bitmap(s.bitmapAt, &s.unused) && (s.previousBitmapAt == 0 || bitmap(s.previousBitmapAt, &s.previousUnused));
    }

    static void add(Store &s, Descriptor d)
    {
        if ((d.flags & kNotUsed) || d.original % kBlock != 0)
            return;
        quint64 original = d.original;
        if (!(d.flags & kOverlay)) {
            // A forwarder pointing at this block now points where this one does.
            const auto r = s.reverse.constFind(d.original / kBlock);
            if (r != s.reverse.cend()) {
                original = s.descriptors[*r].original;
                s.reverse.erase(r);
            }
        }
        if ((d.flags & kForwarder) && original == d.relative)
            return; // pointing at itself
        d.original = original;
        const int i = int(s.descriptors.size());
        s.descriptors.push_back(d);
        const auto existing = s.forward.find(original / kBlock);
        if (existing == s.forward.end()) {
            s.forward.insert(original / kBlock, i);
        } else if (d.flags & kOverlay) {
            // More sectors of an overlay the block has already, or its first.
            Descriptor &e = s.descriptors[*existing];
            const int overlay = (e.flags & kOverlay) ? *existing : e.overlay;
            if (overlay >= 0)
                s.descriptors[overlay].sectors |= d.sectors;
            else
                e.overlay = i;
            return;
        } else {
            // It takes the block over, keeping the overlay that was there.
            Descriptor &e = s.descriptors[*existing];
            if (e.flags & kOverlay) {
                s.descriptors[i].overlay = *existing;
            } else {
                s.descriptors[i].overlay = e.overlay;
                e.overlay = -1;
            }
            *existing = i;
        }
        if (d.flags & kForwarder)
            s.reverse.insert(d.relative / kBlock, i);
    }

private:
    lost::Source &m_source;
    lost::Extent m_where;
};

// One copy, read as a volume: the stores from its own to the newest, over the volume as it is now.
class CopyLayer : public lost::Layer
{
public:
    CopyLayer(std::shared_ptr<lost::Source> source, const lost::Extent &where, QVector<Store> stores, quint64 size)
        : m_source(std::move(source))
        , m_where(where)
        , m_stores(std::move(stores))
        , m_size(size)
    {
    }

    quint64 size() const override { return m_size; }

    bool read(quint64 offset, char *buf, quint64 len) override
    {
        readFrom(0, offset, buf, len);
        return true;
    }

private:
    void readFrom(int s, quint64 at, char *buf, quint64 len) const
    {
        while (len > 0) {
            const quint64 n = readPart(s, at, buf, len);
            at += n;
            buf += n;
            len -= n;
        }
    }

    // As much from `at` as comes from one place (at most to the end of its block).
    quint64 readPart(int s, quint64 at, char *buf, quint64 len) const
    {
        const Store &store = m_stores[s];
        const bool last = s + 1 == m_stores.size();
        const quint64 inBlock = at % kBlock;
        quint64 size = kBlock - inBlock;
        const Descriptor *d = nullptr;
        quint64 from = 0;
        if (const auto f = store.forward.constFind(at / kBlock); f != store.forward.cend()) {
            d = &store.descriptors[*f];
            from = (d->flags & kForwarder) ? d->relative : d->offset;
            const Descriptor *overlay = (d->flags & kOverlay) ? d : d->overlay >= 0 ? &store.descriptors[d->overlay] : nullptr;
            if (overlay && s != 0) {
                // What was written to a copy after it was taken is that copy's alone.
                if (d == overlay)
                    d = nullptr;
            } else if (overlay) {
                const int sector = int(inBlock / 512);
                const bool over = (overlay->sectors >> sector) & 1;
                int end = sector;
                while (end < 32 && bool((overlay->sectors >> end) & 1) == over)
                    ++end;
                size = quint64(end) * 512 - inBlock;
                if (over) {
                    d = overlay;
                    from = overlay->offset;
                } else if (d == overlay) {
                    d = nullptr;
                }
            }
            from += inBlock;
        }
        const quint64 n = std::min(len, size);
        if (d) {
            if ((d->flags & kForwarder) && !last)
                readFrom(s + 1, from, buf, n);
            else
                volume(from, buf, n);
        } else if (!last) {
            readFrom(s + 1, at, buf, n);
        } else if (s == 0 && unused(store, at)) {
            memset(buf, 0, n); // not in use when the newest copy was taken: Windows leaves it undefined
        } else {
            volume(at, buf, n);
        }
        return n;
    }

    static bool bit(const QByteArray &bits, quint64 block)
    {
        return block / 8 < quint64(bits.size()) && (uchar(bits[qsizetype(block / 8)]) >> (block % 8)) & 1;
    }

    static bool unused(const Store &s, quint64 at)
    {
        const quint64 block = at / kBlock;
        return !s.reverse.contains(block) && bit(s.unused, block) && (s.previousBitmapAt == 0 || bit(s.previousUnused, block));
    }

    // The volume as it is now; zeros past its end.
    void volume(quint64 at, char *buf, quint64 len) const
    {
        const quint64 n = at < m_where.length ? std::min(len, m_where.length - at) : 0;
        if (n > 0)
            m_source->read(m_where.start + at, buf, n);
        memset(buf + n, 0, len - n);
    }

    std::shared_ptr<lost::Source> m_source;
    lost::Extent m_where;
    QVector<Store> m_stores;
    quint64 m_size;
};

} // namespace

QVector<Copy> copies(lost::Source &source, const lost::Extent &where, QString *problem)
{
    QString ignored;
    QVector<Copy> out;
    for (const Store &s : Reader(source, where).stores(problem ? problem : &ignored))
        out << s.copy;
    return out;
}

std::shared_ptr<lost::Source> open(const std::shared_ptr<lost::Source> &source, const lost::Extent &where, int index, QString *problem)
{
    QString ignored;
    QString &why = problem ? *problem : ignored;
    const Reader reader(*source, where);
    QVector<Store> stores = reader.stores(&why);
    if (index < 0 || index >= stores.size()) {
        if (why.isEmpty())
            why = tr("That earlier version of the drive isn't there anymore.");
        return {};
    }
    // Reading a copy goes through every newer one.
    stores.remove(0, index);
    for (Store &s : stores) {
        if (!reader.load(s)) {
            why = tr("Windows' record of what changed since %1 is damaged.").arg(s.copy.id);
            return {};
        }
    }
    const quint64 size = stores.first().copy.size;
    return lost::Source::fromLayer(std::make_unique<CopyLayer>(source, where, std::move(stores), size));
}

} // namespace vss
