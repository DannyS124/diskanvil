// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filetypes.h"

#include <QCoreApplication>
#include <QHash>

#include <algorithm>
#include <array>
#include <cstring>

namespace {

constexpr quint64 kMiB = 1024 * 1024;
constexpr quint64 kGiB = 1024 * kMiB;

QString tr(const char *text)
{
    return QCoreApplication::translate("filetypes", text);
}

// Reads a candidate file through a window, so the walks below can ask for a few bytes at a
// time without going to the drive for each one.
class Stream
{
public:
    Stream(const filetypes::Reader &read, quint64 limit)
        : m_read(read)
        , m_limit(limit)
    {
    }

    // The bytes [pos, pos + n), or nullptr past the end of the file or the limit. The pointer
    // is only good until the next read: copy what's needed before asking for more.
    const uchar *peek(quint64 pos, int n)
    {
        if (n <= 0 || pos + quint64(n) > m_limit || pos + quint64(n) < pos)
            return nullptr;
        if (pos < m_start || pos + quint64(n) > m_start + quint64(m_have)) {
            const qint64 want = std::max<qint64>(kWindow, n);
            if (m_buffer.size() < want)
                m_buffer.resize(want);
            m_start = pos;
            m_have = std::max<qint64>(0, m_read(pos, m_buffer.data(), want));
            if (m_have > 0)
                m_reached = std::max(m_reached, std::min(m_limit, m_start + quint64(m_have)));
            if (m_have < n)
                return nullptr;
        }
        return reinterpret_cast<const uchar *>(m_buffer.constData()) + (pos - m_start);
    }
    bool u8(quint64 pos, quint32 &v)
    {
        const uchar *p = peek(pos, 1);
        return p && ((v = p[0]), true);
    }
    bool be16(quint64 pos, quint32 &v)
    {
        const uchar *p = peek(pos, 2);
        return p && ((v = quint32(p[0]) << 8 | p[1]), true);
    }
    bool be32(quint64 pos, quint32 &v)
    {
        const uchar *p = peek(pos, 4);
        return p && ((v = quint32(p[0]) << 24 | quint32(p[1]) << 16 | quint32(p[2]) << 8 | p[3]), true);
    }
    bool be64(quint64 pos, quint64 &v)
    {
        quint32 hi, lo;
        return be32(pos, hi) && be32(pos + 4, lo) && ((v = quint64(hi) << 32 | lo), true);
    }
    bool le16(quint64 pos, quint32 &v)
    {
        const uchar *p = peek(pos, 2);
        return p && ((v = quint32(p[1]) << 8 | p[0]), true);
    }
    bool le32(quint64 pos, quint32 &v)
    {
        const uchar *p = peek(pos, 4);
        return p && ((v = quint32(p[3]) << 24 | quint32(p[2]) << 16 | quint32(p[1]) << 8 | p[0]), true);
    }
    bool le64(quint64 pos, quint64 &v)
    {
        quint32 lo, hi;
        return le32(pos, lo) && le32(pos + 4, hi) && ((v = quint64(hi) << 32 | lo), true);
    }
    bool is(quint64 pos, const char *text)
    {
        const int n = int(strlen(text));
        const uchar *p = peek(pos, n);
        return p && memcmp(p, text, size_t(n)) == 0;
    }
    // Where `needle` next starts at or after `from`, or ~0 if not before the limit.
    quint64 find(quint64 from, const QByteArray &needle)
    {
        const int overlap = int(needle.size()) - 1;
        for (quint64 pos = from; pos < m_limit;) {
            qint64 n = qint64(std::min<quint64>(kWindow, m_limit - pos));
            if (n < needle.size())
                return ~quint64(0);
            const bool whole = peek(pos, int(n)) != nullptr;
            // Near the end of the data there's less than a window left: that much is looked
            // through (a failed peek has just read what there is from pos on).
            if (!whole) {
                if (m_start != pos || m_have < needle.size())
                    return ~quint64(0);
                n = m_have;
            }
            const QByteArray chunk = QByteArray::fromRawData(m_buffer.constData() + (pos - m_start), n);
            const qsizetype at = chunk.indexOf(needle);
            if (at >= 0)
                return pos + quint64(at);
            if (!whole)
                return ~quint64(0);
            pos += quint64(n - overlap);
        }
        return ~quint64(0);
    }
    // CRC-32 over [pos, pos + len), read through the window.
    bool crc32(quint64 pos, quint64 len, quint32 &crc)
    {
        crc = 0;
        while (len > 0) {
            const int n = int(std::min<quint64>(len, kWindow));
            const uchar *p = peek(pos, n);
            if (!p)
                return false;
            crc = filetypes::crc32(p, n, crc);
            pos += quint64(n);
            len -= quint64(n);
        }
        return true;
    }
    quint64 limit() const { return m_limit; }
    // How far there was anything to read: where a file cut off by the end of the drive ends.
    quint64 reached() const { return m_reached; }

private:
    static constexpr qint64 kWindow = 256 * 1024;
    const filetypes::Reader &m_read;
    quint64 m_limit;
    QByteArray m_buffer;
    quint64 m_start = 0;
    qint64 m_have = 0;
    quint64 m_reached = 0;
};

// The order of this list is the type numbers; tests and saved names depend on it.
enum TypeNumber {
    Jpg, Png, Gif, Bmp, Webp, Heic, Avif, Cr3, Pdf, Docx, Xlsx, Pptx, Odt, Ods, Odp, Epub,
    Mp4, Mov, ThreeGp, Avi, Ogv, Mp3, Wav, M4a, Ogg, Opus, Zip, SevenZip, Jar, Apk, Sqlite, TypeCount
};

filetypes::Measured incomplete(int type, quint64 size)
{
    return {type, size, false};
}

filetypes::Measured complete(int type, quint64 size)
{
    return {type, size, true};
}

// --- JPEG: markers from the start to EOI (FF D9), skipping each segment by its length and the
// picture data by looking for the next marker.
filetypes::Measured measureJpeg(Stream &s)
{
    quint64 pos = 2;
    bool frame = false, scan = false;
    for (int markers = 0; markers < 100000; ++markers) {
        quint32 b;
        if (!s.u8(pos, b) || b != 0xFF)
            break;
        while (s.u8(pos + 1, b) && b == 0xFF) // fill bytes
            ++pos;
        quint32 marker;
        if (!s.u8(pos + 1, marker))
            break;
        if (marker == 0xD9)
            return frame && scan ? complete(Jpg, pos + 2) : incomplete(Jpg, pos + 2);
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            pos += 2;
            continue;
        }
        if (marker == 0x00 || marker == 0xD8)
            break;
        quint32 length;
        if (!s.be16(pos + 2, length) || length < 2)
            break;
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)
            frame = true;
        pos += 2 + length;
        if (marker != 0xDA)
            continue;
        // The coded picture: a marker is FF followed by anything but 00 and the restart markers.
        scan = true;
        for (;;) {
            const quint64 ff = s.find(pos, QByteArray("\xff", 1));
            if (ff == ~quint64(0))
                return incomplete(Jpg, frame ? s.reached() : 0);
            quint32 next;
            if (!s.u8(ff + 1, next))
                return incomplete(Jpg, 0);
            if (next == 0x00 || next == 0xFF || (next >= 0xD0 && next <= 0xD7)) {
                pos = ff + (next == 0xFF ? 1 : 2);
                continue;
            }
            pos = ff;
            break;
        }
    }
    // The structure broke off: what's there up to here may still show part of the picture.
    return incomplete(Jpg, frame && scan ? pos : 0);
}

