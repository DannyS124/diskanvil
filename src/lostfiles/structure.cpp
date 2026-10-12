// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "structure.h"

#include "filetypes.h"
#include "ole.h"

#include <QByteArrayView>
#include <QCoreApplication>
#include <QHash>
#include <QSet>
#include <QtEndian>

#include <cstring>

#include <zlib.h>

namespace structure {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("structure", text);
}

// A view of bytes that never reads past their end: what isn't there reads as zero.
struct Bytes {
    const uchar *p;
    quint64 n;
    bool has(quint64 at, quint64 len) const { return at <= n && len <= n - at; }
    quint32 u8(quint64 at) const { return has(at, 1) ? p[at] : 0; }
    quint32 be16(quint64 at) const { return has(at, 2) ? qFromBigEndian<quint16>(p + at) : 0; }
    quint32 be24(quint64 at) const { return has(at, 3) ? quint32(p[at]) << 16 | quint32(p[at + 1]) << 8 | p[at + 2] : 0; }
    quint32 be32(quint64 at) const { return has(at, 4) ? qFromBigEndian<quint32>(p + at) : 0; }
    quint64 be64(quint64 at) const { return has(at, 8) ? qFromBigEndian<quint64>(p + at) : 0; }
    quint32 le16(quint64 at) const { return has(at, 2) ? qFromLittleEndian<quint16>(p + at) : 0; }
    quint32 le32(quint64 at) const { return has(at, 4) ? qFromLittleEndian<quint32>(p + at) : 0; }
    quint64 le64(quint64 at) const { return has(at, 8) ? qFromLittleEndian<quint64>(p + at) : 0; }
    bool is(quint64 at, const char *text, int len = -1) const
    {
        const quint64 l = len < 0 ? strlen(text) : quint64(len);
        return has(at, l) && std::memcmp(p + at, text, l) == 0;
    }
    QByteArray mid(quint64 at, quint64 len) const
    {
        if (at >= n)
            return {};
        return QByteArray(reinterpret_cast<const char *>(p + at), qsizetype(std::min(len, n - at)));
    }
    QString ascii(quint64 at, quint64 len) const
    {
        QString out;
        for (quint64 i = 0; i < len && has(at + i, 1); ++i) {
            const uchar c = p[at + i];
            out += c >= 0x20 && c < 0x7F ? QChar(c) : QLatin1Char('.');
        }
        return out;
    }
    // Where `needle` next is from `from` on, or n.
    quint64 find(quint64 from, const char *needle) const
    {
        const quint64 l = strlen(needle);
        if (l == 0 || from >= n)
            return n;
        const qsizetype hit = QByteArrayView(p + from, qsizetype(n - from)).indexOf(QByteArrayView(needle, qsizetype(l)));
        return hit >= 0 ? from + quint64(hit) : n;
    }
};

class List
{
public:
    QVector<Part> out;
    bool full() const { return out.size() >= kMostParts; }
    void add(quint64 at, quint64 size, int depth, const QString &name, const QString &detail = {}, const QString &problem = {})
    {
        if (!full())
            out.push_back({at, size, depth, name, detail, problem});
    }
};

QString bytes(quint64 n)
{
    return QCoreApplication::translate("structure", "%n byte(s)", nullptr, int(std::min<quint64>(n, INT_MAX)));
}

QString cutShort()
{
    return tr("It runs past the end of the file: the file is cut short here");
}

// What's after the end of a file's own structure: zeros (padding) or something else.
void after(const Bytes &b, quint64 end, List &l)
{
    if (end >= b.n)
        return;
    bool zeros = true;
    for (quint64 i = end; i < b.n && zeros; ++i)
        zeros = b.p[i] == 0;
    l.add(end, b.n - end, 0, tr("After the end"),
          zeros ? tr("Zeros: nothing") : tr("Bytes after the file's own end: another file, or something left over"));
}

// --- JPEG

QString jpegMarker(quint32 m)
{
    static const QHash<quint32, const char *> names = {{0xC4, "DHT"}, {0xCC, "DAC"}, {0xD8, "SOI"}, {0xD9, "EOI"}, {0xDA, "SOS"},
                                                       {0xDB, "DQT"}, {0xDD, "DRI"}, {0xFE, "COM"}};
    if (names.contains(m))
        return QString::fromLatin1(names.value(m));
    if (m >= 0xE0 && m <= 0xEF)
        return QStringLiteral("APP%1").arg(m - 0xE0);
    if (m >= 0xC0 && m <= 0xCF)
        return QStringLiteral("SOF%1").arg(m - 0xC0);
    return QStringLiteral("FF%1").arg(m, 2, 16, QLatin1Char('0')).toUpper();
}

