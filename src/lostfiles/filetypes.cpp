// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filetypes.h"

#include <QCoreApplication>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <array>
#include <cstring>

#include <zlib.h>

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
    Mp4, Mov, ThreeGp, Avi, Ogv, Mp3, Wav, M4a, Ogg, Opus, Zip, SevenZip, Jar, Apk, Sqlite,
    // Added in 0.5.2, at the end so the numbers before stay the same.
    Tif, Cr2, Nef, Arw, Dng, Orf, Rw2, Pef, Raf, Psd, Rar, Mkv, Webm, Flac, Wmv, Wma, Doc, Xls, Ppt, Msg, Ole, Gz,
    Eml, Mbox, Rtf, Html, Xml, Svg, Ics, Vcf, TypeCount
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

// --- TIFF, and the camera raw files built on it (CR2, NEF, ARW, DNG, ORF, RW2, PEF): it ends
// where the furthest thing any of its folders of tags points at ends: the picture's strips or
// tiles, an embedded JPEG, the tags' own values. Which camera made it says which raw file it is.
filetypes::Measured measureTiff(Stream &s)
{
    const uchar *h = s.peek(0, 16);
    if (!h)
        return {};
    const bool little = h[0] == 'I' && h[1] == 'I';
    if (!little && !(h[0] == 'M' && h[1] == 'M'))
        return {};
    const bool cr2 = h[8] == 'C' && h[9] == 'R';
    const auto u16 = [&s, little](quint64 at, quint32 &v) { return little ? s.le16(at, v) : s.be16(at, v); };
    const auto u32 = [&s, little](quint64 at, quint32 &v) { return little ? s.le32(at, v) : s.be32(at, v); };
    quint32 magic = 0, first = 0;
    if (!u16(2, magic) || !u32(4, first))
        return {};
    int type = Tif;
    if (magic == 0x4F52 || magic == 0x5352)
        type = Orf;
    else if (magic == 0x55)
        type = Rw2;
    else if (magic != 42)
        return {};
    else if (cr2)
        type = Cr2;
    quint64 end = 8;
    bool picture = false, dng = false;
    QByteArray make;
    QVector<quint64> folders{first};
    QSet<quint64> seen;
    while (!folders.isEmpty() && seen.size() < 64) {
        const quint64 at = folders.takeFirst();
        if (at < 8 || at >= s.limit() || seen.contains(at))
            continue;
        seen.insert(at);
        quint32 n = 0;
        if (!u16(at, n) || n == 0 || n > 4096)
            continue;
        end = std::max(end, at + 2 + quint64(n) * 12 + 4);
        // Where the picture's pieces are and how long each is: strips, tiles, an embedded JPEG.
        std::array<QVector<quint64>, 3> offsets, counts;
        for (quint32 i = 0; i < n; ++i) {
            const quint64 e = at + 2 + quint64(i) * 12;
            quint32 tag = 0, kind = 0, count = 0, value = 0;
            if (!u16(e, tag) || !u16(e + 2, kind) || !u32(e + 4, count) || !u32(e + 8, value))
                break;
            static constexpr int sizes[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4};
            const int size = kind < 14 ? sizes[kind] : 0;
            if (size == 0 || count == 0 || count > (1u << 24))
                continue;
            const quint64 bytes = quint64(size) * count;
            const quint64 valueAt = bytes <= 4 ? e + 8 : value;
            if (bytes > 4)
                end = std::max(end, valueAt + bytes);
            const auto item = [&](quint32 k) -> quint64 {
                quint32 v = 0;
                const bool ok = size == 2 ? u16(valueAt + 2ull * k, v) : size == 4 ? u32(valueAt + 4ull * k, v) : s.u8(valueAt + k, v);
                return ok ? v : 0;
            };
            const quint32 most = std::min<quint32>(count, 100000);
            switch (tag) {
            case 0x010F: // who made the camera
                if (const uchar *p = s.peek(valueAt, int(std::min<quint64>(bytes, 32))))
                    make = QByteArray(reinterpret_cast<const char *>(p), int(std::min<quint64>(bytes, 32))).toUpper();
                break;
            case 0xC612: // DNGVersion
                dng = true;
                break;
            case 0x8769: // more folders: the photo's settings, and the full-size pictures of a raw file
            case 0x014A:
                for (quint32 k = 0; k < std::min<quint32>(count, 16); ++k)
                    folders << item(k);
                break;
            case 0x0111:
            case 0x0144:
            case 0x0201:
                for (quint32 k = 0; k < most; ++k)
                    offsets[tag == 0x0111 ? 0 : tag == 0x0144 ? 1 : 2] << item(k);
                break;
            case 0x0117:
            case 0x0145:
            case 0x0202:
                for (quint32 k = 0; k < most; ++k)
                    counts[tag == 0x0117 ? 0 : tag == 0x0145 ? 1 : 2] << item(k);
                break;
            default:
                break;
            }
        }
        for (int k = 0; k < 3; ++k) {
            for (qsizetype j = 0; j < std::min(offsets[k].size(), counts[k].size()); ++j) {
                if (counts[k][j] > 0 && offsets[k][j] >= 8) {
                    end = std::max(end, offsets[k][j] + counts[k][j]);
                    picture = true;
                }
            }
        }
        quint32 next = 0;
        if (u32(at + 2 + quint64(n) * 12, next) && next)
            folders << next;
    }
    if (!picture)
        return {};
    if (dng)
        type = Dng;
    else if (type == Tif && make.startsWith("NIKON"))
        type = Nef;
    else if (type == Tif && make.startsWith("SONY"))
        type = Arw;
    else if (type == Tif && (make.startsWith("PENTAX") || make.startsWith("RICOH")))
        type = Pef;
    return complete(type, end);
}