// --- PNG: chunks (length, type, data, CRC) until IEND; every CRC is checked.
filetypes::Measured measurePng(Stream &s)
{
    quint64 pos = 8;
    bool crcsGood = true, first = true;
    for (int chunks = 0; chunks < 1000000; ++chunks) {
        quint32 length;
        if (!s.be32(pos, length) || length > 0x7fffffff)
            break;
        const uchar *peeked = s.peek(pos + 4, 4);
        if (!peeked)
            break;
        uchar type[4];
        memcpy(type, peeked, 4);
        for (int i = 0; i < 4; ++i) {
            if (!((type[i] >= 'A' && type[i] <= 'Z') || (type[i] >= 'a' && type[i] <= 'z')))
                return incomplete(Png, first ? 0 : pos);
        }
        const bool iend = memcmp(type, "IEND", 4) == 0;
        if (first && (memcmp(type, "IHDR", 4) != 0 || length != 13))
            return incomplete(Png, 0);
        first = false;
        quint32 crc, stored;
        if (!s.crc32(pos + 4, 4 + quint64(length), crc) || !s.be32(pos + 8 + length, stored))
            return incomplete(Png, pos);
        crcsGood = crcsGood && crc == stored;
        pos += 12 + quint64(length);
        if (iend)
            return {Png, pos, crcsGood};
    }
    return incomplete(Png, pos > 8 ? pos : 0);
}

// --- GIF: header, colour table, then image and extension blocks until the trailer (3B).
bool skipSubBlocks(Stream &s, quint64 &pos)
{
    for (int blocks = 0; blocks < 10000000; ++blocks) {
        quint32 n;
        if (!s.u8(pos, n))
            return false;
        pos += 1 + n;
        if (n == 0)
            return true;
    }
    return false;
}