void jpeg(const Bytes &b, List &l)
{
    l.add(0, 2, 0, QStringLiteral("SOI"), tr("Start of the picture"));
    quint64 pos = 2;
    bool end = false, frame = false;
    while (pos + 2 <= b.n && !l.full()) {
        if (b.u8(pos) != 0xFF) {
            l.add(pos, b.n - pos, 0, tr("Unknown"), {}, tr("A marker should be here: the file is damaged from here on"));
            return;
        }
        const quint32 m = b.u8(pos + 1);
        if (m == 0xFF) {
            ++pos;
            continue;
        }
        if (m == 0xD9) {
            l.add(pos, 2, 0, QStringLiteral("EOI"), tr("End of the picture"));
            pos += 2;
            end = true;
            break;
        }
        if ((m >= 0xD0 && m <= 0xD7) || m == 0x01) {
            pos += 2;
            continue;
        }
        const quint32 length = b.be16(pos + 2);
        if (length < 2 || !b.has(pos + 2, length)) {
            l.add(pos, b.n - pos, 0, jpegMarker(m), {}, cutShort());
            return;
        }
        QString detail;
        const quint64 body = pos + 4;
        if (m >= 0xE0 && m <= 0xEF) {
            const QString id = b.ascii(body, std::min<quint32>(length - 2, 30)).section(QLatin1Char('.'), 0, 0);
            detail = id.startsWith(QLatin1String("http://ns.adobe.com/xap")) ? QStringLiteral("XMP") : id;
            if (detail == QLatin1String("Exif"))
                detail = tr("EXIF: the camera's details and a small copy of the photo");
            else if (detail == QLatin1String("ICC_PROFILE"))
                detail = tr("Colour profile");
        } else if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            frame = true;
            detail = tr("%1 × %2 pixels, %3 colour parts, %4-bit%5")
                         .arg(b.be16(body + 3))
                         .arg(b.be16(body + 1))
                         .arg(b.u8(body + 5))
                         .arg(b.u8(body))
                         .arg(m == 0xC2 ? tr(", progressive") : QString());
        } else if (m == 0xDB) {
            detail = tr("How finely it's compressed");
        } else if (m == 0xC4) {
            detail = tr("Huffman tables");
        } else if (m == 0xFE) {
            detail = b.ascii(body, std::min<quint32>(length - 2, 60));
        }
        l.add(pos, 2 + quint64(length), 0, jpegMarker(m), detail,
              m == 0xDA && !frame ? tr("The picture's size (SOF) should come before this") : QString());
        pos += 2 + quint64(length);
        if (m == 0xDA) {
            quint64 q = pos;
            for (; q + 1 < b.n; ++q) {
                if (b.p[q] == 0xFF) {
                    const uchar next = b.p[q + 1];
                    if (next != 0 && !(next >= 0xD0 && next <= 0xD7) && next != 0xFF)
                        break;
                }
            }
            const bool stopped = q + 1 >= b.n;
            l.add(pos, (stopped ? b.n : q) - pos, 1, tr("Picture data"), bytes((stopped ? b.n : q) - pos),
                  stopped ? tr("It ends before the picture is done: the file is cut short") : QString());
            pos = stopped ? b.n : q;
        }
    }
    if (!end)
        l.add(b.n, 0, 0, QStringLiteral("EOI"), {}, tr("Missing: the file is cut short"));
    after(b, pos, l);
}

// --- PNG

void png(const Bytes &b, List &l)
{
    l.add(0, 8, 0, tr("Signature"), QStringLiteral("PNG"));
    quint64 pos = 8;
    bool end = false;
    while (pos + 12 <= b.n && !l.full()) {
        const quint32 length = b.be32(pos);
        const QString name = b.ascii(pos + 4, 4);
        if (length > b.n || !b.has(pos + 8, quint64(length) + 4)) {
            l.add(pos, b.n - pos, 0, name, {}, cutShort());
            return;
        }
        const quint32 crc = filetypes::crc32(b.p + pos + 4, int(length + 4));
        QString detail = bytes(length);
        if (name == QLatin1String("IHDR")) {
            detail = tr("%1 × %2 pixels, %3-bit").arg(b.be32(pos + 8)).arg(b.be32(pos + 12)).arg(b.u8(pos + 16));
        } else if (name == QLatin1String("tEXt") || name == QLatin1String("iTXt") || name == QLatin1String("zTXt")) {
            detail = b.ascii(pos + 8, std::min<quint32>(length, 40)).section(QLatin1Char('.'), 0, 0);
        } else if (name == QLatin1String("IDAT")) {
            detail = tr("Picture data, %1").arg(bytes(length));
        }
        l.add(pos, 12 + quint64(length), 0, name, detail,
              crc != b.be32(pos + 8 + length) ? tr("Its checksum doesn't match: damaged (Repair can put the checksums right)") : QString());
        pos += 12 + quint64(length);
        if (name == QLatin1String("IEND")) {
            end = true;
            break;
        }
    }
    if (!end)
        l.add(pos, b.n - pos, 0, QStringLiteral("IEND"), {}, tr("Missing: the file is cut short"));
    after(b, pos, l);
}

// --- GIF

void gif(const Bytes &b, List &l)
{
    l.add(0, 6, 0, tr("Header"), b.ascii(0, 6));
    const quint32 flags = b.u8(10);
    l.add(6, 7, 0, tr("Screen"), tr("%1 × %2 pixels").arg(b.le16(6)).arg(b.le16(8)));
    quint64 pos = 13;
    if (flags & 0x80) {
        const quint64 table = 3ull << ((flags & 7) + 1);
        l.add(pos, table, 0, tr("Colours"));
        pos += table;
    }
    const auto blocks = [&b](quint64 at) { // sub-blocks to the zero one
        while (b.has(at, 1) && b.u8(at) != 0)
            at += 1 + quint64(b.u8(at));
        return at + 1;
    };
    int pictures = 0;
    while (pos < b.n && !l.full()) {
        const quint32 kind = b.u8(pos);
        if (kind == 0x3B) {
            l.add(pos, 1, 0, tr("End"));
            after(b, pos + 1, l);
            return;
        }
        if (kind == 0x21) {
            const quint32 label = b.u8(pos + 1);
            const quint64 end = blocks(pos + 2);
            l.add(pos, std::min(end, b.n) - pos, 0,
                  label == 0xF9 ? tr("Timing") : label == 0xFE ? tr("Comment") : label == 0xFF ? tr("Application: %1").arg(b.ascii(pos + 3, 8)) : tr("Extension"),
                  {}, end > b.n ? cutShort() : QString());
            pos = end;
        } else if (kind == 0x2C) {
            quint64 end = pos + 10;
            const quint32 local = b.u8(pos + 9);
            if (local & 0x80)
                end += 3ull << ((local & 7) + 1);
            end = blocks(end + 1);
            l.add(pos, std::min(end, b.n) - pos, 0, tr("Picture %1").arg(++pictures), tr("%1 × %2 pixels").arg(b.le16(pos + 5)).arg(b.le16(pos + 7)),
                  end > b.n ? cutShort() : QString());
            pos = end;
        } else {
            l.add(pos, b.n - pos, 0, tr("Unknown"), {}, tr("Damaged from here on"));
            return;
        }
    }
    l.add(b.n, 0, 0, tr("End"), {}, tr("Missing: the file is cut short"));
}

// --- BMP

