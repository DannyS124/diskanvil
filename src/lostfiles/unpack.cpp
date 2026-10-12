// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "unpack.h"

#include <QMutex>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cstring>

namespace unpack {

namespace {

// A canonical Huffman code, from how long each symbol's codeword is: the codewords are handed
// out shortest first, and in symbol order within a length, and read most significant bit first.
// A code that doesn't fill its space is allowed; the codewords it doesn't use are errors.
class Huffman
{
public:
    static constexpr int kMaxSymbols = 512;

    bool build(const quint8 *lengths, int symbols, int longest)
    {
        if (symbols > kMaxSymbols)
            return false;
        std::array<quint16, 17> count{};
        for (int s = 0; s < symbols; ++s) {
            if (lengths[s] > longest)
                return false;
            ++count[lengths[s]];
        }
        count[0] = 0;
        // Too many short codewords can't be a code at all.
        int left = 1;
        for (int n = 1; n <= 16; ++n) {
            left = left * 2 - count[n];
            if (left < 0)
                return false;
        }
        quint32 code = 0;
        quint16 index = 0;
        m_longest = 0;
        for (int n = 1; n <= 16; ++n) {
            m_first[n] = code;
            m_count[n] = count[n];
            m_index[n] = index;
            index += count[n];
            code = (code + count[n]) << 1;
            if (count[n])
                m_longest = n;
        }
        std::array<quint16, 17> next = m_index;
        for (int s = 0; s < symbols; ++s) {
            if (lengths[s])
                m_sorted[next[lengths[s]]++] = quint16(s);
        }
        // The short codewords straight from a table of their first kFast bits.
        m_fast.fill(0);
        for (int n = 1; n <= std::min(m_longest, kFast); ++n) {
            for (quint32 i = 0; i < m_count[n]; ++i) {
                const quint32 start = (m_first[n] + i) << (kFast - n), span = 1u << (kFast - n);
                const quint16 entry = quint16(m_sorted[m_index[n] + i] << 5 | n);
                std::fill(m_fast.begin() + start, m_fast.begin() + start + span, entry);
            }
        }
        return true;
    }

    // The symbol whose codeword starts `top` (the next 16 bits, the first one highest), and its
    // codeword's length; -1 for none.
    int decode(quint32 top, int *length) const
    {
        const quint16 entry = m_fast[top >> (16 - kFast)];
        if (entry) {
            *length = entry & 31;
            return entry >> 5;
        }
        for (int n = kFast + 1; n <= m_longest; ++n) {
            const quint32 offset = (top >> (16 - n)) - m_first[n];
            if (offset < m_count[n]) {
                *length = n;
                return m_sorted[m_index[n] + offset];
            }
        }
        *length = 0;
        return -1;
    }

private:
    static constexpr int kFast = 10;
    std::array<quint16, 1 << kFast> m_fast{}; // symbol << 5 | length, 0 for a longer codeword
    std::array<quint32, 17> m_first{};        // each length's first codeword
    std::array<quint16, 17> m_count{};
    std::array<quint16, 17> m_index{};        // where each length's symbols start in m_sorted
    std::array<quint16, kMaxSymbols> m_sorted{};
    int m_longest = 0;
};

// Copies a match. It may overlap what it copies (a run of the same bytes is a match with offset 1).
inline void copyMatch(uchar *out, qsizetype at, qsizetype offset, qsizetype length)
{
    for (qsizetype i = 0; i < length; ++i)
        out[at + i] = out[at - offset + i];
}

// --- LZX, as Windows uses it for WOF and WIM: a 32 KB window, each chunk on its own, and the x86
// call translation always on, for a file of 12000000 bytes.

constexpr int kLzxChars = 256;
constexpr int kLzxSlots = 30; // offset slots for a 32 KB window
constexpr int kLzxMainSymbols = kLzxChars + kLzxSlots * 8;
constexpr int kLzxLengthSymbols = 249;
constexpr int kLzxPreSymbols = 20;
constexpr int kLzxAlignedSymbols = 8;
constexpr quint32 kLzxDefaultBlock = 32768;
constexpr qint32 kLzxTranslationSize = 12000000;

constexpr std::array<quint8, kLzxSlots> kLzxExtraBits = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

constexpr std::array<quint32, kLzxSlots> lzxSlotBases()
{
    std::array<quint32, kLzxSlots> base{};
    for (int s = 1; s < kLzxSlots; ++s)
        base[s] = base[s - 1] + (1u << kLzxExtraBits[s - 1]);
    return base;
}
constexpr std::array<quint32, kLzxSlots> kLzxSlotBase = lzxSlotBases();

// LZX's bits: 16-bit little-endian words, each read from its highest bit. Words are taken in only
// as they're needed, as the format's own decoder does: where an uncompressed block's header
// starts depends on it. Past the end, zeros.
struct LzxBits {
    const uchar *next;
    const uchar *end;
    quint32 buffer = 0; // the next bit is the highest
    int have = 0;