// --- Fujifilm's raw files: a header saying where the embedded JPEG and the sensor data are.
filetypes::Measured measureRaf(Stream &s)
{
    if (!s.is(0, "FUJIFILMCCD-RAW "))
        return {};
    quint64 end = 0;
    for (const quint64 at : {84ull, 92ull, 100ull}) {
        quint32 offset = 0, length = 0;
        if (!s.be32(at, offset) || !s.be32(at + 4, length))
            return {};
        if (length > 0)
            end = std::max(end, quint64(offset) + length);
    }
    return end > 108 ? complete(Raf, end) : filetypes::Measured{};
}

// --- Photoshop: its sections one after another, then the merged picture, whose size comes from
// its rows (the byte count of each one when they're packed).
filetypes::Measured measurePsd(Stream &s)
{
    quint32 version = 0, channels = 0, height = 0, width = 0, depth = 0;
    if (!s.is(0, "8BPS") || !s.be16(4, version) || (version != 1 && version != 2) || !s.be16(12, channels) || !s.be32(14, height)
        || !s.be32(18, width) || !s.be16(22, depth))
        return {};
    if (channels == 0 || channels > 56 || height == 0 || width == 0 || height > 300000 || width > 300000
        || (depth != 1 && depth != 8 && depth != 16 && depth != 32))
        return {};
    quint64 at = 26;
    quint32 length = 0;
    for (int section = 0; section < 2; ++section) { // the colour table, then the picture's resources
        if (!s.be32(at, length))
            return {};
        at += 4 + quint64(length);
    }
    if (version == 1) {
        if (!s.be32(at, length))
            return {};
        at += 4 + quint64(length);
    } else {
        quint64 big = 0;
        if (!s.be64(at, big))
            return {};
        at += 8 + big;
    }
    quint32 compression = 0;
    if (!s.be16(at, compression))
        return incomplete(Psd, s.reached());
    at += 2;
    const quint64 rows = quint64(channels) * height;
    if (compression == 0)
        return complete(Psd, at + rows * ((quint64(width) * depth + 7) / 8));
    if (compression != 1)
        return incomplete(Psd, at); // compressed with ZIP: nothing says how long it is
    const int countSize = version == 1 ? 2 : 4;
    quint64 sum = 0;
    for (quint64 r = 0; r < rows; ++r) {
        quint32 count = 0;
        if (!(countSize == 2 ? s.be16(at + r * 2, count) : s.be32(at + r * 4, count)))
            return incomplete(Psd, s.reached());
        sum += count;
    }
    return complete(Psd, at + rows * countSize + sum);
}

// --- RAR 4 and 5: block after block, each header's checksum checked, to the end-of-archive one.
bool rarNumber(Stream &s, quint64 &pos, quint64 &v)
{
    v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        quint32 b = 0;
        if (!s.u8(pos++, b))
            return false;
        v |= quint64(b & 0x7F) << shift;
        if (!(b & 0x80))
            return true;
    }
    return false;
}

filetypes::Measured measureRar(Stream &s)
{
    const uchar *h = s.peek(0, 8);
    if (!h || memcmp(h, "Rar!\x1a\x07", 6) != 0)
        return {};
    if (h[6] == 1 && h[7] == 0) { // RAR 5
        quint64 pos = 8;
        for (int blocks = 0; blocks < 10000000; ++blocks) {
            quint32 crc = 0;
            quint64 p = pos + 4, size = 0, type = 0, flags = 0, extra = 0, data = 0;
            if (!s.le32(pos, crc) || !rarNumber(s, p, size) || size == 0 || size > 2 * kMiB)
                return blocks ? incomplete(Rar, pos) : filetypes::Measured{};
            const quint64 headerEnd = p + size;
            quint32 got = 0;
            if (!s.crc32(pos + 4, headerEnd - pos - 4, got) || got != crc)
                return blocks ? incomplete(Rar, pos) : filetypes::Measured{};
            if (!rarNumber(s, p, type) || !rarNumber(s, p, flags) || ((flags & 1) && !rarNumber(s, p, extra)) || ((flags & 2) && !rarNumber(s, p, data)))
                return incomplete(Rar, pos);
            pos = headerEnd + data;
            if (type == 5)
                return complete(Rar, pos);
        }
        return {};
    }
    if (h[6] != 0) // RAR 1.5 to 4: a 7-byte marker
        return {};
    quint64 pos = 7;
    for (int blocks = 0; blocks < 10000000; ++blocks) {
        quint32 crc = 0, type = 0, flags = 0, size = 0;
        if (!s.le16(pos, crc) || !s.u8(pos + 2, type) || !s.le16(pos + 3, flags) || !s.le16(pos + 5, size) || size < 7)
            return blocks ? incomplete(Rar, pos) : filetypes::Measured{};
        quint32 got = 0;
        if (!s.crc32(pos + 2, size - 2, got) || (got & 0xFFFF) != crc)
            return blocks ? incomplete(Rar, pos) : filetypes::Measured{};
        if (blocks == 0 && type != 0x73)
            return {};
        quint64 add = 0;
        quint32 low = 0, high = 0;
        if (type == 0x74 || type == 0x7A) { // a file, or a comment: its packed size, maybe in two halves
            if (!s.le32(pos + 7, low) || ((flags & 0x100) && !s.le32(pos + 32, high)))
                return incomplete(Rar, pos);
            add = quint64(high) << 32 | low;
        } else if (flags & 0x8000) {
            if (!s.le32(pos + 7, low))
                return incomplete(Rar, pos);
            add = low;
        }
        pos += size + add;
        if (type == 0x7B)
            return complete(Rar, pos);
    }
    return {};
}