void bmp(const Bytes &b, List &l)
{
    const quint64 size = b.le32(2), pixels = b.le32(10), header = b.le32(14);
    l.add(0, 14, 0, tr("File header"), bytes(size), size > b.n ? cutShort() : QString());
    l.add(14, header, 0, tr("Picture header"), tr("%1 × %2 pixels, %3-bit").arg(qint32(b.le32(18))).arg(std::abs(qint32(b.le32(22)))).arg(b.le16(28)));
    if (pixels > 14 + header)
        l.add(14 + header, pixels - 14 - header, 0, tr("Colours"));
    l.add(pixels, (size > pixels ? size : b.n) - pixels, 0, tr("Pixels"), {}, size > b.n || pixels > b.n ? cutShort() : QString());
    after(b, std::max<quint64>(size, pixels), l);
}

// --- MP4, MOV, 3GP, M4A, HEIC, AVIF, CR3: boxes inside boxes

void boxes(const Bytes &b, quint64 from, quint64 to, int depth, List &l, bool *moov, bool *mdat)
{
    static const QSet<QByteArray> containers = {"moov", "trak", "mdia", "minf", "stbl", "dinf", "udta", "edts", "mvex", "moof", "traf",
                                                "ilst", "iprp", "ipco", "sinf", "schi", "mfra", "tref", "gmhd"};
    for (quint64 at = from; at + 8 <= to && !l.full();) {
        quint64 size = b.be32(at);
        const QByteArray name = b.mid(at + 4, 4);
        quint64 header = 8;
        if (size == 1) {
            size = b.be64(at + 8);
            header = 16;
        } else if (size == 0) {
            size = to - at; // to the end
        }
        if (size < header) {
            l.add(at, to - at, depth, QString::fromLatin1(name), {}, tr("A box with an impossible size: damaged from here on"));
            return;
        }
        const bool fits = at + size <= to;
        QString detail;
        if (name == "ftyp") {
            detail = tr("Kind: %1").arg(b.ascii(at + 8, 4));
        } else if (name == "mvhd") {
            const bool v1 = b.u8(at + 8) == 1;
            const quint64 scale = v1 ? b.be32(at + 28) : b.be32(at + 20), length = v1 ? b.be64(at + 32) : b.be32(at + 24);
            if (scale)
                detail = tr("%1 seconds long").arg(length / scale);
        } else if (name == "hdlr") {
            const QByteArray handler = b.mid(at + 16, 4);
            detail = handler == "vide" ? tr("Video") : handler == "soun" ? tr("Sound") : QString::fromLatin1(handler);
        } else if (name == "tkhd") {
            const quint64 end = at + (b.u8(at + 8) == 1 ? 96 : 84);
            if (b.be32(end) >> 16)
                detail = tr("%1 × %2").arg(b.be32(end) >> 16).arg(b.be32(end + 4) >> 16);
        } else if (name == "mdat") {
            detail = tr("The film and sound themselves, %1").arg(bytes(size - header));
            *mdat = true;
        }
        if (name == "moov")
            *moov = true;
        l.add(at, fits ? size : to - at, depth, QString::fromLatin1(name), detail, fits ? QString() : cutShort());
        if (depth < 8 && (containers.contains(name) || name == "meta")) {
            const quint64 skip = name == "meta" ? 4 : 0; // meta is a "full box": version and flags first
            boxes(b, at + header + skip, std::min(at + size, to), depth + 1, l, moov, mdat);
        }
        if (!fits)
            return;
        at += size;
    }
}

void bmff(const Bytes &b, List &l)
{
    bool moov = false, mdat = false;
    boxes(b, 0, b.n, 0, l, &moov, &mdat);
    if (mdat && !moov)
        l.add(b.n, 0, 0, QStringLiteral("moov"), {},
              tr("Missing: the index that says where each frame is (the recording stopped before it was written). Repair can rebuild it "
                 "from another clip made with the same camera"));
}

// --- RIFF: WAV, AVI, WebP

void riff(const Bytes &b, quint64 from, quint64 to, int depth, List &l)
{
    for (quint64 at = from; at + 8 <= to && !l.full();) {
        const QByteArray id = b.mid(at, 4);
        const quint64 size = b.le32(at + 4);
        const bool fits = at + 8 + size <= to;
        QString detail = bytes(size);
        if (id == "fmt ")
            detail = tr("%1 channels, %2 Hz").arg(b.le16(at + 10)).arg(b.le32(at + 12));
        else if (id == "LIST" || id == "RIFF")
            detail = b.ascii(at + 8, 4);
        else if (id == "avih")
            detail = tr("%1 frames, %2 × %3").arg(b.le32(at + 24)).arg(b.le32(at + 40)).arg(b.le32(at + 44));
        l.add(at, fits ? 8 + size + (size & 1) : to - at, depth, QString::fromLatin1(id).trimmed(), detail, fits ? QString() : cutShort());
        if ((id == "LIST" || id == "RIFF") && depth < 6)
            riff(b, at + 12, std::min(at + 8 + size, to), depth + 1, l);
        if (!fits)
            return;
        at += 8 + size + (size & 1);
    }
}

// --- ZIP and the files built on it: the central directory, each entry checked against its CRC