filetypes::Measured measureGif(Stream &s)
{
    quint32 flags;
    if (!s.u8(10, flags))
        return {};
    quint64 pos = 13 + ((flags & 0x80) ? 3 * (2u << (flags & 7)) : 0);
    bool image = false;
    for (int blocks = 0; blocks < 1000000; ++blocks) {
        quint32 b;
        if (!s.u8(pos, b))
            break;
        if (b == 0x3B)
            return image ? complete(Gif, pos + 1) : incomplete(Gif, 0);
        if (b == 0x21) {
            pos += 2;
            if (!skipSubBlocks(s, pos))
                break;
        } else if (b == 0x2C) {
            quint32 local;
            if (!s.u8(pos + 9, local))
                break;
            pos += 10 + ((local & 0x80) ? 3 * (2u << (local & 7)) : 0) + 1; // + LZW code size
            if (!skipSubBlocks(s, pos))
                break;
            image = true;
        } else {
            break;
        }
    }
    return incomplete(Gif, image ? pos : 0);
}

// --- BMP: the header says the size; it's believed when the rest of the header agrees.
filetypes::Measured measureBmp(Stream &s)
{
    quint32 size, reserved, offset, dib;
    if (!s.le32(2, size) || !s.le32(6, reserved) || !s.le32(10, offset) || !s.le32(14, dib))
        return {};
    if (reserved != 0 || size < 26 || size > s.limit() || offset < 14 + dib || offset >= size)
        return {};
    qint64 width, height;
    quint32 planes, bpp, compression = 0;
    if (dib == 12) {
        quint32 w, h;
        if (!s.le16(18, w) || !s.le16(20, h) || !s.le16(22, planes) || !s.le16(24, bpp))
            return {};
        width = w;
        height = h;
    } else if (dib == 40 || dib == 52 || dib == 56 || dib == 108 || dib == 124) {
        quint32 w, h;
        if (!s.le32(18, w) || !s.le32(22, h) || !s.le16(26, planes) || !s.le16(28, bpp) || !s.le32(30, compression))
            return {};
        width = qint32(w);
        height = qint32(h);
    } else {
        return {};
    }
    if (planes != 1 || width <= 0 || width > 100000 || height == 0 || std::abs(height) > 100000
        || (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32))
        return {};
    // Uncompressed pictures can be checked: rows are padded to 4 bytes.
    if (compression == 0 || compression == 3) {
        const quint64 pixels = quint64((bpp * width + 31) / 32 * 4) * quint64(std::abs(height));
        if (quint64(offset) + pixels > size)
            return {};
        return {Bmp, size, quint64(offset) + pixels + 4096 > size};
    }
    return incomplete(Bmp, size);
}

// --- PDF: the linearized header's length when there is one, else the last %%EOF of the file.
// A %%EOF followed by more PDF is an update saved onto the end, so the search goes on.
filetypes::Measured measurePdf(Stream &s)
{
    const uchar *head = s.peek(0, 1024);
    if (head) {
        const QByteArray start(reinterpret_cast<const char *>(head), 1024);
        const qsizetype lin = start.indexOf("/Linearized");
        const qsizetype l = lin >= 0 ? start.indexOf("/L ", lin) : -1;
        if (l >= 0) {
            bool ok = false;
            const quint64 length = start.mid(l + 3, 20).split(' ').value(0).trimmed().split('/').value(0).toULongLong(&ok);
            if (ok && length > 1024 && length <= s.limit() && s.peek(length - 1, 1)) {
                const uchar *tail = s.peek(length > 1024 ? length - 1024 : 0, int(std::min<quint64>(length, 1024)));
                if (tail && QByteArray(reinterpret_cast<const char *>(tail), int(std::min<quint64>(length, 1024))).contains("%%EOF"))
                    return complete(Pdf, length);
            }
        }
    }
    quint64 end = 0;
    for (quint64 from = 5;;) {
        const quint64 eof = s.find(from, QByteArrayLiteral("%%EOF"));
        if (eof == ~quint64(0))
            break;
        end = eof + 5;
        quint32 c;
        if (s.u8(end, c) && c == '\r')
            ++end;
        if (s.u8(end, c) && c == '\n')
            ++end;
        // More PDF after it (an incremental update starts with an object number or "xref")?
        const uchar *after = s.peek(end, 16);
        if (!after)
            break;
        int i = 0;
        while (i < 16 && (after[i] == ' ' || after[i] == '\r' || after[i] == '\n'))
            ++i;
        const bool more = i < 16 && ((after[i] >= '0' && after[i] <= '9') || (i + 4 <= 16 && memcmp(after + i, "xref", 4) == 0));
        if (!more)
            break;
        from = end;
    }
    return end ? complete(Pdf, end) : incomplete(Pdf, 0);
}