    void need(int n)
    {
        if (have >= n)
            return;
        quint32 word = 0;
        if (end - next >= 2) {
            word = quint32(next[0]) | quint32(next[1]) << 8;
            next += 2;
        }
        buffer |= word << (16 - have);
        have += 16;
    }
    quint32 pop(int n)
    {
        if (n == 0)
            return 0;
        const quint32 value = buffer >> (32 - n);
        buffer <<= n;
        have -= n;
        return value;
    }
    quint32 get(int n)
    {
        need(n);
        return pop(n);
    }
    int symbol(const Huffman &code, int longest)
    {
        need(longest);
        int length = 0;
        const int s = code.decode(buffer >> 16, &length);
        if (s >= 0)
            pop(length);
        return s;
    }
    // To the next 16-bit word: the rest of this one, or the whole next one when this one's done.
    void align()
    {
        need(1);
        buffer = 0;
        have = 0;
    }
};

// A run of codeword lengths, sent as changes from the last block's through a small code of its
// own (the pretree).
bool lzxLengths(LzxBits &bits, quint8 *lengths, int count, int room)
{
    std::array<quint8, kLzxPreSymbols> pre{};
    for (quint8 &l : pre)
        l = quint8(bits.get(4));
    Huffman code;
    if (!code.build(pre.data(), kLzxPreSymbols, 15))
        return false;
    for (int i = 0; i < count;) {
        int s = bits.symbol(code, 15);
        if (s < 0)
            return false;
        if (s < 17) {
            lengths[i] = quint8((lengths[i] + 17 - s) % 17);
            ++i;
            continue;
        }
        int run;
        quint8 value = 0;
        if (s == 17) {
            run = 4 + int(bits.get(4));
        } else if (s == 18) {
            run = 20 + int(bits.get(5));
        } else {
            run = 4 + int(bits.get(1));
            s = bits.symbol(code, 15);
            if (s < 0 || s > 17) // 17 is "the same", like 0
                return false;
            value = quint8((lengths[i] + 17 - s) % 17);
        }
        // A run may go past the end; what it writes there is never used.
        for (; run > 0 && i < room; --run)
            lengths[i++] = value;
    }
    return true;
}

// Undoes the x86 call translation: the compressor turned the offset after each 0xE8 byte from
// "from here" into "from the start", so calls to the same place look the same.
void lzxUntranslate(uchar *data, qsizetype size)
{
    if (size <= 10)
        return;
    for (qsizetype at = 0; at < size - 10;) {
        if (data[at] != 0xE8) {
            ++at;
            continue;
        }
        const qint32 absolute = qFromLittleEndian<qint32>(data + at + 1);
        const qint32 here = qint32(at);
        if (absolute >= 0 && absolute < kLzxTranslationSize)
            qToLittleEndian<qint32>(absolute - here, data + at + 1);
        else if (absolute < 0 && absolute >= -here)
            qToLittleEndian<qint32>(absolute + kLzxTranslationSize, data + at + 1);
        at += 5;
    }
}

// --- WOF and NTFS compression's layers.

// Bytes at a place in what's kept: pieces of a source, or memory.
class Stored
{
public:
    Stored(std::shared_ptr<lost::Source> source, QVector<lost::Extent> extents, QByteArray inRecord)
        : m_source(std::move(source))
        , m_extents(std::move(extents))
        , m_inRecord(std::move(inRecord))
    {
        quint64 at = 0;
        for (const lost::Extent &e : std::as_const(m_extents)) {
            m_starts.push_back(at);
            at += e.length;
        }
        m_size = m_extents.isEmpty() ? quint64(m_inRecord.size()) : at;
    }