// --- Matroska and WebM (EBML): the header says which, and the Segment how long the rest is. A
// live recording leaves that open, so its parts are walked to where they stop.
bool ebmlId(Stream &s, quint64 &pos, quint32 &id)
{
    quint32 b = 0;
    if (!s.u8(pos, b) || b == 0 || b < 0x10)
        return false;
    int length = 1;
    while (!(b & (0x80u >> (length - 1))))
        ++length;
    id = b;
    for (int i = 1; i < length; ++i) {
        quint32 c = 0;
        if (!s.u8(pos + i, c))
            return false;
        id = id << 8 | c;
    }
    pos += quint64(length);
    return true;
}

bool ebmlSize(Stream &s, quint64 &pos, quint64 &size, bool &unknown)
{
    quint32 b = 0;
    if (!s.u8(pos, b) || b == 0)
        return false;
    int length = 1;
    while (!(b & (0x80u >> (length - 1))))
        ++length;
    quint64 v = b & (0xFFu >> length);
    bool ones = v == (0xFFull >> length);
    for (int i = 1; i < length; ++i) {
        quint32 c = 0;
        if (!s.u8(pos + i, c))
            return false;
        v = v << 8 | c;
        ones = ones && c == 0xFF;
    }
    pos += quint64(length);
    size = v;
    unknown = ones;
    return true;
}

filetypes::Measured measureMkv(Stream &s)
{
    quint64 pos = 0, size = 0;
    quint32 id = 0;
    bool unknown = false;
    if (!ebmlId(s, pos, id) || id != 0x1A45DFA3 || !ebmlSize(s, pos, size, unknown) || unknown || size > 4096)
        return {};
    const quint64 headerEnd = pos + size;
    int type = -1;
    for (quint64 p = pos; p < headerEnd;) {
        quint32 child = 0;
        quint64 childSize = 0;
        bool open = false;
        if (!ebmlId(s, p, child) || !ebmlSize(s, p, childSize, open) || open)
            return {};
        if (child == 0x4282) // DocType
            type = s.is(p, "webm") ? Webm : s.is(p, "matroska") ? Mkv : -1;
        p += childSize;
    }
    if (type < 0)
        return {};
    pos = headerEnd;
    if (!ebmlId(s, pos, id) || id != 0x18538067 || !ebmlSize(s, pos, size, unknown))
        return incomplete(type, headerEnd);
    if (!unknown)
        return complete(type, pos + size);
    static const QSet<quint32> top = {0x114D9B74, 0x1549A966, 0x1654AE6B, 0x1F43B675, 0x1C53BB6B, 0x1043A770, 0x1254C367, 0x1941A469, 0xEC, 0xBF};
    static const QSet<quint32> inCluster = {0xE7, 0xA7, 0xAB, 0xA3, 0xA0, 0xAF, 0xEC, 0xBF};
    while (pos < s.limit()) {
        quint64 p = pos;
        if (!ebmlId(s, p, id) || !top.contains(id) || !ebmlSize(s, p, size, unknown))
            break;
        if (!unknown) {
            pos = p + size;
            continue;
        }
        if (id != 0x1F43B675)
            break;
        pos = p; // a cluster that's open too: its own parts
        for (;;) {
            quint64 q = pos;
            quint32 child = 0;
            quint64 childSize = 0;
            bool open = false;
            if (!ebmlId(s, q, child) || !inCluster.contains(child) || !ebmlSize(s, q, childSize, open) || open)
                break;
            pos = q + childSize;
        }
    }
    return incomplete(type, pos);
}