void zip(const Bytes &b, List &l)
{
    quint64 eocd = b.n;
    for (quint64 at = b.n >= 22 ? b.n - 22 : 0; at + 22 <= b.n && b.n - at <= 65557; --at) {
        if (b.le32(at) == 0x06054b50) {
            eocd = at;
            break;
        }
        if (at == 0)
            break;
    }
    if (eocd == b.n) {
        // No directory: the entries one after another, as far as their own headers say.
        quint64 at = 0;
        while (b.le32(at) == 0x04034b50 && !l.full()) {
            const quint64 packed = b.le32(at + 18), length = 30 + quint64(b.le16(at + 26)) + b.le16(at + 28) + packed;
            l.add(at, std::min(length, b.n - at), 0, QString::fromUtf8(b.mid(at + 30, b.le16(at + 26))), bytes(packed),
                  b.le16(at + 6) & 8 ? tr("Its size is only in the missing directory") : QString());
            if (b.le16(at + 6) & 8)
                break;
            at += length;
        }
        l.add(b.n, 0, 0, tr("Directory"), {}, tr("Missing: the list of what's inside it, at its end. Repair can rebuild it from the entries"));
        return;
    }
    const quint64 count = b.le16(eocd + 10), dirSize = b.le32(eocd + 12), dirAt = b.le32(eocd + 16);
    quint64 at = dirAt;
    for (quint64 i = 0; i < count && b.le32(at) == 0x02014b50 && !l.full(); ++i) {
        const quint32 method = b.le16(at + 10), crc = b.le32(at + 16);
        const quint64 packed = b.le32(at + 20), unpacked = b.le32(at + 24), local = b.le32(at + 42);
        const quint32 nameLength = b.le16(at + 28);
        const QString name = QString::fromUtf8(b.mid(at + 46, nameLength));
        const quint64 data = local + 30 + b.le16(local + 26) + b.le16(local + 28);
        QString problem;
        if (b.le32(local) != 0x04034b50)
            problem = tr("Its own header isn't where the directory says");
        else if (!b.has(data, packed))
            problem = cutShort();
        else if (unpacked <= 256ull * 1024 * 1024 && (method == 0 || method == 8)) {
            // Its contents against their checksum.
            quint32 got = 0;
            if (method == 0) {
                got = quint32(::crc32(0, b.p + data, uInt(packed)));
            } else {
                z_stream z{};
                if (inflateInit2(&z, -MAX_WBITS) == Z_OK) {
                    QByteArray out(64 * 1024, Qt::Uninitialized);
                    z.next_in = const_cast<Bytef *>(b.p + data);
                    z.avail_in = uInt(packed);
                    int result = Z_OK;
                    while (result == Z_OK) {
                        z.next_out = reinterpret_cast<Bytef *>(out.data());
                        z.avail_out = uInt(out.size());
                        result = inflate(&z, Z_NO_FLUSH);
                        got = quint32(::crc32(got, reinterpret_cast<const Bytef *>(out.constData()), uInt(out.size()) - z.avail_out));
                    }
                    inflateEnd(&z);
                    if (result != Z_STREAM_END)
                        problem = tr("It can't be unpacked: damaged");
                }
            }
            if (problem.isEmpty() && got != crc)
                problem = tr("Its contents don't match their checksum: damaged");
        }
        l.add(local, data + packed - local, 0, name,
              method == 0 ? tr("Stored, %1").arg(bytes(unpacked)) : tr("Packed, %1 → %2").arg(bytes(packed), bytes(unpacked)), problem);
        at += 46 + quint64(nameLength) + b.le16(at + 30) + b.le16(at + 32);
    }
    l.add(dirAt, dirSize, 0, tr("Directory"), QCoreApplication::translate("structure", "%n entries", nullptr, int(count)),
          dirAt + dirSize > eocd ? tr("It runs into its end record: damaged") : QString());
    l.add(eocd, 22 + quint64(b.le16(eocd + 20)), 0, tr("End of the directory"));
    after(b, eocd + 22 + b.le16(eocd + 20), l);
}

// --- PDF: objects, the cross-reference table that says where they are, and the trailer

void pdf(const Bytes &b, List &l)
{
    const quint64 line = b.find(0, "\n");
    l.add(0, std::min(line + 1, b.n), 0, tr("Header"), b.ascii(0, std::min<quint64>(line, 12)));
    QSet<quint64> objects;
    quint64 pos = 0;
    while (pos < b.n && !l.full()) {
        const quint64 obj = b.find(pos, " obj");
        const quint64 xref = b.find(pos, "xref");
        const quint64 trailer = b.find(pos, "trailer");
        const quint64 next = std::min({obj, xref, trailer});
        if (next >= b.n)
            break;
        if (next == obj) {
            // Back over "12 0" to where the object starts.
            quint64 start = obj;
            int numbers = 0;
            while (start > 0 && numbers < 2) {
                quint64 s = start;
                while (s > 0 && b.p[s - 1] == ' ')
                    --s;
                quint64 d = s;
                while (d > 0 && b.p[d - 1] >= '0' && b.p[d - 1] <= '9')
                    --d;
                if (d == s)
                    break;
                start = d;
                ++numbers;
            }
            if (numbers < 2) {
                pos = obj + 4;
                continue;
            }
            const quint64 end = b.find(obj, "endobj");
            const QByteArray head = b.mid(obj + 4, 300);
            QString kind;
            const qsizetype type = head.indexOf("/Type");
            if (type >= 0)
                kind = QString::fromLatin1(head.mid(type + 5, 20).trimmed().split('/').value(1).split(' ').first().split('>').first().split('\n').first());
            if (head.contains("stream"))
                kind += (kind.isEmpty() ? QString() : QStringLiteral(", ")) + tr("with data");
            objects.insert(start);
            l.add(start, (end < b.n ? end + 6 : b.n) - start, 0, tr("Object %1").arg(QString::fromLatin1(b.mid(start, obj - start)).simplified()), kind,
                  end >= b.n ? cutShort() : QString());
            pos = end < b.n ? end + 6 : b.n;
        } else if (next == xref && (xref == 0 || b.p[xref - 1] == '\n' || b.p[xref - 1] == '\r')) {
            const quint64 end = b.find(xref, "trailer");
            l.add(xref, std::min(end, b.n) - xref, 0, tr("Where the objects are (xref)"));
            pos = xref + 4;
        } else if (next == trailer) {
            const quint64 eof = b.find(trailer, "%%EOF");
            l.add(trailer, std::min(eof + 5, b.n) - trailer, 0, tr("Trailer"), {}, eof >= b.n ? tr("Its %%EOF is missing: cut short") : QString());
            pos = trailer + 7;
        } else {
            pos = next + 4;
        }
    }
    // startxref must point at the table (or a cross-reference stream object).
    const quint64 start = b.n > 1024 ? b.n - 1024 : 0;
    quint64 last = b.n;
    for (quint64 at = b.find(start, "startxref"); at < b.n; at = b.find(at + 9, "startxref"))
        last = at;
    if (last < b.n) {
        const quint64 where = b.mid(last + 9, 24).trimmed().split('\n').first().trimmed().toULongLong();
        const bool good = b.is(where, "xref") || objects.contains(where);
        l.add(last, 9, 0, QStringLiteral("startxref"), tr("Says the table is at %1").arg(where),
              good ? QString() : tr("That's not where the table is: the file was changed or damaged. Repair can rebuild the table"));
    } else {
        l.add(b.n, 0, 0, QStringLiteral("startxref"), {}, tr("Missing: the file is cut short. Repair can rebuild the table"));
    }
}