// --- ZIP and everything built on it: the end of central directory record, where the central
// directory it points at ends right before it. Then the names inside say what it really is.
int zipKind(Stream &s, quint64 cdOffset, quint64 entries)
{
    bool contentTypes = false, word = false, xl = false, ppt = false, android = false, manifest = false;
    quint64 pos = cdOffset;
    for (quint64 i = 0; i < std::min<quint64>(entries, 2000); ++i) {
        quint32 nameLength, extra, comment;
        if (!s.is(pos, "PK\x01\x02") || !s.le16(pos + 28, nameLength) || !s.le16(pos + 30, extra) || !s.le16(pos + 32, comment))
            break;
        const uchar *name = s.peek(pos + 46, int(nameLength));
        if (!name)
            break;
        const QByteArray n(reinterpret_cast<const char *>(name), int(nameLength));
        contentTypes = contentTypes || n == "[Content_Types].xml";
        word = word || n.startsWith("word/");
        xl = xl || n.startsWith("xl/");
        ppt = ppt || n.startsWith("ppt/");
        android = android || n == "AndroidManifest.xml";
        manifest = manifest || n == "META-INF/MANIFEST.MF";
        pos += 46 + quint64(nameLength) + extra + comment;
    }
    if (contentTypes && word)
        return Docx;
    if (contentTypes && xl)
        return Xlsx;
    if (contentTypes && ppt)
        return Pptx;
    // OpenDocument and EPUB start with an uncompressed "mimetype" file.
    quint32 nameLength, extra;
    if (s.is(0, "PK\x03\x04") && s.le16(26, nameLength) && s.le16(28, extra) && nameLength == 8 && s.is(30, "mimetype")) {
        const uchar *mime = s.peek(38 + extra, 48);
        if (mime) {
            const QByteArray m(reinterpret_cast<const char *>(mime), 48);
            if (m.startsWith("application/vnd.oasis.opendocument.text"))
                return Odt;
            if (m.startsWith("application/vnd.oasis.opendocument.spreadsheet"))
                return Ods;
            if (m.startsWith("application/vnd.oasis.opendocument.presentation"))
                return Odp;
            if (m.startsWith("application/epub+zip"))
                return Epub;
        }
    }
    if (android)
        return Apk;
    if (manifest)
        return Jar;
    return Zip;
}

filetypes::Measured measureZip(Stream &s)
{
    for (quint64 from = 30;;) {
        const quint64 eocd = s.find(from, QByteArrayLiteral("PK\x05\x06"));
        if (eocd == ~quint64(0))
            return incomplete(Zip, 0);
        from = eocd + 4;
        quint32 disk, cdDisk, here, total, cdSize32, cdOffset32, commentLength;
        if (!s.le16(eocd + 4, disk) || !s.le16(eocd + 6, cdDisk) || !s.le16(eocd + 8, here) || !s.le16(eocd + 10, total)
            || !s.le32(eocd + 12, cdSize32) || !s.le32(eocd + 16, cdOffset32) || !s.le16(eocd + 20, commentLength))
            continue;
        quint64 cdSize = cdSize32, cdOffset = cdOffset32, entries = total, cdEnd = eocd;
        if (cdOffset32 == 0xFFFFFFFF || cdSize32 == 0xFFFFFFFF || total == 0xFFFF) {
            // ZIP64: a locator just before points at the 64-bit record.
            quint64 record;
            if (eocd < 20 || !s.is(eocd - 20, "PK\x06\x07") || !s.le64(eocd - 12, record) || !s.is(record, "PK\x06\x06")
                || !s.le64(record + 32, entries) || !s.le64(record + 40, cdSize) || !s.le64(record + 48, cdOffset))
                continue;
            cdEnd = record;
        } else if (disk != 0 || cdDisk != 0 || here != total) {
            continue;
        }
        // An archive stored inside this one has its own end record, with offsets that don't
        // fit ours: only the one whose directory ends right here is the real end.
        if (cdOffset + cdSize != cdEnd || !s.is(cdOffset, entries ? "PK\x01\x02" : "PK\x05\x06"))
            continue;
        return complete(zipKind(s, cdOffset, entries), eocd + 22 + commentLength);
    }
}