// --- FLAC: its information blocks, then frame after frame, each ending where its CRC-16 adds
// up, until all the samples STREAMINFO counts are there.
quint8 flacCrc8(const uchar *p, int n)
{
    quint8 crc = 0;
    for (int i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b)
            crc = quint8(crc & 0x80 ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}

// The frame header at `pos`: how many samples the frame has (0 when it isn't one).
quint32 flacFrame(Stream &s, quint64 pos)
{
    const uchar *h = s.peek(pos, 16);
    if (!h || h[0] != 0xFF || (h[1] & 0xFE) != 0xF8 || (h[2] >> 4) == 0 || (h[2] & 0x0F) == 0x0F || (h[3] >> 4) > 10 || (h[3] & 0x01))
        return 0;
    int at = 4;
    // The frame or sample number, UTF-8 style: 1 to 7 bytes.
    int extra = 0;
    for (quint8 b = h[4]; b & 0x80; b <<= 1)
        ++extra;
    if (extra == 1 || extra > 7)
        return 0;
    at += extra ? extra : 1;
    const int code = h[2] >> 4;
    quint32 block = code == 1 ? 192 : code <= 5 ? 576u << (code - 2) : code >= 8 ? 256u << (code - 8) : 0;
    if (code == 6)
        block = quint32(h[at++]) + 1;
    else if (code == 7) {
        block = (quint32(h[at]) << 8 | h[at + 1]) + 1;
        at += 2;
    }
    const int rate = h[2] & 0x0F;
    at += rate == 12 ? 1 : (rate == 13 || rate == 14) ? 2 : 0;
    if (at >= 16 || flacCrc8(h, at) != h[at])
        return 0;
    return block;
}

filetypes::Measured measureFlac(Stream &s)
{
    if (!s.is(0, "fLaC"))
        return {};
    quint64 pos = 4, total = 0;
    quint32 maxFrame = 0;
    for (int blocks = 0;; ++blocks) {
        quint32 head = 0;
        if (!s.be32(pos, head) || blocks > 4096)
            return {};
        const quint32 type = (head >> 24) & 0x7F, length = head & 0xFFFFFF;
        if (blocks == 0 && (type != 0 || length != 34))
            return {};
        if (type == 0) {
            const uchar *info = s.peek(pos + 4, 34);
            if (!info)
                return {};
            maxFrame = quint32(info[7]) << 16 | quint32(info[8]) << 8 | info[9];
            total = (quint64(info[13] & 0x0F) << 32) | quint64(info[14]) << 24 | quint64(info[15]) << 16 | quint64(info[16]) << 8 | info[17];
        }
        pos += 4 + quint64(length);
        if (head & 0x80000000)
            break;
    }
    const quint64 longest = maxFrame ? maxFrame : 16 * kMiB;
    quint64 samples = 0;
    while (total == 0 || samples < total) {
        const quint32 block = flacFrame(s, pos);
        if (block == 0)
            return total ? incomplete(Flac, pos) : complete(Flac, pos);
        // Over the whole frame, its checksum included, CRC-16 comes to 0.
        quint32 crc = 0;
        quint64 end = 0;
        for (quint64 q = pos; q < pos + longest + 2; ++q) {
            quint32 b = 0;
            if (!s.u8(q, b))
                break;
            crc ^= b << 8;
            for (int k = 0; k < 8; ++k)
                crc = (crc & 0x8000 ? (crc << 1) ^ 0x8005 : crc << 1) & 0xFFFF;
            if (crc == 0 && q > pos + 4) {
                const quint64 next = q + 1;
                if ((total && samples + block >= total) || flacFrame(s, next) || !s.peek(next, 1)) {
                    end = next;
                    break;
                }
            }
        }
        if (end == 0)
            return incomplete(Flac, pos);
        samples += block;
        pos = end;
    }
    return complete(Flac, pos);
}

// --- Windows Media (ASF): the header's File Properties say how long the file is; a video stream
// makes it a WMV, sound alone a WMA.
filetypes::Measured measureAsf(Stream &s)
{
    static const char kHeader[] = "\x30\x26\xB2\x75\x8E\x66\xCF\x11\xA6\xD9\x00\xAA\x00\x62\xCE\x6C";
    static const char kData[] = "\x36\x26\xB2\x75\x8E\x66\xCF\x11\xA6\xD9\x00\xAA\x00\x62\xCE\x6C";
    static const char kFileProperties[] = "\xA1\xDC\xAB\x8C\x47\xA9\xCF\x11\x8E\xE4\x00\xC0\x0C\x20\x53\x65";
    static const char kStreamProperties[] = "\x91\x07\xDC\xB7\xB7\xA9\xCF\x11\x8E\xE6\x00\xC0\x0C\x20\x53\x65";
    static const char kVideo[] = "\xC0\xEF\x19\xBC\x4D\x5B\xCF\x11\xA8\xFD\x00\x80\x5F\x5C\x44\x2B";
    const uchar *h = s.peek(0, 30);
    quint64 headerSize = 0;
    quint32 objects = 0;
    if (!h || memcmp(h, kHeader, 16) != 0 || !s.le64(16, headerSize) || !s.le32(24, objects) || headerSize < 30 || headerSize > 64 * kMiB || objects > 1000)
        return {};
    quint64 fileSize = 0;
    int type = Wma;
    quint64 pos = 30;
    for (quint32 i = 0; i < objects && pos + 24 <= headerSize; ++i) {
        const uchar *g = s.peek(pos, 16);
        if (!g)
            return {};
        const QByteArray guid(reinterpret_cast<const char *>(g), 16);
        quint64 size = 0;
        if (!s.le64(pos + 16, size) || size < 24)
            return {};
        if (guid == QByteArray(kFileProperties, 16)) {
            s.le64(pos + 40, fileSize);
        } else if (guid == QByteArray(kStreamProperties, 16)) {
            const uchar *kind = s.peek(pos + 24, 16);
            if (kind && memcmp(kind, kVideo, 16) == 0)
                type = Wmv;
        }
        pos += size;
    }
    const uchar *d = s.peek(headerSize, 16);
    quint64 dataSize = 0;
    if (!d || memcmp(d, kData, 16) != 0 || !s.le64(headerSize + 16, dataSize) || dataSize < 50)
        return incomplete(type, headerSize);
    const quint64 atLeast = headerSize + dataSize;
    // After the data come the indexes, which the file size counts.
    if (fileSize >= atLeast)
        return complete(type, fileSize);
    return fileSize ? incomplete(type, atLeast) : complete(type, atLeast);
}

// --- Office 97-2003 documents and Outlook emails are compound files (OLE2): little files of
// their own inside one, laid out in sectors. It ends at the last sector its table uses, and the
// names inside say whether it's a Word document, an Excel sheet, a PowerPoint or an email.
filetypes::Measured measureOle(Stream &s)
{
    const uchar *h = s.peek(0, 512);
    if (!h || memcmp(h, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) != 0)
        return {};
    quint32 shift = 0, fatSectors = 0, dirStart = 0, difatStart = 0, difatCount = 0;
    if (!s.le16(30, shift) || (shift != 9 && shift != 12) || !s.le32(44, fatSectors) || !s.le32(48, dirStart) || !s.le32(68, difatStart)
        || !s.le32(72, difatCount) || fatSectors == 0 || fatSectors > (1u << 20) || difatCount > (1u << 20))
        return {};
    const quint64 sector = 1ull << shift;
    const quint32 perSector = quint32(sector / 4);
    // The table's own sectors: the first 109 are listed in the header, the rest in DIFAT sectors.
    QVector<quint32> fat;
    for (int i = 0; i < 109 && quint32(fat.size()) < fatSectors; ++i) {
        quint32 v = 0;
        s.le32(76 + 4ull * i, v);
        fat << v;
    }
    quint32 next = difatStart;
    for (quint32 d = 0; d < difatCount && quint32(fat.size()) < fatSectors && next < 0xFFFFFFFA; ++d) {
        const quint64 at = (quint64(next) + 1) * sector;
        for (quint32 k = 0; k + 1 < perSector && quint32(fat.size()) < fatSectors; ++k) {
            quint32 v = 0;
            if (!s.le32(at + 4ull * k, v))
                return incomplete(Doc, s.reached());
            fat << v;
        }
        if (!s.le32(at + sector - 4, next))
            return incomplete(Doc, s.reached());
    }
    if (quint32(fat.size()) < fatSectors)
        return {};
    const auto nextSector = [&](quint32 i, quint32 &v) {
        const quint32 f = i / perSector;
        return f < quint32(fat.size()) && fat[f] < 0xFFFFFFFA && s.le32((quint64(fat[f]) + 1) * sector + 4ull * (i % perSector), v);
    };
    quint32 last = 0;
    for (quint32 i = 0; i < quint32(fat.size()) * perSector; ++i) {
        quint32 v = 0;
        if (!nextSector(i, v))
            return incomplete(Doc, s.reached());
        if (v != 0xFFFFFFFF)
            last = i;
    }
    // What's in it, by the names in its directory (128 bytes an entry, UTF-16).
    int type = Ole;
    quint32 dir = dirStart;
    for (int sectors = 0; dir < 0xFFFFFFFA && sectors < 64 && type == Ole; ++sectors) {
        for (quint64 e = 0; e < sector / 128; ++e) {
            const quint64 at = (quint64(dir) + 1) * sector + e * 128;
            const uchar *entry = s.peek(at, 128);
            if (!entry)
                break;
            const int length = std::min<int>(entry[64] | entry[65] << 8, 64);
            const QString name = QString::fromUtf16(reinterpret_cast<const char16_t *>(entry), std::max(0, length / 2 - 1));
            if (name == QLatin1String("WordDocument"))
                type = Doc;
            else if (name == QLatin1String("Workbook") || name == QLatin1String("Book"))
                type = Xls;
            else if (name == QLatin1String("PowerPoint Document"))
                type = Ppt;
            else if (name.startsWith(QLatin1String("__substg1.0_")) || name == QLatin1String("__properties_version1.0"))
                type = Msg;
        }
        if (!nextSector(dir, dir))
            break;
    }
    return complete(type, (quint64(last) + 2) * sector);
}

// --- gzip: each part unpacked to its end, its CRC-32 and length checked (one gzip file can be
// several parts in a row).
filetypes::Measured measureGz(Stream &s)
{
    quint64 pos = 0;
    int parts = 0;
    QByteArray out(64 * 1024, Qt::Uninitialized);
    for (;;) {
        const uchar *h = s.peek(pos, 10);
        if (!h || h[0] != 0x1F || h[1] != 0x8B || h[2] != 8 || (h[3] & 0xE0))
            return parts ? complete(Gz, pos) : filetypes::Measured{};
        const quint32 flags = h[3];
        quint64 p = pos + 10;
        if (flags & 4) {
            quint32 extra = 0;
            if (!s.le16(p, extra))
                return {};
            p += 2 + extra;
        }
        for (const quint32 field : {8u, 16u}) { // a name, a comment: up to a zero byte
            if (!(flags & field))
                continue;
            const quint64 zero = s.find(p, QByteArray(1, '\0'));
            if (zero == ~quint64(0) || zero - p > 4096)
                return {};
            p = zero + 1;
        }
        if (flags & 2)
            p += 2;
        z_stream z{};
        if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
            return {};
        quint32 crc = 0;
        quint64 unpacked = 0, in = p;
        int result = Z_OK;
        while (result == Z_OK) {
            const int n = int(std::min<quint64>(64 * 1024, s.limit() > in ? s.limit() - in : 0));
            const uchar *chunk = n > 0 ? s.peek(in, n) : nullptr;
            int have = n;
            if (!chunk) { // near the end: whatever is left
                have = 0;
                while (have < n && s.peek(in + quint64(have), 1))
                    have += 4096 < n - have && s.peek(in + quint64(have) + 4095, 1) ? 4096 : 1;
                chunk = have ? s.peek(in, have) : nullptr;
            }
            if (!chunk)
                break;
            z.next_in = const_cast<Bytef *>(chunk);
            z.avail_in = uInt(have);
            while (z.avail_in > 0 && result == Z_OK) {
                z.next_out = reinterpret_cast<Bytef *>(out.data());
                z.avail_out = uInt(out.size());
                result = inflate(&z, Z_NO_FLUSH);
                const uInt made = uInt(out.size()) - z.avail_out;
                crc = quint32(::crc32(crc, reinterpret_cast<const Bytef *>(out.constData()), made));
                unpacked += made;
            }
            in += quint64(have) - z.avail_in;
        }
        inflateEnd(&z);
        // Data that isn't deflate at all: not a gzip file (or its end, after a part that was).
        if (result != Z_STREAM_END && result != Z_OK)
            return parts ? complete(Gz, pos) : filetypes::Measured{};
        if (result != Z_STREAM_END) // the drive ran out first
            return parts ? complete(Gz, pos) : incomplete(Gz, in);
        quint32 wantCrc = 0, wantSize = 0;
        if (!s.le32(in, wantCrc) || !s.le32(in + 4, wantSize) || wantCrc != crc || wantSize != quint32(unpacked))
            return parts ? complete(Gz, pos) : incomplete(Gz, in + 8);
        pos = in + 8;
        ++parts;
    }
}

// --- Text that starts in a way that says what it is: emails and mailboxes, RTF, web pages, XML
// and SVG, calendars and contacts. Nothing in it says how long it is, so it runs to where its
// bytes stop looking like text, and it's whole when zeros follow it to the end of that sector
// (that's how a file's last sector is left). The kinds with a closing mark end there.

bool textByte(uchar c)
{
    return c >= 0x20 ? c != 0x7F : c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

// Where the text from `from` stops, and whether only zeros follow to the sector's end.
quint64 textEnd(Stream &s, quint64 from, quint64 most, bool *zeros)
{
    const quint64 limit = std::min(s.limit(), from + most);
    quint64 pos = from;
    while (pos < limit) {
        int n = int(std::min<quint64>(64 * 1024, limit - pos));
        const uchar *p = s.peek(pos, n);
        while (!p && n > 1) // near the end of what there is: smaller pieces
            p = s.peek(pos, n /= 2);
        if (!p)
            break;
        int i = 0;
        while (i < n && textByte(p[i]))
            ++i;
        pos += quint64(i);
        if (i < n)
            break;
    }
    *zeros = false;
    quint32 c = 0;
    if (s.u8(pos, c) && c == 0) {
        *zeros = true;
        for (quint64 q = pos + 1; *zeros && q % 512 != 0; ++q)
            *zeros = s.u8(q, c) && c == 0;
    }
    return pos;
}

filetypes::Measured textRun(Stream &s, int type, quint64 from = 0)
{
    bool zeros = false;
    const quint64 end = textEnd(s, from, filetypes::sizeLimit(type), &zeros);
    if (end <= from + 16)
        return {};
    return zeros ? complete(type, end) : incomplete(type, end);
}

// Up to the last `closing` (in either case) inside the text, and the line end after it.
filetypes::Measured textUpTo(Stream &s, int type, const QByteArray &closing)
{
    bool zeros = false;
    const quint64 end = textEnd(s, 0, filetypes::sizeLimit(type), &zeros);
    quint64 last = ~quint64(0);
    for (const QByteArray &mark : {closing.toLower(), closing.toUpper()}) {
        for (quint64 at = s.find(0, mark); at != ~quint64(0) && at < end; at = s.find(at + 1, mark))
            last = last == ~quint64(0) ? at : std::max(last, at);
    }
    if (last == ~quint64(0) || last + quint64(closing.size()) > end)
        return end > 16 ? incomplete(type, end) : filetypes::Measured{};
    quint64 after = last + quint64(closing.size());
    quint32 c = 0;
    if (s.u8(after, c) && c == '\r')
        ++after;
    if (s.u8(after, c) && c == '\n')
        ++after;
    return complete(type, after);
}

// An email's header lines: "Name: value", or carrying on the one before with a space.
bool mailHeaders(const uchar *h, int n, bool strict)
{
    static const char *const known[] = {"Return-Path:", "Received:", "From:", "Delivered-To:", "MIME-Version:", "Message-ID:", "Message-Id:",
                                        "Date:", "To:", "Subject:", "DKIM-Signature:", "ARC-Seal:", "Authentication-Results:", "X-"};
    bool first = false;
    for (const char *k : known)
        first = first || (n >= int(strlen(k)) && memcmp(h, k, strlen(k)) == 0);
    if (!first)
        return false;
    int lines = 0;
    bool from = false, other = false;
    for (int at = 0; at < n;) {
        int end = at;
        while (end < n && h[end] != '\n')
            ++end;
        if (end == n)
            break; // a line cut off by the end of what's looked at
        const int length = end - at - (end > at && h[end - 1] == '\r' ? 1 : 0);
        if (length == 0)
            return lines >= 3 && (!strict || (from && other)); // the blank line before the message
        if (h[at] != ' ' && h[at] != '\t') {
            int colon = at;
            while (colon < at + length && h[colon] != ':' && h[colon] > 0x20 && h[colon] < 0x7F)
                ++colon;
            if (colon == at || colon == at + length || h[colon] != ':')
                return false;
            const QByteArray name(reinterpret_cast<const char *>(h + at), colon - at);
            from = from || name.compare("From", Qt::CaseInsensitive) == 0;
            other = other || name.compare("Date", Qt::CaseInsensitive) == 0 || name.compare("Subject", Qt::CaseInsensitive) == 0
                 || name.compare("Message-ID", Qt::CaseInsensitive) == 0;
        }
        for (int i = at; i < at + length; ++i) {
            if (!textByte(h[i]))
                return false;
        }
        ++lines;
        at = end + 1;
    }
    return !strict && lines >= 3; // the header carries on past what's looked at
}

filetypes::Measured measureMail(Stream &s)
{
    // Its whole header, to the blank line, must read as one (at most 256 KiB of it).
    const quint64 blank = s.find(0, QByteArrayLiteral("\n\n")), crlf = s.find(0, QByteArrayLiteral("\r\n\r\n"));
    const quint64 end = std::min(blank, crlf);
    if (end == ~quint64(0) || end > 256 * 1024)
        return {};
    const int n = int(end + (end == crlf ? 4 : 2));
    const uchar *h = s.peek(0, n);
    if (!h || !mailHeaders(h, n, true))
        return {};
    return textRun(s, Eml);
}

// A mailbox (mbox), as Thunderbird and others keep them: messages one after another, each after
// a "From " line.
filetypes::Measured measureMbox(Stream &s)
{
    const uchar *h = s.peek(0, 512);
    if (!h || memcmp(h, "From ", 5) != 0)
        return {};
    int line = 5;
    while (line < 200 && h[line] != '\n')
        ++line;
    if (line >= 200 || line < 20 || !mailHeaders(h + line + 1, 511 - line, false))
        return {};
    return textRun(s, Mbox);
}

// RTF: groups in braces, to where the first one closes.
filetypes::Measured measureRtf(Stream &s)
{
    if (!s.is(0, "{\\rtf"))
        return {};
    int depth = 0;
    for (quint64 pos = 0; pos < s.limit();) {
        const uchar *p = s.peek(pos, 1);
        if (!p)
            return incomplete(Rtf, pos);
        const uchar c = *p;
        if (c == '\\') {
            // \binN: N bytes of anything follow
            if (s.is(pos + 1, "bin")) {
                quint64 n = 0, q = pos + 4;
                quint32 d = 0;
                while (s.u8(q, d) && d >= '0' && d <= '9' && n < (1ull << 40))
                    n = n * 10 + (d - '0'), ++q;
                if (s.u8(q, d) && d == ' ')
                    ++q;
                pos = q + n;
                continue;
            }
            pos += 2; // an escaped character, or the start of a control word
            continue;
        }
        if (c == '{')
            ++depth;
        else if (c == '}' && --depth == 0) {
            quint64 end = pos + 1;
            quint32 d = 0;
            while (end < pos + 3 && s.u8(end, d) && (d == '\r' || d == '\n'))
                ++end;
            return complete(Rtf, end);
        } else if (!textByte(c)) {
            return incomplete(Rtf, pos);
        }
        ++pos;
    }
    return incomplete(Rtf, s.reached());
}

filetypes::Measured measureMarkup(Stream &s)
{
    const uchar *h = s.peek(0, 512);
    if (!h)
        return {};
    const QByteArray head = QByteArray(reinterpret_cast<const char *>(h), 512).toLower();
    if (head.startsWith("<?xml")) {
        if (head.contains("<svg"))
            return textUpTo(s, Svg, QByteArrayLiteral("</svg>"));
        if (head.contains("<html"))
            return textUpTo(s, Html, QByteArrayLiteral("</html>"));
        return textRun(s, Xml);
    }
    if (head.startsWith("<svg"))
        return textUpTo(s, Svg, QByteArrayLiteral("</svg>"));
    return textUpTo(s, Html, QByteArrayLiteral("</html>"));
}

filetypes::Measured measureCards(Stream &s)
{
    if (s.is(0, "BEGIN:VCALENDAR"))
        return textUpTo(s, Ics, QByteArrayLiteral("END:VCALENDAR"));
    if (s.is(0, "BEGIN:VCARD"))
        return textUpTo(s, Vcf, QByteArrayLiteral("END:VCARD"));
    return {};
}

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
        {QStringLiteral("tif"), C::Picture, tr("TIFF picture")},
        {QStringLiteral("cr2"), C::Picture, tr("Canon raw photo (CR2)")},
        {QStringLiteral("nef"), C::Picture, tr("Nikon raw photo")},
        {QStringLiteral("arw"), C::Picture, tr("Sony raw photo")},
        {QStringLiteral("dng"), C::Picture, tr("Raw photo (DNG)")},
        {QStringLiteral("orf"), C::Picture, tr("Olympus raw photo")},
        {QStringLiteral("rw2"), C::Picture, tr("Panasonic raw photo")},
        {QStringLiteral("pef"), C::Picture, tr("Pentax raw photo")},
        {QStringLiteral("raf"), C::Picture, tr("Fujifilm raw photo")},
        {QStringLiteral("psd"), C::Picture, tr("Photoshop picture")},
        {QStringLiteral("rar"), C::Archive, tr("RAR archive")},
        {QStringLiteral("mkv"), C::Video, tr("Matroska video")},
        {QStringLiteral("webm"), C::Video, tr("WebM video")},
        {QStringLiteral("flac"), C::Music, tr("FLAC music")},
        {QStringLiteral("wmv"), C::Video, tr("Windows Media video")},
        {QStringLiteral("wma"), C::Music, tr("Windows Media audio")},
        {QStringLiteral("doc"), C::Document, tr("Word 97-2003 document")},
        {QStringLiteral("xls"), C::Document, tr("Excel 97-2003 spreadsheet")},
        {QStringLiteral("ppt"), C::Document, tr("PowerPoint 97-2003 presentation")},
        {QStringLiteral("msg"), C::Document, tr("Outlook email")},
        {QStringLiteral("ole"), C::Other, tr("Compound file")},
        {QStringLiteral("gz"), C::Archive, tr("gzip file")},
        {QStringLiteral("eml"), C::Document, tr("Email")},
        {QStringLiteral("mbox"), C::Document, tr("Mailbox")},
        {QStringLiteral("rtf"), C::Document, tr("Rich text document")},
        {QStringLiteral("html"), C::Document, tr("Web page")},
        {QStringLiteral("xml"), C::Other, tr("XML file")},
        {QStringLiteral("svg"), C::Picture, tr("SVG drawing")},
        {QStringLiteral("ics"), C::Document, tr("Calendar")},
        {QStringLiteral("vcf"), C::Document, tr("Contacts")},
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
        {QStringLiteral("oga"), QStringLiteral("ogg")},  {QStringLiteral("tiff"), QStringLiteral("tif")},
        {QStringLiteral("psb"), QStringLiteral("psd")},  {QStringLiteral("nrw"), QStringLiteral("nef")},
        {QStringLiteral("mka"), QStringLiteral("mkv")},  {QStringLiteral("asf"), QStringLiteral("wmv")},
        {QStringLiteral("dot"), QStringLiteral("doc")},  {QStringLiteral("xlt"), QStringLiteral("xls")},
        {QStringLiteral("pps"), QStringLiteral("ppt")},  {QStringLiteral("tgz"), QStringLiteral("gz")},
        {QStringLiteral("htm"), QStringLiteral("html")}, {QStringLiteral("vcard"), QStringLiteral("vcf")},
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
    case Cr2:
    case Nef:
    case Arw:
    case Dng:
    case Orf:
    case Rw2:
    case Pef:
        return Tif;
    case Webm:
        return Mkv;
    case Xml:
    case Svg:
        return Html;
    case Vcf:
        return Ics;
    case Wma:
        return Wmv;
    case Xls:
    case Ppt:
    case Msg:
    case Ole:
        return Doc;
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
        else if (memcmp(h, "Rar!\x1a\x07", 6) == 0)
            out << Rar;
        break;
    case 'I':
        if (memcmp(h, "ID3", 3) == 0 && h[3] >= 2 && h[3] <= 4 && h[4] != 0xFF)
            out << Mp3;
        // TIFF and the raw files built on it (Olympus and Panasonic put their own number there)
        if (h[1] == 'I' && (memcmp(h + 2, "*\0", 2) == 0 || memcmp(h + 2, "RO", 2) == 0 || memcmp(h + 2, "RS", 2) == 0 || memcmp(h + 2, "U\0", 2) == 0))
            out << Tif;
        break;
    case 'M':
        if (memcmp(h, "MM\0*", 4) == 0)
            out << Tif;
        break;
    case 'F':
        if (memcmp(h, "FUJIFILMCCD-RAW ", 16) == 0)
            out << Raf;
        break;
    case '8':
        if (memcmp(h, "8BPS", 4) == 0)
            out << Psd;
        break;
    case 0x1A:
        if (memcmp(h, "\x1A\x45\xDF\xA3", 4) == 0)
            out << Mkv;
        break;
    case 'f':
        if (memcmp(h, "fLaC", 4) == 0)
            out << Flac;
        break;
    case 0x30:
        if (memcmp(h, "\x30\x26\xB2\x75\x8E\x66\xCF\x11", 8) == 0)
            out << Wmv;
        break;
    case 0xD0:
        if (memcmp(h, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) == 0)
            out << Doc;
        break;
    case 0x1F:
        if (h[1] == 0x8B && h[2] == 8 && !(h[3] & 0xE0))
            out << Gz;
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
    // Text kinds, by how they start.
    if (h[0] == '<') {
        QByteArray start = QByteArray(reinterpret_cast<const char *>(h), 16).toLower();
        if (start.startsWith("<?xml") || start.startsWith("<!doctype html") || start.startsWith("<html") || start.startsWith("<svg"))
            out << Html; // measureMarkup says which
    } else if (h[0] == '{' && memcmp(h, "{\\rtf", 5) == 0) {
        out << Rtf;
    } else if (h[0] == 'B' && (memcmp(h, "BEGIN:VCALENDAR", 15) == 0 || memcmp(h, "BEGIN:VCARD", 11) == 0)) {
        out << Ics; // or Vcf
    } else if (memcmp(h, "From ", 5) == 0) {
        out << Mbox;
    } else if (h[0] >= 'A' && h[0] <= 'Z' && mailHeaders(h, len, false)) {
        out << Eml;
    }
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
    case Rar:
    case Gz:
        return 8 * kGiB;
    case Tif:
    case Cr2:
    case Nef:
    case Arw:
    case Dng:
    case Orf:
    case Rw2:
    case Pef:
    case Raf:
        return 512 * kMiB;
    case Mkv:
    case Webm:
    case Wmv:
        return 16 * kGiB;
    case Doc:
    case Xls:
    case Ppt:
    case Msg:
    case Ole:
        return 1 * kGiB;
    case Mbox:
        return 16 * kGiB;
    case Eml:
    case Rtf:
    case Xml:
        return 512 * kMiB;
    case Html:
    case Svg:
    case Ics:
    case Vcf:
        return 64 * kMiB;
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
    case Tif:
        return measureTiff(s);
    case Raf:
        return measureRaf(s);
    case Psd:
        return measurePsd(s);
    case Rar:
        return measureRar(s);
    case Mkv:
        return measureMkv(s);
    case Flac:
        return measureFlac(s);
    case Wmv:
        return measureAsf(s);
    case Doc:
        return measureOle(s);
    case Gz:
        return measureGz(s);
    case Eml:
        return measureMail(s);
    case Mbox:
        return measureMbox(s);
    case Rtf:
        return measureRtf(s);
    case Html:
        return measureMarkup(s);
    case Ics:
        return measureCards(s);
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