// --- TIFF and the raw photos built on it: folders of tags, and the picture data they point at

void tiff(const Bytes &b, List &l)
{
    const bool little = b.u8(0) == 'I';
    const auto u16 = [&b, little](quint64 at) { return little ? b.le16(at) : b.be16(at); };
    const auto u32 = [&b, little](quint64 at) { return little ? b.le32(at) : b.be32(at); };
    l.add(0, 8, 0, tr("Header"), little ? tr("Little-endian") : tr("Big-endian"));
    QVector<QPair<quint64, QString>> folders{{u32(4), tr("Folder 1")}};
    QSet<quint64> seen;
    int number = 1;
    while (!folders.isEmpty() && !l.full() && seen.size() < 64) {
        const auto [at, name] = folders.takeFirst();
        if (at < 8 || seen.contains(at))
            continue;
        seen.insert(at);
        const quint32 count = u16(at);
        const quint64 size = 2 + quint64(count) * 12 + 4;
        if (count == 0 || count > 4096 || !b.has(at, size)) {
            l.add(at, 0, 0, name, {}, cutShort());
            continue;
        }
        quint64 width = 0, height = 0, strips = 0, stripBytes = 0, firstStrip = 0;
        for (quint32 i = 0; i < count; ++i) {
            const quint64 e = at + 2 + quint64(i) * 12;
            const quint32 tag = u16(e), kind = u16(e + 2), n = u32(e + 4);
            const quint32 value = kind == 3 && n == 1 ? u16(e + 8) : u32(e + 8);
            if (tag == 0x0100)
                width = value;
            else if (tag == 0x0101)
                height = value;
            else if (tag == 0x0111 || tag == 0x0144 || tag == 0x0201) {
                strips = n;
                firstStrip = n == 1 ? value : u32(value);
            } else if (tag == 0x0117 || tag == 0x0145 || tag == 0x0202) {
                for (quint32 k = 0; k < std::min<quint32>(n, 100000); ++k)
                    stripBytes += n == 1 ? value : (kind == 3 ? u16(value + 2ull * k) : u32(value + 4ull * k));
            } else if (tag == 0x8769) {
                folders << qMakePair(quint64(value), tr("Camera settings (EXIF)"));
            } else if (tag == 0x014A) {
                for (quint32 k = 0; k < std::min<quint32>(n, 16); ++k)
                    folders << qMakePair(quint64(n == 1 ? value : u32(value + 4ull * k)), tr("Folder %1").arg(++number));
            }
        }
        l.add(at, size, 0, name, width ? tr("%1 tags, %2 × %3 pixels").arg(count).arg(width).arg(height) : tr("%1 tags").arg(count));
        if (strips && stripBytes)
            l.add(firstStrip, stripBytes, 1, tr("Picture data"), QCoreApplication::translate("structure", "in %n piece(s)", nullptr, int(strips)),
                  firstStrip + stripBytes > b.n ? cutShort() : QString());
        if (const quint32 next = u32(at + 2 + quint64(count) * 12))
            folders << qMakePair(quint64(next), tr("Folder %1").arg(++number));
    }
}

// --- Compound files (Office 97-2003, Outlook): their streams

void compound(const QByteArray &data, List &l)
{
    l.add(0, 512, 0, tr("Header"), tr("Compound file"));
    const ole::File file(data);
    if (!file.valid()) {
        l.add(512, quint64(data.size()) - 512, 0, tr("Contents"), {}, tr("Its directory can't be read: damaged"));
        return;
    }
    const quint64 sector = 1ull << (quint8(data[30]) == 12 ? 12 : 9);
    for (const ole::File::Entry &e : file.entries()) {
        if (l.full())
            break;
        if (e.storage) {
            l.add(0, 0, int(e.path.count(QLatin1Char('/'))), e.path.section(QLatin1Char('/'), -1), tr("Folder"));
            continue;
        }
        const bool small = e.size < 4096;
        const quint64 got = quint64(file.stream(e.path).size());
        l.add(small ? 0 : (quint64(e.start) + 1) * sector, e.size, int(e.path.count(QLatin1Char('/'))), e.path.section(QLatin1Char('/'), -1),
              small ? tr("%1, in the small-stream area").arg(bytes(e.size)) : bytes(e.size),
              got < e.size ? tr("Only %1 of it can be read: damaged or cut short").arg(bytes(got)) : QString());
    }
}

// --- Matroska and WebM

void ebml(const Bytes &b, quint64 from, quint64 to, int depth, List &l)
{
    static const QHash<quint32, const char *> names = {
        {0x1A45DFA3, "EBML header"}, {0x18538067, "Segment"}, {0x114D9B74, "SeekHead"},  {0x1549A966, "Info"},
        {0x1654AE6B, "Tracks"},      {0xAE, "TrackEntry"},   {0x1F43B675, "Cluster"},   {0x1C53BB6B, "Cues"},
        {0x1254C367, "Tags"},        {0x1941A469, "Attachments"}, {0x1043A770, "Chapters"}, {0xEC, "Void"}};
    for (quint64 at = from; at < to && !l.full();) {
        const quint32 first = b.u8(at);
        int idLength = 1;
        while (idLength <= 4 && !(first & (0x80u >> (idLength - 1))))
            ++idLength;
        if (first == 0 || idLength > 4) {
            l.add(at, to - at, depth, tr("Unknown"), {}, tr("Damaged from here on"));
            return;
        }
        quint32 id = 0;
        for (int i = 0; i < idLength; ++i)
            id = id << 8 | b.u8(at + quint64(i));
        const quint64 s = at + quint64(idLength);
        const quint32 sizeFirst = b.u8(s);
        int sizeLength = 1;
        while (sizeLength <= 8 && !(sizeFirst & (0x80u >> (sizeLength - 1))))
            ++sizeLength;
        if (sizeFirst == 0 || sizeLength > 8) {
            l.add(at, to - at, depth, tr("Unknown"), {}, tr("Damaged from here on"));
            return;
        }
        quint64 size = sizeFirst & (0xFFu >> sizeLength);
        bool open = size == (0xFFu >> sizeLength);
        for (int i = 1; i < sizeLength; ++i) {
            size = size << 8 | b.u8(s + quint64(i));
            open = open && b.u8(s + quint64(i)) == 0xFF;
        }
        const quint64 body = s + quint64(sizeLength);
        const quint64 end = open ? to : body + size;
        const bool fits = end <= to;
        if (names.contains(id) || depth == 0)
            l.add(at, (fits ? end : to) - at, depth, names.contains(id) ? QString::fromLatin1(names.value(id)) : QStringLiteral("0x%1").arg(id, 0, 16),
                  open ? tr("Its size isn't said (a live recording)") : QString(), fits ? QString() : cutShort());
        if ((id == 0x18538067 || id == 0x1654AE6B) && depth < 3)
            ebml(b, body, std::min(end, to), depth + 1, l);
        if (!fits)
            return;
        at = end;
    }
}