// --- MP4, MOV, 3GP, M4A, HEIC, AVIF, CR3: a row of boxes, each starting with its size.
filetypes::Measured measureBmff(Stream &s)
{
    const uchar *brand = s.peek(8, 4);
    if (!brand)
        return {};
    const QByteArray major(reinterpret_cast<const char *>(brand), 4);
    int type;
    static const QHash<QByteArray, int> brands = {
        {"isom", Mp4}, {"iso2", Mp4}, {"iso4", Mp4}, {"iso5", Mp4}, {"iso6", Mp4}, {"mp41", Mp4}, {"mp42", Mp4},
        {"avc1", Mp4}, {"M4V ", Mp4}, {"dash", Mp4}, {"MSNV", Mp4}, {"XAVC", Mp4}, {"f4v ", Mp4}, {"qt  ", Mov},
        {"3gp4", ThreeGp}, {"3gp5", ThreeGp}, {"3gp6", ThreeGp}, {"3g2a", ThreeGp}, {"M4A ", M4a}, {"M4B ", M4a},
        {"heic", Heic}, {"heix", Heic}, {"heim", Heic}, {"heis", Heic}, {"hevc", Heic}, {"mif1", Heic}, {"msf1", Heic},
        {"avif", Avif}, {"avis", Avif}, {"crx ", Cr3},
    };
    if (!brands.contains(major))
        return {};
    type = brands.value(major);
    bool moov = false, mdat = false, meta = false;
    quint64 pos = 0;
    for (int boxes = 0; boxes < 100000; ++boxes) {
        quint32 size32;
        if (!s.be32(pos, size32))
            break;
        const uchar *peeked = s.peek(pos + 4, 4);
        if (!peeked)
            break;
        uchar name[4];
        memcpy(name, peeked, 4);
        bool printable = true;
        for (int i = 0; i < 4; ++i)
            printable = printable && name[i] >= 0x20 && name[i] <= 0x7E;
        quint64 size = size32;
        if (size32 == 1 && !s.be64(pos + 8, size))
            break;
        if (!printable || size < 8 || (size32 == 1 && size < 16) || size32 == 0)
            break;
        if (boxes == 0 && memcmp(name, "ftyp", 4) != 0)
            return {};
        moov = moov || memcmp(name, "moov", 4) == 0;
        mdat = mdat || memcmp(name, "mdat", 4) == 0;
        meta = meta || memcmp(name, "meta", 4) == 0;
        if (pos + size > s.limit())
            break;
        pos += size;
    }
    if (pos <= 8)
        return {};
    const bool picture = type == Heic || type == Avif;
    const bool whole = picture ? meta && mdat : moov && mdat;
    return {type, pos, whole};
}

// --- RIFF (AVI, WAV, WebP): the size is in the header; the chunks inside have to add up to it.
filetypes::Measured measureRiff(Stream &s)
{
    quint32 size32;
    if (!s.le32(4, size32) || size32 < 12)
        return {};
    const uchar *form = s.peek(8, 8);
    if (!form)
        return {};
    int type;
    const QByteArray f(reinterpret_cast<const char *>(form), 4), first(reinterpret_cast<const char *>(form) + 4, 4);
    if (f == "AVI " && first == "LIST")
        type = Avi;
    else if (f == "WAVE" && (first == "fmt " || first == "JUNK" || first == "bext" || first == "LIST"))
        type = Wav;
    else if (f == "WEBP" && (first == "VP8 " || first == "VP8L" || first == "VP8X"))
        type = Webp;
    else
        return {};
    const quint64 total = quint64(size32) + 8;
    if (total > s.limit())
        return {};
    quint64 pos = 12;
    for (int chunks = 0; chunks < 1000000 && pos + 8 <= total; ++chunks) {
        quint32 length;
        if (!s.le32(pos + 4, length))
            return incomplete(type, total);
        pos += 8 + quint64(length) + (length & 1);
    }
    return {type, total, pos == total};
}

// --- MP3 with an ID3 tag in front: the tag says its own size; then MPEG frames, each one's
// length worked out from its header, until they stop. An ID3v1 tag can close it.
int mp3FrameLength(const uchar *h)
{
    static const int bitrates[2][3][16] = {
        {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
         {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
         {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}},
        {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}}};
    static const int rates[4][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0)
        return 0;
    const int version = (h[1] >> 3) & 3, layer = (h[1] >> 1) & 3;
    const int bitrateIndex = h[2] >> 4, rateIndex = (h[2] >> 2) & 3, padding = (h[2] >> 1) & 1;
    if (version == 1 || layer == 0 || bitrateIndex == 0 || bitrateIndex == 15 || rateIndex == 3)
        return 0;
    const int rate = rates[version][rateIndex];
    const int bitrate = bitrates[version == 3 ? 0 : 1][3 - layer][bitrateIndex] * 1000;
    if (layer == 3) // layer I
        return (12 * bitrate / rate + padding) * 4;
    return (version == 3 || layer == 2 ? 144 : 72) * bitrate / rate + padding;
}