    quint64 size() const { return m_size; }

    // `len` bytes at `at`; false when any of them couldn't be read.
    bool read(quint64 at, char *buf, quint64 len) const
    {
        if (at > m_size || len > m_size - at)
            return false;
        if (m_extents.isEmpty()) {
            std::memcpy(buf, m_inRecord.constData() + at, len);
            return true;
        }
        qsizetype i = std::upper_bound(m_starts.cbegin(), m_starts.cend(), at) - m_starts.cbegin() - 1;
        while (len > 0 && i < m_extents.size()) {
            const lost::Extent &e = m_extents[i];
            const quint64 inside = at - m_starts[i];
            const quint64 n = std::min(len, e.length - inside);
            if (e.zeros()) {
                std::memset(buf, 0, n);
            } else if (m_source->read(e.start + inside, buf, n) != n || m_source->unreadable(e.start + inside, n)) {
                return false;
            }
            buf += n;
            at += n;
            len -= n;
            ++i;
        }
        return len == 0;
    }

    // The parts of [from, from + len) that are kept (not holes), and how many bytes they are.
    quint64 keptIn(quint64 from, quint64 len, bool *hole) const
    {
        *hole = false;
        quint64 kept = 0, covered = 0;
        if (from >= m_size)
            return 0;
        if (m_extents.isEmpty())
            return std::min(len, m_size - from);
        qsizetype i = std::upper_bound(m_starts.cbegin(), m_starts.cend(), from) - m_starts.cbegin() - 1;
        for (quint64 at = from; covered < len && i >= 0 && i < m_extents.size(); ++i) {
            const lost::Extent &e = m_extents[i];
            const quint64 inside = at - m_starts[i];
            const quint64 n = std::min(len - covered, e.length - inside);
            if (e.zeros())
                *hole = true;
            else
                kept += n;
            covered += n;
            at += n;
        }
        return kept;
    }

private:
    std::shared_ptr<lost::Source> m_source;
    QVector<lost::Extent> m_extents;
    QVector<quint64> m_starts; // where each extent starts in what's kept
    QByteArray m_inRecord;
    quint64 m_size = 0;
};

// The last few pieces unpacked, kept: reads come in small bits, and in order.
class Recent
{
public:
    const QByteArray *find(quint64 index) const
    {
        for (const Piece &p : m_pieces) {
            if (p.valid && p.index == index)
                return &p.bytes;
        }
        return nullptr;
    }
    const QByteArray *keep(quint64 index, QByteArray bytes)
    {
        Piece &p = m_pieces[m_next++ % m_pieces.size()];
        p = {index, std::move(bytes), true};
        return &p.bytes;
    }

private:
    struct Piece {
        quint64 index = 0;
        QByteArray bytes;
        bool valid = false;
    };
    std::array<Piece, 4> m_pieces;
    size_t m_next = 0;
};

class Packed : public lost::Layer
{
public:
    Packed(Stored stored, quint64 size, quint64 piece)
        : m_stored(std::move(stored))
        , m_size(size)
        , m_piece(piece)
    {
    }
    quint64 size() const override { return m_size; }