// --- Ogg: pages, each with its own checksum

quint32 oggCrc(const uchar *p, quint64 n)
{
    static quint32 table[256];
    static bool made = false;
    if (!made) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 r = i << 24;
            for (int k = 0; k < 8; ++k)
                r = r & 0x80000000 ? (r << 1) ^ 0x04C11DB7 : r << 1;
            table[i] = r;
        }
        made = true;
    }
    quint32 crc = 0;
    for (quint64 i = 0; i < n; ++i)
        crc = (crc << 8) ^ table[((crc >> 24) ^ p[i]) & 0xFF];
    return crc;
}

void ogg(const Bytes &b, List &l)
{
    quint64 at = 0;
    int pages = 0;
    while (b.is(at, "OggS") && !l.full()) {
        const quint32 segments = b.u8(at + 26);
        quint64 body = 0;
        for (quint32 i = 0; i < segments; ++i)
            body += b.u8(at + 27 + i);
        const quint64 size = 27 + quint64(segments) + body;
        if (!b.has(at, size)) {
            l.add(at, b.n - at, 0, tr("Page %1").arg(++pages), {}, cutShort());
            return;
        }
        QByteArray page = b.mid(at, size);
        std::memset(page.data() + 22, 0, 4);
        const bool good = oggCrc(reinterpret_cast<const uchar *>(page.constData()), quint64(page.size())) == b.le32(at + 22);
        l.add(at, size, 0, tr("Page %1").arg(++pages), tr("Stream %1").arg(b.le32(at + 14), 8, 16, QLatin1Char('0')),
              good ? QString() : tr("Its checksum doesn't match: damaged"));
        at += size;
    }
    if (at < b.n)
        after(b, at, l);
}

// --- FLAC: its information blocks, then the sound

void flac(const Bytes &b, List &l)
{
    static const char *const kinds[] = {"STREAMINFO", "PADDING", "APPLICATION", "SEEKTABLE", "VORBIS_COMMENT", "CUESHEET", "PICTURE"};
    l.add(0, 4, 0, QStringLiteral("fLaC"));
    quint64 at = 4;
    for (int blocks = 0; blocks < 1000 && b.has(at, 4) && !l.full(); ++blocks) {
        const quint32 head = b.be32(at), kind = (head >> 24) & 0x7F, length = head & 0xFFFFFF;
        l.add(at, 4 + quint64(length), 0, kind < 7 ? QString::fromLatin1(kinds[kind]) : tr("Block %1").arg(kind), bytes(length),
              b.has(at + 4, length) ? QString() : cutShort());
        at += 4 + quint64(length);
        if (head & 0x80000000)
            break;
    }
    if (at < b.n)
        l.add(at, b.n - at, 0, tr("Sound"), bytes(b.n - at));
}

// --- Windows Media (ASF)

void asf(const Bytes &b, List &l)
{
    static const QHash<QByteArray, const char *> names = {
        {QByteArray("\x30\x26\xB2\x75\x8E\x66\xCF\x11\xA6\xD9\x00\xAA\x00\x62\xCE\x6C", 16), "Header"},
        {QByteArray("\x36\x26\xB2\x75\x8E\x66\xCF\x11\xA6\xD9\x00\xAA\x00\x62\xCE\x6C", 16), "Data"},
        {QByteArray("\xA1\xDC\xAB\x8C\x47\xA9\xCF\x11\x8E\xE4\x00\xC0\x0C\x20\x53\x65", 16), "File properties"},
        {QByteArray("\x91\x07\xDC\xB7\xB7\xA9\xCF\x11\x8E\xE6\x00\xC0\x0C\x20\x53\x65", 16), "Stream properties"},
        {QByteArray("\x33\x26\xB2\x75\x8E\x66\xCF\x11\xA6\xD9\x00\xAA\x00\x62\xCE\x6C", 16), "Title and author"},
        {QByteArray("\x40\xA4\xD0\xD2\x07\xE3\xD2\x11\x97\xF0\x00\xA0\xC9\x5E\xA8\x50", 16), "Tags"},
        {QByteArray("\x90\x08\x00\x33\xB1\xE5\xCF\x11\x89\xF4\x00\xA0\xC9\x03\x49\xCB", 16), "Index"},
        {QByteArray("\xD3\x29\xE2\xD6\xDA\x35\xD1\x11\x90\x34\x00\xA0\xC9\x03\x49\xBE", 16), "Index"}};
    const auto walk = [&](quint64 from, quint64 to, int depth, auto &self) -> void {
        for (quint64 at = from; at + 24 <= to && !l.full();) {
            const QByteArray guid = b.mid(at, 16);
            const quint64 size = b.le64(at + 16);
            if (size < 24) {
                l.add(at, to - at, depth, tr("Unknown"), {}, tr("Damaged from here on"));
                return;
            }
            const bool fits = at + size <= to;
            l.add(at, fits ? size : to - at, depth, names.contains(guid) ? tr(names.value(guid)) : tr("Object"), bytes(size), fits ? QString() : cutShort());
            if (depth == 0 && names.value(guid) == QByteArrayLiteral("Header"))
                self(at + 30, std::min(at + size, to), 1, self);
            if (!fits)
                return;
            at += size;
        }
    };
    walk(0, b.n, 0, walk);
}