filetypes::Measured measureMp3(Stream &s)
{
    const uchar *tag = s.peek(0, 10);
    if (!tag || tag[3] < 2 || tag[3] > 4 || (tag[6] | tag[7] | tag[8] | tag[9]) & 0x80)
        return {};
    quint64 pos = 10 + (quint64(tag[6]) << 21 | quint64(tag[7]) << 14 | quint64(tag[8]) << 7 | tag[9]) + ((tag[5] & 0x10) ? 10 : 0);
    // Some taggers leave zeros between the tag and the music.
    for (int i = 0; i < 65536; ++i) {
        quint32 b;
        if (!s.u8(pos, b) || b != 0)
            break;
        ++pos;
    }
    int frames = 0;
    for (; frames < 10000000; ++frames) {
        const uchar *h = s.peek(pos, 4);
        const int length = h ? mp3FrameLength(h) : 0;
        if (length < 4)
            break;
        pos += quint64(length);
    }
    if (s.is(pos, "TAG"))
        pos += 128;
    return frames >= 4 ? complete(Mp3, pos) : incomplete(Mp3, 0);
}

// --- Ogg: pages, each with its own CRC, from the first (BOS) to the last (EOS).
quint32 oggCrc(quint32 crc, const uchar *data, int len)
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 r = i << 24;
            for (int j = 0; j < 8; ++j)
                r = (r & 0x80000000) ? (r << 1) ^ 0x04C11DB7 : r << 1;
            t[i] = r;
        }
        return t;
    }();
    for (int i = 0; i < len; ++i)
        crc = (crc << 8) ^ table[((crc >> 24) ^ data[i]) & 0xFF];
    return crc;
}

filetypes::Measured measureOgg(Stream &s)
{
    int type = Ogg;
    quint32 segments0;
    if (s.u8(26, segments0)) {
        const uchar *payload = s.peek(27 + segments0, 8);
        if (payload && memcmp(payload, "OpusHead", 8) == 0)
            type = Opus;
        else if (payload && memcmp(payload, "\x80theora", 7) == 0)
            type = Ogv;
    }
    quint64 pos = 0;
    bool crcsGood = true, ended = false;
    for (int pages = 0; pages < 10000000; ++pages) {
        quint32 flags, segments;
        if (!s.is(pos, "OggS") || !s.u8(pos + 5, flags) || !s.u8(pos + 26, segments))
            break;
        const uchar *table = s.peek(pos + 27, int(segments));
        if (!table)
            break;
        quint64 body = 0;
        for (quint32 i = 0; i < segments; ++i)
            body += table[i];
        const quint64 length = 27 + segments + body;
        const uchar *page = s.peek(pos, int(length));
        if (!page)
            break;
        QByteArray copy(reinterpret_cast<const char *>(page), int(length));
        const quint32 stored = quint32(uchar(copy[22])) | quint32(uchar(copy[23])) << 8 | quint32(uchar(copy[24])) << 16 | quint32(uchar(copy[25])) << 24;
        memset(copy.data() + 22, 0, 4);
        crcsGood = crcsGood && oggCrc(0, reinterpret_cast<const uchar *>(copy.constData()), int(length)) == stored;
        pos += length;
        ended = flags & 0x04;
    }
    if (pos == 0)
        return {};
    return {type, pos, crcsGood && ended};
}

// --- SQLite: page size times page count, when the header's counters agree it's up to date.
filetypes::Measured measureSqlite(Stream &s)
{
    quint32 pageSize, changes, pages, validFor;
    if (!s.be16(16, pageSize) || !s.be32(24, changes) || !s.be32(28, pages) || !s.be32(92, validFor))
        return {};
    if (pageSize == 1)
        pageSize = 65536;
    if (pageSize < 512 || pageSize > 65536 || (pageSize & (pageSize - 1)) || pages == 0 || changes != validFor)
        return {};
    const quint64 size = quint64(pageSize) * pages;
    return size <= s.limit() ? complete(Sqlite, size) : incomplete(Sqlite, 0);
}

// --- 7z: the start header (with its own CRC) says where the last header is and how long it is.
filetypes::Measured measureSevenZip(Stream &s)
{
    quint32 stored;
    quint64 nextOffset, nextSize;
    if (!s.le32(8, stored))
        return {};
    const uchar *start = s.peek(12, 20);
    if (!start || filetypes::crc32(start, 20) != stored || !s.le64(12, nextOffset) || !s.le64(20, nextSize))
        return {};
    const quint64 size = 32 + nextOffset + nextSize;
    if (nextOffset > s.limit() || nextSize > s.limit() || size > s.limit() || !s.peek(size - 1, 1))
        return incomplete(SevenZip, 0);
    quint32 nextCrc, crc;
    const bool whole = nextSize <= 64 * kMiB && s.le32(28, nextCrc) && s.crc32(32 + nextOffset, nextSize, crc) && crc == nextCrc;
    return {SevenZip, size, whole};
}

} // namespace