    bool read(quint64 offset, char *buf, quint64 len) override
    {
        if (offset > m_size || len > m_size - offset)
            return false;
        QMutexLocker locker(&m_lock);
        while (len > 0) {
            const quint64 index = offset / m_piece, inside = offset % m_piece;
            const QByteArray *bytes = m_recent.find(index);
            if (!bytes) {
                QByteArray unpacked;
                if (!piece(index, &unpacked))
                    return false;
                bytes = m_recent.keep(index, std::move(unpacked));
            }
            const quint64 n = std::min(len, quint64(bytes->size()) - std::min(inside, quint64(bytes->size())));
            if (n == 0)
                return false;
            std::memcpy(buf, bytes->constData() + inside, n);
            buf += n;
            offset += n;
            len -= n;
        }
        return true;
    }

protected:
    // Piece `index` of the file, unpacked: m_piece bytes, fewer at the end.
    virtual bool piece(quint64 index, QByteArray *out) = 0;
    quint64 pieceSize(quint64 index) const { return std::min(m_piece, m_size - index * m_piece); }

    Stored m_stored;
    quint64 m_size;
    quint64 m_piece;

private:
    QMutex m_lock;
    Recent m_recent;
};

class Wof : public Packed
{
public:
    Wof(Stored stored, quint64 size, Format format)
        : Packed(std::move(stored), size, chunkSize(format))
        , m_format(format)
    {
    }

protected:
    bool piece(quint64 index, QByteArray *out) override
    {
        if (!m_tableRead) {
            m_tableRead = true;
            m_table = readTable();
        }
        if (index + 1 >= quint64(m_table.size()))
            return false;
        const quint64 from = m_table[qsizetype(index)], to = m_table[qsizetype(index + 1)];
        const quint64 size = pieceSize(index);
        if (to - from > size || to == from)
            return false;
        QByteArray packed(qsizetype(to - from), Qt::Uninitialized);
        if (!m_stored.read(from, packed.data(), quint64(packed.size())))
            return false;
        if (to - from == size) { // didn't get smaller: kept as it is
            *out = std::move(packed);
            return true;
        }
        out->resize(qsizetype(size));
        const auto *in = reinterpret_cast<const uchar *>(packed.constData());
        auto *o = reinterpret_cast<uchar *>(out->data());
        return m_format == Format::Lzx ? lzx(in, packed.size(), o, out->size()) : xpress(in, packed.size(), o, out->size());
    }

private:
    // Where each chunk starts in the stream, and where the last one ends. The table has one entry
    // for every chunk but the first, counted from the table's end; 8 bytes each for a file over
    // 4 GB.
    QVector<quint64> readTable() const
    {
        const quint64 chunks = (m_size + m_piece - 1) / m_piece;
        const quint64 entry = m_size > 0xFFFFFFFFull ? 8 : 4;
        const quint64 tableSize = chunks > 0 ? (chunks - 1) * entry : 0;
        const quint64 stored = m_stored.size();
        if (tableSize > stored)
            return {};
        QByteArray table(qsizetype(tableSize), Qt::Uninitialized);
        if (!m_stored.read(0, table.data(), tableSize))
            return {};
        QVector<quint64> starts;
        starts.reserve(qsizetype(chunks + 1));
        starts.push_back(tableSize);
        const auto *p = reinterpret_cast<const uchar *>(table.constData());
        for (quint64 i = 0; i + 1 < chunks; ++i) {
            const quint64 at = tableSize + (entry == 8 ? qFromLittleEndian<quint64>(p + i * 8) : qFromLittleEndian<quint32>(p + i * 4));
            if (at < starts.last() || at > stored)
                return {};
            starts.push_back(at);
        }
        starts.push_back(stored);
        return starts;
    }