// --- SQLite: its header and pages

void sqlite(const Bytes &b, List &l)
{
    quint64 pageSize = b.be16(16);
    if (pageSize == 1)
        pageSize = 65536;
    const quint64 pages = b.be32(28);
    l.add(0, 100, 0, tr("Header"), tr("%1 pages of %2").arg(pages).arg(bytes(pageSize)),
          pageSize < 512 || (pageSize & (pageSize - 1)) ? tr("Its page size is impossible: damaged") : QString());
    if (pageSize < 512 || (pageSize & (pageSize - 1)))
        return;
    const quint64 have = b.n / pageSize;
    for (quint64 i = 0; i < std::max(pages, have) && !l.full(); ++i) {
        const quint64 at = i * pageSize, head = i == 0 ? 100 : at;
        const quint32 kind = b.u8(head);
        const QString name = kind == 2 ? tr("Index page") : kind == 5 ? tr("Table page") : kind == 10 ? tr("Index rows") : kind == 13 ? tr("Rows")
                                                                                                                                     : tr("Other page");
        l.add(at, pageSize, 0, tr("Page %1").arg(i + 1), name, i >= have ? cutShort() : i >= pages ? tr("After the last page the header counts") : QString());
    }
}

// --- MP3: tags, and the frames between them

void mp3(const Bytes &b, List &l)
{
    quint64 at = 0;
    if (b.is(0, "ID3")) {
        const quint64 size = 10 + ((quint64(b.u8(6) & 0x7F) << 21) | (quint64(b.u8(7) & 0x7F) << 14) | (quint64(b.u8(8) & 0x7F) << 7) | (b.u8(9) & 0x7F));
        l.add(0, size, 0, tr("Tags (ID3v2)"), bytes(size), size > b.n ? cutShort() : QString());
        at = size;
    }
    const quint64 end = b.n >= 128 && b.is(b.n - 128, "TAG") ? b.n - 128 : b.n;
    l.add(at, end > at ? end - at : 0, 0, tr("Sound frames"), bytes(end > at ? end - at : 0));
    if (end < b.n)
        l.add(end, 128, 0, tr("Tags (ID3v1)"));
}

// --- gzip: each part, checked against its CRC

void gzip(const Bytes &b, List &l)
{
    quint64 at = 0;
    for (int parts = 1; b.u8(at) == 0x1F && b.u8(at + 1) == 0x8B && !l.full(); ++parts) {
        z_stream z{};
        quint64 p = at + 10;
        const quint32 flags = b.u8(at + 3);
        if (flags & 4)
            p += 2 + quint64(b.le16(p));
        for (const quint32 field : {8u, 16u}) {
            if (flags & field) {
                while (p < b.n && b.p[p] != 0)
                    ++p;
                ++p;
            }
        }
        if (flags & 2)
            p += 2;
        if (p >= b.n || inflateInit2(&z, -MAX_WBITS) != Z_OK)
            break;
        QByteArray out(64 * 1024, Qt::Uninitialized);
        quint32 crc = 0;
        quint64 size = 0;
        z.next_in = const_cast<Bytef *>(b.p + p);
        z.avail_in = uInt(std::min<quint64>(b.n - p, 0xFFFFFFFFu));
        int result = Z_OK;
        while (result == Z_OK) {
            z.next_out = reinterpret_cast<Bytef *>(out.data());
            z.avail_out = uInt(out.size());
            result = inflate(&z, Z_NO_FLUSH);
            const uInt made = uInt(out.size()) - z.avail_out;
            crc = quint32(::crc32(crc, reinterpret_cast<const Bytef *>(out.constData()), made));
            size += made;
        }
        const quint64 end = p + z.total_in;
        inflateEnd(&z);
        if (result != Z_STREAM_END) {
            l.add(at, b.n - at, 0, tr("Part %1").arg(parts), {}, tr("It can't be unpacked to its end: damaged or cut short"));
            return;
        }
        const bool good = b.le32(end) == crc && b.le32(end + 4) == quint32(size);
        l.add(at, end + 8 - at, 0, tr("Part %1").arg(parts), tr("%1 unpacked").arg(bytes(size)), good ? QString() : tr("Its checksum doesn't match: damaged"));
        at = end + 8;
    }
    after(b, at, l);
}

// --- Photoshop: its sections

void psd(const Bytes &b, List &l)
{
    l.add(0, 26, 0, tr("Header"), tr("%1 × %2 pixels, %3 channels").arg(b.be32(18)).arg(b.be32(14)).arg(b.be16(12)));
    quint64 at = 26;
    const QString names[] = {tr("Colours"), tr("Resources"), tr("Layers")};
    for (int i = 0; i < 3 && !l.full(); ++i) {
        const bool big = i == 2 && b.be16(4) == 2;
        const quint64 size = big ? b.be64(at) : b.be32(at);
        const quint64 header = big ? 8 : 4;
        l.add(at, header + size, 0, names[i], bytes(size), at + header + size > b.n ? cutShort() : QString());
        if (i == 1) {
            for (quint64 r = at + 4; r + 12 <= at + 4 + size && b.is(r, "8BIM") && !l.full();) {
                quint64 name = 1 + quint64(b.u8(r + 6));
                name += name & 1;
                const quint64 length = b.be32(r + 6 + name);
                l.add(r, 10 + name + length + (length & 1), 1, tr("Resource %1").arg(b.be16(r + 4)), bytes(length));
                r += 10 + name + length + (length & 1);
            }
        }
        at += header + size;
    }
    l.add(at, b.n > at ? b.n - at : 0, 0, tr("Picture data"), b.be16(at) == 1 ? tr("Packed row by row") : b.be16(at) == 0 ? tr("As it is") : tr("Zipped"));
}

// --- RAR: blocks, each header checked