const QVector<filetypes::Type> &filetypes::types()
{
    using C = Category;
    static const QVector<Type> list = {
        {QStringLiteral("jpg"), C::Picture, tr("JPEG picture")},
        {QStringLiteral("png"), C::Picture, tr("PNG picture")},
        {QStringLiteral("gif"), C::Picture, tr("GIF picture")},
        {QStringLiteral("bmp"), C::Picture, tr("Bitmap picture")},
        {QStringLiteral("webp"), C::Picture, tr("WebP picture")},
        {QStringLiteral("heic"), C::Picture, tr("HEIC photo")},
        {QStringLiteral("avif"), C::Picture, tr("AVIF picture")},
        {QStringLiteral("cr3"), C::Picture, tr("Canon raw photo")},
        {QStringLiteral("pdf"), C::Document, tr("PDF document")},
        {QStringLiteral("docx"), C::Document, tr("Word document")},
        {QStringLiteral("xlsx"), C::Document, tr("Excel spreadsheet")},
        {QStringLiteral("pptx"), C::Document, tr("PowerPoint presentation")},
        {QStringLiteral("odt"), C::Document, tr("OpenDocument text")},
        {QStringLiteral("ods"), C::Document, tr("OpenDocument spreadsheet")},
        {QStringLiteral("odp"), C::Document, tr("OpenDocument presentation")},
        {QStringLiteral("epub"), C::Document, tr("E-book")},
        {QStringLiteral("mp4"), C::Video, tr("MP4 video")},
        {QStringLiteral("mov"), C::Video, tr("QuickTime video")},
        {QStringLiteral("3gp"), C::Video, tr("Phone video")},
        {QStringLiteral("avi"), C::Video, tr("AVI video")},
        {QStringLiteral("ogv"), C::Video, tr("Ogg video")},
        {QStringLiteral("mp3"), C::Music, tr("MP3 music")},
        {QStringLiteral("wav"), C::Music, tr("WAV sound")},
        {QStringLiteral("m4a"), C::Music, tr("M4A music")},
        {QStringLiteral("ogg"), C::Music, tr("Ogg music")},
        {QStringLiteral("opus"), C::Music, tr("Opus sound")},
        {QStringLiteral("zip"), C::Archive, tr("ZIP archive")},
        {QStringLiteral("7z"), C::Archive, tr("7-Zip archive")},
        {QStringLiteral("jar"), C::Archive, tr("Java program")},
        {QStringLiteral("apk"), C::Archive, tr("Android app")},
        {QStringLiteral("sqlite"), C::Other, tr("SQLite database")},
    };
    Q_ASSERT(list.size() == TypeCount);
    return list;
}

int filetypes::typeNumber(const QString &id)
{
    const QVector<Type> &all = types();
    for (int i = 0; i < all.size(); ++i) {
        if (all[i].id == id)
            return i;
    }
    return -1;
}

int filetypes::typeForName(const QString &fileName)
{
    const qsizetype dot = fileName.lastIndexOf(QLatin1Char('.'));
    if (dot < 0)
        return -1;
    const QString ext = fileName.mid(dot + 1).toLower();
    static const QHash<QString, QString> also = {
        {QStringLiteral("jpeg"), QStringLiteral("jpg")}, {QStringLiteral("jpe"), QStringLiteral("jpg")},
        {QStringLiteral("jfif"), QStringLiteral("jpg")}, {QStringLiteral("heif"), QStringLiteral("heic")},
        {QStringLiteral("m4v"), QStringLiteral("mp4")},  {QStringLiteral("qt"), QStringLiteral("mov")},
        {QStringLiteral("oga"), QStringLiteral("ogg")},
    };
    return typeNumber(also.value(ext, ext));
}

int filetypes::startType(int type)
{
    switch (type) {
    case Docx:
    case Xlsx:
    case Pptx:
    case Odt:
    case Ods:
    case Odp:
    case Epub:
    case Jar:
    case Apk:
        return Zip;
    case Heic:
    case Avif:
    case Cr3:
    case Mov:
    case ThreeGp:
    case M4a:
        return Mp4;
    case Webp:
    case Wav:
        return Avi;
    case Ogv:
    case Opus:
        return Ogg;
    default:
        return type;
    }
}