    Format m_format;
    bool m_tableRead = false;
    QVector<quint64> m_table;
};

class Lznt1 : public Packed
{
public:
    Lznt1(Stored stored, quint64 size, quint32 unit)
        : Packed(std::move(stored), size, unit)
    {
    }

protected:
    bool piece(quint64 index, QByteArray *out) override
    {
        const quint64 from = index * m_piece;
        bool hole = false;
        const quint64 kept = m_stored.keptIn(from, m_piece, &hole);
        QByteArray unit(qsizetype(m_piece), '\0');
        if (kept > 0 && !hole) {
            // No hole: kept as it is.
            if (!m_stored.read(from, unit.data(), std::min<quint64>(m_piece, m_stored.size() - from)))
                return false;
        } else if (kept > 0) {
            QByteArray packed(qsizetype(kept), Qt::Uninitialized);
            if (!m_stored.read(from, packed.data(), kept)
                || !lznt1(reinterpret_cast<const uchar *>(packed.constData()), packed.size(), reinterpret_cast<uchar *>(unit.data()), unit.size()))
                return false;
        }
        unit.truncate(qsizetype(pieceSize(index)));
        *out = std::move(unit);
        return true;
    }
};

} // namespace

Format wofFormat(quint32 algorithm)
{
    switch (algorithm) {
    case 0:
        return Format::Xpress4k;
    case 1:
        return Format::Lzx;
    case 2:
        return Format::Xpress8k;
    case 3:
        return Format::Xpress16k;
    }
    return Format::None;
}

quint32 chunkSize(Format format)
{
    switch (format) {
    case Format::Xpress4k:
        return 4096;
    case Format::Lzx:
        return 32768;
    case Format::Xpress8k:
        return 8192;
    case Format::Xpress16k:
        return 16384;
    case Format::None:
    case Format::Lznt1:
        break;
    }
    return 0;
}

QString name(Format format)
{
    switch (format) {
    case Format::Lznt1:
        return QStringLiteral("LZNT1");
    case Format::Xpress4k:
        return QStringLiteral("XPRESS 4K");
    case Format::Lzx:
        return QStringLiteral("LZX");
    case Format::Xpress8k:
        return QStringLiteral("XPRESS 8K");
    case Format::Xpress16k:
        return QStringLiteral("XPRESS 16K");
    case Format::None:
        break;
    }
    return QString();
}

std::unique_ptr<lost::Layer> layer(std::shared_ptr<lost::Source> source, QVector<lost::Extent> stored, QByteArray inRecord, const Packing &packing,
                                   quint64 size)
{
    Stored kept(std::move(source), std::move(stored), std::move(inRecord));
    if (packing.format == Format::Lznt1) {
        if (packing.unit < 4096 || packing.unit > 4 * 1024 * 1024)
            return nullptr;
        return std::make_unique<Lznt1>(std::move(kept), size, packing.unit);
    }
    if (chunkSize(packing.format) == 0)
        return nullptr;
    return std::make_unique<Wof>(std::move(kept), size, packing.format);
}

bool xpress(const uchar *in, qsizetype inSize, uchar *out, qsizetype outSize)
{
    // [MS-XCA] 2.2: 512 codeword lengths, 4 bits each, then the bits. Bits come 16 at a time
    // (little-endian), the first from the highest; a match's longer lengths are bytes of their own,
    // taken from where the next 16 bits would be.
    if (inSize < 256)
        return false;
    std::array<quint8, 512> lengths;
    for (int i = 0; i < 256; ++i) {
        lengths[2 * i] = in[i] & 15;
        lengths[2 * i + 1] = in[i] >> 4;
    }
    Huffman code;
    if (!code.build(lengths.data(), 512, 15))
        return false;
    qsizetype at = 256;
    auto word = [&]() -> quint32 {
        quint32 w = 0;
        if (at + 2 <= inSize)
            w = quint32(in[at]) | quint32(in[at + 1]) << 8;
        at += 2;
        return w;
    };
    quint32 bits = word() << 16;
    bits |= word();
    int extra = 16; // bits there are past the 16 the next codeword may need
    auto used = [&](int n) {
        bits = n >= 32 ? 0 : bits << n;
        extra -= n;
        if (extra < 0) {
            bits |= word() << -extra;
            extra += 16;
        }
    };
    qsizetype o = 0;
    while (o < outSize) {
        int length = 0;
        const int symbol = code.decode(bits >> 16, &length);
        if (symbol < 0)
            return false;
        used(length);
        if (symbol < 256) {
            out[o++] = uchar(symbol);
            continue;
        }
        qsizetype matchLength = (symbol - 256) & 15;
        const int offsetBits = (symbol - 256) >> 4;
        if (matchLength == 15) {
            if (at >= inSize)
                return false;
            matchLength = in[at++];
            if (matchLength == 255) {
                if (at + 2 > inSize)
                    return false;
                matchLength = qFromLittleEndian<quint16>(in + at);
                at += 2;
                if (matchLength == 0) {
                    if (at + 4 > inSize)
                        return false;
                    matchLength = qsizetype(qFromLittleEndian<quint32>(in + at));
                    at += 4;
                }
                if (matchLength < 15)
                    return false;
                matchLength -= 15;
            }
            matchLength += 15;
        }
        matchLength += 3;
        const qsizetype offset = qsizetype(offsetBits ? bits >> (32 - offsetBits) : 0) + (qsizetype(1) << offsetBits);
        used(offsetBits);
        if (offset > o || matchLength > outSize - o)
            return false;
        copyMatch(out, o, offset, matchLength);
        o += matchLength;
    }
    return true;
}

bool lzx(const uchar *in, qsizetype inSize, uchar *out, qsizetype outSize)
{
    LzxBits bits{in, in + inSize};
    // The codeword lengths carry on from block to block, starting at nothing, and so do the
    // three most recent match offsets, starting at 1.
    std::array<quint8, kLzxMainSymbols + 64> mainLengths{};
    std::array<quint8, kLzxLengthSymbols + 64> lengthLengths{};
    std::array<quint32, 3> recent = {1, 1, 1};
    qsizetype o = 0;
    while (o < outSize) {
        bits.need(4);
        const int type = int(bits.pop(3));
        quint32 blockSize = kLzxDefaultBlock;
        if (!bits.pop(1)) {
            blockSize = bits.get(8) << 8;
            blockSize |= bits.get(8);
        }
        if (blockSize == 0)
            return false;
        const qsizetype blockEnd = o + std::min<qsizetype>(blockSize, outSize - o);
        if (type == 3) {
            // Uncompressed: the three recent offsets, the bytes, and a byte to make it even.
            bits.align();
            if (bits.end - bits.next < 12)
                return false;
            for (quint32 &r : recent) {
                r = qFromLittleEndian<quint32>(bits.next);
                bits.next += 4;
                if (r == 0)
                    return false;
            }
            const qsizetype n = blockEnd - o;
            if (bits.end - bits.next < n)
                return false;
            std::memcpy(out + o, bits.next, size_t(n));
            bits.next += n;
            o += n;
            if ((blockSize & 1) && bits.next < bits.end)
                ++bits.next;
            continue;
        }
        if (type != 1 && type != 2)
            return false;
        Huffman aligned;
        if (type == 2) {
            std::array<quint8, kLzxAlignedSymbols> a;
            for (quint8 &l : a)
                l = quint8(bits.get(3));
            if (!aligned.build(a.data(), kLzxAlignedSymbols, 7))
                return false;
        }
        Huffman mainCode, lengthCode;
        if (!lzxLengths(bits, mainLengths.data(), kLzxChars, int(mainLengths.size()))
            || !lzxLengths(bits, mainLengths.data() + kLzxChars, kLzxMainSymbols - kLzxChars, int(mainLengths.size()) - kLzxChars)
            || !lzxLengths(bits, lengthLengths.data(), kLzxLengthSymbols, int(lengthLengths.size()))
            || !mainCode.build(mainLengths.data(), kLzxMainSymbols, 16) || !lengthCode.build(lengthLengths.data(), kLzxLengthSymbols, 16))
            return false;
        while (o < blockEnd) {
            int symbol = bits.symbol(mainCode, 16);
            if (symbol < 0)
                return false;
            if (symbol < kLzxChars) {
                out[o++] = uchar(symbol);
                continue;
            }
            symbol -= kLzxChars;
            qsizetype length = symbol & 7;
            const int slot = symbol >> 3;
            if (length == 7) {
                const int more = bits.symbol(lengthCode, 16);
                if (more < 0)
                    return false;
                length += more;
            }
            length += 2;
            quint32 offset;
            if (slot < 3) {
                // One of the recent ones: it swaps places with the most recent.
                offset = recent[slot];
                recent[slot] = recent[0];
                recent[0] = offset;
            } else {
                const int extra = kLzxExtraBits[slot];
                quint32 footer;
                if (type == 2 && extra >= 3) {
                    footer = bits.get(extra - 3) << 3;
                    const int low = bits.symbol(aligned, 7);
                    if (low < 0)
                        return false;
                    footer |= quint32(low);
                } else {
                    footer = bits.get(extra);
                }
                offset = kLzxSlotBase[slot] + footer - 2;
                recent[2] = recent[1];
                recent[1] = recent[0];
                recent[0] = offset;
            }
            if (offset == 0 || qsizetype(offset) > o || length > blockEnd - o)
                return false;
            copyMatch(out, o, qsizetype(offset), length);
            o += length;
        }
    }
    lzxUntranslate(out, outSize);
    return true;
}

bool lznt1(const uchar *in, qsizetype inSize, uchar *out, qsizetype outSize)
{
    // [MS-XCA] 2.5: blocks of up to 4096 bytes, each with a 16-bit header: its size less 3, and
    // whether it's packed. Packed ones are groups of a flag byte and eight items, each a byte as it
    // is or a 16-bit back-reference, whose split between offset and length depends on how far
    // into the block it is. A block that unpacks short is followed by zeros.
    std::memset(out, 0, size_t(outSize));
    qsizetype at = 0, o = 0;
    while (at + 2 <= inSize && o < outSize) {
        const quint16 header = qFromLittleEndian<quint16>(in + at);
        if (header == 0)
            break;
        const qsizetype blockEnd = at + (header & 0x0FFF) + 3;
        if (blockEnd > inSize)
            return false;
        at += 2;
        const qsizetype outStart = o, outEnd = std::min<qsizetype>(o + 4096, outSize);
        if (!(header & 0x8000)) {
            const qsizetype n = std::min(blockEnd - at, outEnd - o);
            std::memcpy(out + o, in + at, size_t(n));
            o = outEnd;
            at = blockEnd;
            continue;
        }
        while (at < blockEnd) {
            const quint8 flags = in[at++];
            for (int item = 0; item < 8 && at < blockEnd; ++item) {
                if (!(flags & (1 << item))) {
                    if (o >= outEnd)
                        return false;
                    out[o++] = in[at++];
                    continue;
                }
                if (at + 2 > blockEnd || o == outStart)
                    return false;
                const quint16 token = qFromLittleEndian<quint16>(in + at);
                at += 2;
                int lengthBits = 12;
                for (qsizetype n = o - outStart - 1; n >= 0x10; n >>= 1)
                    --lengthBits;
                const qsizetype offset = (token >> lengthBits) + 1;
                const qsizetype length = (token & ((1 << lengthBits) - 1)) + 3;
                if (offset > o - outStart || length > outEnd - o)
                    return false;
                copyMatch(out, o, offset, length);
                o += length;
            }
        }
        o = outEnd; // a short block: zeros for the rest of its 4096 bytes
    }
    return true;
}

} // namespace unpack