void rar(const Bytes &b, List &l)
{
    if (b.u8(6) == 1) {
        l.add(0, 8, 0, tr("Signature"), QStringLiteral("RAR 5"));
        quint64 at = 8;
        while (b.has(at, 6) && !l.full()) {
            quint64 p = at + 4, size = 0, type = 0, flags = 0, data = 0, skip = 0;
            const auto number = [&b](quint64 &q, quint64 &v) {
                v = 0;
                for (int shift = 0; shift < 64 && b.has(q, 1); shift += 7) {
                    const quint32 c = b.u8(q++);
                    v |= quint64(c & 0x7F) << shift;
                    if (!(c & 0x80))
                        return true;
                }
                return false;
            };
            if (!number(p, size) || size == 0 || !b.has(p, size)) {
                l.add(at, b.n - at, 0, tr("Block"), {}, cutShort());
                return;
            }
            const quint64 headerEnd = p + size;
            const bool good = quint32(::crc32(0, b.p + at + 4, uInt(headerEnd - at - 4))) == b.le32(at);
            number(p, type);
            number(p, flags);
            if (flags & 1)
                number(p, skip);
            if (flags & 2)
                number(p, data);
            static const char *const kinds[] = {"", "Main header", "File", "Service", "Encryption", "End"};
            l.add(at, headerEnd + data - at, 0, type >= 1 && type <= 5 ? tr(kinds[type]) : tr("Block"), bytes(data),
                  !good ? tr("Its header's checksum doesn't match: damaged") : headerEnd + data > b.n ? cutShort() : QString());
            at = headerEnd + data;
            if (type == 5)
                break;
        }
        after(b, at, l);
        return;
    }
    l.add(0, 7, 0, tr("Signature"), QStringLiteral("RAR 4"));
    quint64 at = 7;
    while (b.has(at, 7) && !l.full()) {
        const quint32 type = b.u8(at + 2), flags = b.le16(at + 3), size = b.le16(at + 5);
        if (size < 7 || !b.has(at, size)) {
            l.add(at, b.n - at, 0, tr("Block"), {}, cutShort());
            return;
        }
        quint64 add = 0;
        if (type == 0x74 || (flags & 0x8000))
            add = b.le32(at + 7);
        const bool good = (::crc32(0, b.p + at + 2, uInt(size - 2)) & 0xFFFF) == b.le16(at);
        l.add(at, size + add, 0, type == 0x73 ? tr("Main header") : type == 0x74 ? tr("File") : type == 0x7B ? tr("End") : tr("Block"), bytes(add),
              good ? QString() : tr("Its header's checksum doesn't match: damaged"));
        at += size + add;
        if (type == 0x7B)
            break;
    }
    after(b, at, l);
}

// --- 7z: the signature header, the packed data, and the header at the end (each with a CRC)

void sevenZip(const Bytes &b, List &l)
{
    const quint64 next = b.le64(12), nextSize = b.le64(20);
    const bool start = quint32(::crc32(0, b.p + 12, 20)) == b.le32(8);
    l.add(0, 32, 0, tr("Signature header"), {}, start ? QString() : tr("Its checksum doesn't match: damaged"));
    l.add(32, next, 0, tr("Packed data"), bytes(next));
    const quint64 at = 32 + next;
    const bool fits = b.has(at, nextSize);
    const bool good = fits && quint32(::crc32(0, b.p + at, uInt(nextSize))) == b.le32(28);
    l.add(at, nextSize, 0, tr("Header"), tr("What's inside, and where"), !fits ? cutShort() : good ? QString() : tr("Its checksum doesn't match: damaged"));
    after(b, at + nextSize, l);
}

} // namespace

QVector<Part> parts(int type, const uchar *data, quint64 size)
{
    List l;
    const Bytes b{data, size};
    const QString id = type >= 0 && type < filetypes::types().size() ? filetypes::types()[type].id : QString();
    const auto any = [&id](std::initializer_list<const char *> ids) {
        for (const char *i : ids) {
            if (id == QLatin1String(i))
                return true;
        }
        return false;
    };
    if (size == 0)
        return {};
    if (any({"jpg"}) && b.u8(0) == 0xFF && b.u8(1) == 0xD8)
        jpeg(b, l);
    else if (any({"png"}) && b.is(0, "\x89PNG\r\n\x1a\n", 8))
        png(b, l);
    else if (any({"gif"}) && b.is(0, "GIF8"))
        gif(b, l);
    else if (any({"bmp"}) && b.is(0, "BM"))
        bmp(b, l);
    else if (any({"mp4", "mov", "3gp", "m4a", "heic", "avif", "cr3"}))
        bmff(b, l);
    else if (any({"wav", "avi", "webp"}) && b.is(0, "RIFF"))
        riff(b, 0, size, 0, l);
    else if (any({"zip", "docx", "xlsx", "pptx", "odt", "ods", "odp", "epub", "jar", "apk"}))
        zip(b, l);
    else if (any({"pdf"}))
        pdf(b, l);
    else if (any({"tif", "cr2", "nef", "arw", "dng", "orf", "rw2", "pef"}) && (b.is(0, "II") || b.is(0, "MM")))
        tiff(b, l);
    else if (any({"doc", "xls", "ppt", "msg", "ole"}))
        compound(QByteArray::fromRawData(reinterpret_cast<const char *>(data), qsizetype(std::min<quint64>(size, 1024ull * 1024 * 1024))), l);
    else if (any({"mkv", "webm"}))
        ebml(b, 0, size, 0, l);
    else if (any({"ogg", "ogv", "opus"}))
        ogg(b, l);
    else if (any({"flac"}) && b.is(0, "fLaC"))
        flac(b, l);
    else if (any({"wmv", "wma"}))
        asf(b, l);
    else if (any({"sqlite"}))
        sqlite(b, l);
    else if (any({"mp3"}))
        mp3(b, l);
    else if (any({"gz"}))
        gzip(b, l);
    else if (any({"psd"}) && b.is(0, "8BPS"))
        psd(b, l);
    else if (any({"rar"}) && b.is(0, "Rar!"))
        rar(b, l);
    else if (any({"7z"}) && b.is(0, "7z\xbc\xaf\x27\x1c", 6))
        sevenZip(b, l);
    if (l.out.isEmpty())
        l.add(0, size, 0, tr("Data"), bytes(size));
    // Parts that go past the end are cut to it.
    for (Part &p : l.out) {
        if (p.at > size)
            p.at = size;
        if (p.size > size - p.at)
            p.size = size - p.at;
    }
    return l.out;
}

} // namespace structure