bool filetypes::alwaysRecognised(int type)
{
    return type >= 0 && type != Mp3 && type != Mov;
}

QString filetypes::categoryName(Category category)
{
    switch (category) {
    case Category::Picture:
        return tr("Pictures");
    case Category::Document:
        return tr("Documents");
    case Category::Video:
        return tr("Videos");
    case Category::Music:
        return tr("Music");
    case Category::Archive:
        return tr("Archives");
    case Category::Other:
        break;
    }
    return tr("Other");
}

QVector<int> filetypes::candidates(const uchar *h, int len)
{
    QVector<int> out;
    if (len < 32)
        return out;
    switch (h[0]) {
    case 0xFF:
        if (h[1] == 0xD8 && h[2] == 0xFF && ((h[3] >= 0xE0 && h[3] <= 0xEF) || h[3] == 0xDB || h[3] == 0xFE || h[3] == 0xC0 || h[3] == 0xC4))
            out << Jpg;
        break;
    case 0x89:
        if (memcmp(h, "\x89PNG\r\n\x1a\n", 8) == 0)
            out << Png;
        break;
    case 'G':
        if (memcmp(h, "GIF87a", 6) == 0 || memcmp(h, "GIF89a", 6) == 0)
            out << Gif;
        break;
    case 'B':
        if (h[1] == 'M' && h[6] == 0 && h[7] == 0 && h[8] == 0 && h[9] == 0)
            out << Bmp;
        break;
    case '%':
        if (memcmp(h, "%PDF-", 5) == 0 && h[5] >= '1' && h[5] <= '2' && h[6] == '.')
            out << Pdf;
        break;
    case 'P':
        if (memcmp(h, "PK\x03\x04", 4) == 0 && h[4] >= 10 && h[4] <= 63 && h[5] == 0)
            out << Zip;
        break;
    case 'R':
        if (memcmp(h, "RIFF", 4) == 0)
            out << Avi; // measureRiff says which
        break;
    case 'I':
        if (memcmp(h, "ID3", 3) == 0 && h[3] >= 2 && h[3] <= 4 && h[4] != 0xFF)
            out << Mp3;
        break;
    case 'O':
        if (memcmp(h, "OggS", 4) == 0 && h[4] == 0 && (h[5] & 0x02))
            out << Ogg;
        break;
    case 'S':
        if (memcmp(h, "SQLite format 3\0", 16) == 0)
            out << Sqlite;
        break;
    case '7':
        if (memcmp(h, "7z\xbc\xaf\x27\x1c", 6) == 0)
            out << SevenZip;
        break;
    default:
        break;
    }
    if (memcmp(h + 4, "ftyp", 4) == 0)
        out << Mp4; // measureBmff says which
    return out;
}

quint64 filetypes::sizeLimit(int type)
{
    switch (type) {
    case Jpg:
    case Gif:
    case Mp3:
        return 128 * kMiB;
    case Png:
    case Bmp:
    case Webp:
    case Heic:
    case Avif:
    case Cr3:
        return 256 * kMiB;
    case Pdf:
    case Epub:
        return 512 * kMiB;
    case Mp4:
    case Mov:
    case ThreeGp:
    case Avi:
    case Ogv:
        return 16 * kGiB;
    case Sqlite:
    case Zip:
    case SevenZip:
        return 8 * kGiB;
    default:
        return 2 * kGiB;
    }
}

namespace {

filetypes::Measured measureOne(int type, Stream &s)
{
    switch (type) {
    case Jpg:
        return measureJpeg(s);
    case Png:
        return measurePng(s);
    case Gif:
        return measureGif(s);
    case Bmp:
        return measureBmp(s);
    case Pdf:
        return measurePdf(s);
    case Zip:
        return measureZip(s);
    case Mp4:
        return measureBmff(s);
    case Avi:
        return measureRiff(s);
    case Mp3:
        return measureMp3(s);
    case Ogg:
        return measureOgg(s);
    case Sqlite:
        return measureSqlite(s);
    case SevenZip:
        return measureSevenZip(s);
    default:
        return {};
    }
}

} // namespace

filetypes::Measured filetypes::measure(int type, const Reader &read)
{
    Stream s(read, sizeLimit(type));
    Measured m = measureOne(type, s);
    // A header can claim more than there is (the end of the drive, or just a wrong number).
    if (m.size > 0 && !s.peek(m.size - 1, 1)) {
        m.complete = false;
        m.size = std::min(m.size, s.reached());
    }
    return m;
}


quint32 filetypes::crc32(const uchar *data, qint64 len, quint32 crc)
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int j = 0; j < 8; ++j)
                c = (c & 1) ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (qint64 i = 0; i < len; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
