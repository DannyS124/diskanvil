// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "repair.h"

#include "filetypes.h"
#include "hostprocess.h"
#include "mp4fix.h"

#include <QByteArrayView>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtEndian>

#include <algorithm>
#include <cstring>

#include <zlib.h>

namespace repair {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("repair", text);
}

QString idOf(int type)
{
    return type >= 0 && type < filetypes::types().size() ? filetypes::types()[type].id : QString();
}

bool any(const QString &id, std::initializer_list<const char *> ids)
{
    for (const char *i : ids) {
        if (id == QLatin1String(i))
            return true;
    }
    return false;
}

// A file's bytes, mapped when that's possible.
class Input
{
public:
    bool open(const QString &path, QString *why)
    {
        m_file.setFileName(path);
        if (!m_file.open(QIODevice::ReadOnly)) {
            *why = m_file.errorString();
            return false;
        }
        n = quint64(m_file.size());
        p = n ? m_file.map(0, qint64(n)) : nullptr;
        if (!p && n) {
            m_memory = m_file.readAll();
            if (quint64(m_memory.size()) != n) {
                *why = tr("It couldn't be read whole");
                return false;
            }
            p = reinterpret_cast<const uchar *>(m_memory.constData());
        }
        return true;
    }
    const uchar *p = nullptr;
    quint64 n = 0;

private:
    QFile m_file;
    QByteArray m_memory;
};

struct Bytes {
    const uchar *p;
    quint64 n;
    bool has(quint64 at, quint64 len) const { return at <= n && len <= n - at; }
    quint32 u8(quint64 at) const { return has(at, 1) ? p[at] : 0; }
    quint32 be16(quint64 at) const { return has(at, 2) ? qFromBigEndian<quint16>(p + at) : 0; }
    quint32 be32(quint64 at) const { return has(at, 4) ? qFromBigEndian<quint32>(p + at) : 0; }
    quint32 le16(quint64 at) const { return has(at, 2) ? qFromLittleEndian<quint16>(p + at) : 0; }
    quint32 le32(quint64 at) const { return has(at, 4) ? qFromLittleEndian<quint32>(p + at) : 0; }
    bool is(quint64 at, const char *text, int len = -1) const
    {
        const quint64 l = len < 0 ? strlen(text) : quint64(len);
        return has(at, l) && std::memcmp(p + at, text, l) == 0;
    }
    quint64 find(quint64 from, QByteArrayView needle) const
    {
        if (from >= n)
            return n;
        const qsizetype hit = QByteArrayView(p + from, qsizetype(n - from)).indexOf(needle);
        return hit >= 0 ? from + quint64(hit) : n;
    }
};

QByteArray be32(quint32 v)
{
    QByteArray b(4, '\0');
    qToBigEndian(v, b.data());
    return b;
}

QByteArray le16(quint32 v)
{
    QByteArray b(2, '\0');
    qToLittleEndian(quint16(v), b.data());
    return b;
}

QByteArray le32(quint32 v)
{
    QByteArray b(4, '\0');
    qToLittleEndian(v, b.data());
    return b;
}

bool write(QIODevice &out, const uchar *p, quint64 n)
{
    for (quint64 done = 0; done < n;) {
        const qint64 piece = qint64(std::min<quint64>(n - done, 16 * 1024 * 1024));
        if (out.write(reinterpret_cast<const char *>(p + done), piece) != piece)
            return false;
        done += quint64(piece);
    }
    return true;
}

bool write(QIODevice &out, const QByteArray &b)
{
    return out.write(b) == b.size();
}

Result fail(const QString &why)
{
    Result r;
    r.why = why;
    return r;
}

// Unpacks deflate data from `p`: how much of it was the packed stream (0 when it wasn't one to
// its end), and the CRC-32 and size of what came out.
quint64 inflated(const uchar *p, quint64 n, bool zlibHeader, quint32 *crc, quint64 *size, QByteArray *keep = nullptr, quint64 most = 0)
{
    z_stream z{};
    if (inflateInit2(&z, zlibHeader ? MAX_WBITS : -MAX_WBITS) != Z_OK)
        return 0;
    QByteArray out(256 * 1024, Qt::Uninitialized);
    *crc = 0;
    *size = 0;
    int result = Z_OK;
    quint64 in = 0;
    while (result == Z_OK && in < n) {
        const uInt chunk = uInt(std::min<quint64>(n - in, 1u << 30));
        z.next_in = const_cast<Bytef *>(p + in);
        z.avail_in = chunk;
        while (result == Z_OK && (z.avail_in > 0 || z.avail_out == 0)) {
            z.next_out = reinterpret_cast<Bytef *>(out.data());
            z.avail_out = uInt(out.size());
            result = inflate(&z, Z_NO_FLUSH);
            const uInt made = uInt(out.size()) - z.avail_out;
            *crc = quint32(::crc32(*crc, reinterpret_cast<const Bytef *>(out.constData()), made));
            *size += made;
            if (keep && quint64(keep->size()) < most)
                keep->append(out.constData(), qsizetype(std::min<quint64>(made, most - quint64(keep->size()))));
            if (result == Z_BUF_ERROR && z.avail_in == 0)
                result = Z_OK; // wants more input
            if (made == 0 && z.avail_in == 0)
                break;
        }
        in += chunk - z.avail_in;
    }
    const quint64 used = z.total_in;
    inflateEnd(&z);
    return result == Z_STREAM_END ? used : 0;
}

// --- PNG

Result png(const Bytes &b, QIODevice &out)
{
    if (!b.is(0, "\x89PNG\r\n\x1a\n", 8))
        return fail(tr("It doesn't start like a PNG, so there's nothing to go by"));
    Result r;
    write(out, b.p, 8);
    quint64 pos = 8;
    int fixed = 0;
    bool end = false, cut = false;
    while (pos + 12 <= b.n) {
        const quint32 length = b.be32(pos);
        bool name = true;
        for (int i = 0; i < 4; ++i) {
            const quint32 c = b.u8(pos + 4 + quint64(i));
            name = name && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
        }
        if (!name)
            break; // not a chunk: damaged from here, or something else
        if (!b.has(pos + 8, quint64(length) + 4)) {
            cut = true;
            break;
        }
        const quint32 crc = filetypes::crc32(b.p + pos + 4, int(length + 4));
        if (crc != b.be32(pos + 8 + length))
            ++fixed;
        write(out, b.p + pos, 8 + quint64(length));
        write(out, be32(crc));
        pos += 12 + quint64(length);
        if (b.is(pos - 12 - length + 4, "IEND")) {
            end = true;
            break;
        }
    }
    if (!end) {
        write(out, QByteArray("\0\0\0\0IEND\xAE\x42\x60\x82", 12));
        r.done << (cut ? tr("Left out the last chunk, which was cut short, and added the end it was missing") : tr("Added the end it was missing"));
    }
    if (fixed)
        r.done << QCoreApplication::translate("repair", "Put %n checksum(s) right", nullptr, fixed);
    if (pos < b.n && end)
        r.done << tr("Left out %1 bytes after its end").arg(b.n - pos);
    if (r.done.isEmpty())
        return fail(tr("Its structure is fine: any damage is in the picture itself, which can't be put back"));
    r.ok = true;
    return r;
}

// --- JPEG

// Where its scan data starts (after the SOS segment), or 0 when its markers up to there aren't
// all there and readable.
quint64 jpegScan(const Bytes &b)
{
    if (!(b.u8(0) == 0xFF && b.u8(1) == 0xD8))
        return 0;
    bool frame = false, tables = false;
    for (quint64 pos = 2; pos + 4 <= b.n;) {
        if (b.u8(pos) != 0xFF)
            return 0;
        const quint32 m = b.u8(pos + 1);
        if (m == 0xFF) {
            ++pos;
            continue;
        }
        const quint32 length = b.be16(pos + 2);
        if (length < 2 || !b.has(pos + 2, length))
            return 0;
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC)
            frame = true;
        if (m == 0xDB)
            tables = true;
        pos += 2 + quint64(length);
        if (m == 0xDA)
            return frame && tables ? pos : 0;
    }
    return 0;
}

Result jpeg(const Bytes &b, const Bytes &reference, const QString &referenceName, QIODevice &out)
{
    Result r;
    quint64 scan = jpegScan(b);
    quint64 from = scan;
    if (scan == 0) {
        // Its start is lost or damaged: the start of a photo from the same camera, then this one's picture data.
        if (!reference.p) {
            r.needsReference = true;
            r.why = tr("Its start, where its size and tables are, is lost or damaged. A photo that opens, taken with the same camera at the "
                       "same size, can lend it its start");
            return r;
        }
        const quint64 lent = jpegScan(reference);
        if (lent == 0)
            return fail(tr("The photo picked to borrow from isn't a whole JPEG either"));
        // This one's own picture data: after its SOS when that's there, else all of it.
        const quint64 sos = b.find(0, QByteArrayView("\xFF\xDA", 2));
        from = sos < b.n ? sos + 2 + b.be16(sos + 2) : (b.u8(0) == 0xFF && b.u8(1) == 0xD8 ? 2 : 0);
        write(out, reference.p, lent);
        r.done << tr("Borrowed its start from \"%1\". Where the camera's settings differed, the picture shows shifted or in the wrong colours")
                      .arg(referenceName);
    } else {
        write(out, b.p, scan);
    }
    // The picture data, to its end marker.
    quint64 end = from;
    for (; end + 1 < b.n; ++end) {
        if (b.p[end] == 0xFF && b.p[end + 1] == 0xD9)
            break;
    }
    const bool hasEnd = end + 1 < b.n;
    write(out, b.p + from, (hasEnd ? end : b.n) - from);
    write(out, QByteArray("\xFF\xD9", 2));
    if (!hasEnd)
        r.done << tr("Added the end it was missing: what's after the cut shows grey");
    else if (end + 2 < b.n)
        r.done << tr("Left out %1 bytes after its end").arg(b.n - end - 2);
    if (r.done.isEmpty())
        return fail(tr("Its structure is fine: any damage is in the picture itself, which can't be put back"));
    r.ok = true;
    return r;
}

// --- ZIP, and Word, Excel, PowerPoint, OpenDocument and EPUB files

Result zip(const Bytes &b, QIODevice &out, const std::function<bool(int)> &progress)
{
    struct Entry {
        QByteArray header; // the local header, with its sizes and checksum filled in
        quint64 data = 0, packed = 0, unpacked = 0;
        quint32 crc = 0;
        QByteArray name;
        quint64 offset = 0; // in the new file
        bool damaged = false;
    };
    QVector<Entry> entries;
    int skipped = 0, damaged = 0;
    for (quint64 at = b.find(0, QByteArrayView("PK\x03\x04", 4)); at < b.n; at = b.find(at + 4, QByteArrayView("PK\x03\x04", 4))) {
        if (progress && !progress(int(at * 90 / b.n)))
            return fail(tr("Stopped"));
        const quint32 flags = b.le16(at + 6), method = b.le16(at + 8), nameLength = b.le16(at + 26), extra = b.le16(at + 28);
        quint32 crc = b.le32(at + 14);
        quint64 packed = b.le32(at + 18), unpacked = b.le32(at + 22);
        const quint64 data = at + 30 + nameLength + extra;
        if (!b.has(at, 30) || nameLength == 0 || !b.has(at + 30, quint64(nameLength) + extra) || (method != 0 && method != 8)
            || packed == 0xFFFFFFFF) {
            ++skipped;
            continue;
        }
        Entry e;
        e.name = QByteArray(reinterpret_cast<const char *>(b.p + at + 30), nameLength);
        quint64 after = 0;
        if (method == 8) {
            quint32 got = 0;
            quint64 size = 0;
            const quint64 used = inflated(b.p + data, b.n - data, false, &got, &size);
            if (used == 0) {
                ++skipped;
                continue;
            }
            e.damaged = !(flags & 8) && (got != crc || size != unpacked);
            packed = used;
            unpacked = size;
            crc = got;
            after = data + packed;
        } else {
            if (flags & 8) { // stored, its size only after it: up to the descriptor
                const quint64 descriptor = b.find(data, QByteArrayView("PK\x07\x08", 4));
                if (descriptor >= b.n) {
                    ++skipped;
                    continue;
                }
                packed = unpacked = descriptor - data;
            }
            if (!b.has(data, packed)) {
                ++skipped;
                continue;
            }
            const quint32 got = quint32(::crc32(0, b.p + data, uInt(packed)));
            e.damaged = !(flags & 8) && got != crc;
            crc = got;
            after = data + packed;
        }
        damaged += e.damaged;
        e.data = data;
        e.packed = packed;
        e.unpacked = unpacked;
        e.crc = crc;
        e.header = QByteArray(reinterpret_cast<const char *>(b.p + at), int(30 + nameLength + extra));
        qToLittleEndian(quint16(flags & ~8u), e.header.data() + 6);
        qToLittleEndian(crc, e.header.data() + 14);
        qToLittleEndian(quint32(packed), e.header.data() + 18);
        qToLittleEndian(quint32(unpacked), e.header.data() + 22);
        entries << e;
        at = after > 4 ? after - 4 : at; // the next one is looked for after this one's data
    }
    if (entries.isEmpty())
        return fail(tr("None of what was inside it could be found"));
    quint64 offset = 0;
    for (Entry &e : entries) {
        e.offset = offset;
        if (!write(out, e.header) || !write(out, b.p + e.data, e.packed))
            return fail(tr("The repaired copy couldn't be written"));
        offset += quint64(e.header.size()) + e.packed;
    }
    if (offset > 0xFFFFFFFFull)
        return fail(tr("It's too big to rebuild this way (over 4 GB)"));
    QByteArray directory;
    for (const Entry &e : std::as_const(entries)) {
        // Made by, then the local header's fields, then comment, disk, attributes and where it is.
        directory += le32(0x02014b50) + le16(20) + QByteArray(e.header.constData() + 4, 26) + le16(0) + le16(0) + le16(0) + le32(0) + le32(quint32(e.offset))
                   + e.name;
        // The local header's extra-field length was copied in; the directory's has none.
        qToLittleEndian(quint16(0), directory.data() + directory.size() - e.name.size() - 16);
    }
    const QByteArray end = le32(0x06054b50) + le16(0) + le16(0) + le16(quint32(entries.size())) + le16(quint32(entries.size()))
                         + le32(quint32(directory.size())) + le32(quint32(offset)) + le16(0);
    if (!write(out, directory) || !write(out, end))
        return fail(tr("The repaired copy couldn't be written"));
    Result r;
    r.ok = true;
    r.done << QCoreApplication::translate("repair", "Rebuilt the list of what's inside it: %n file(s)", nullptr, int(entries.size()));
    if (damaged)
        r.done << QCoreApplication::translate("repair", "%n of them don't match their checksum and may be damaged", nullptr, damaged);
    if (skipped)
        r.done << QCoreApplication::translate("repair", "%n couldn't be read and were left out", nullptr, skipped);
    return r;
}

// --- PDF: the table of where each object is, rebuilt from the objects

Result pdf(const Bytes &b, QIODevice &out, const std::function<bool(int)> &progress)
{
    if (!b.is(0, "%PDF-"))
        return fail(tr("It doesn't start like a PDF"));
    struct Where {
        quint64 at = 0;
        quint32 generation = 0;
        qint64 stream = -1; // in an object stream: its number
        quint32 index = 0;
    };
    QMap<quint32, Where> objects;
    QMap<quint32, quint64> streamsAt; // object streams, by number
    qint64 root = -1, info = -1;
    const auto isSpace = [](uchar c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '\f' || c == 0; };
    for (quint64 obj = b.find(0, " obj"); obj < b.n; obj = b.find(obj + 4, " obj")) {
        if (progress && !progress(int(obj * 80 / b.n)))
            return fail(tr("Stopped"));
        if (b.has(obj + 4, 1) && !isSpace(b.p[obj + 4]) && b.p[obj + 4] != '<' && b.p[obj + 4] != '[')
            continue;
        // "12 0 obj": the generation, then the number, before it.
        quint64 s = obj, d = s;
        while (d > 0 && b.p[d - 1] >= '0' && b.p[d - 1] <= '9' && s - d < 6)
            --d;
        if (d == s || d == 0 || b.p[d - 1] != ' ')
            continue;
        const quint32 generation = QByteArray(reinterpret_cast<const char *>(b.p + d), int(s - d)).toUInt();
        s = d - 1;
        d = s;
        while (d > 0 && b.p[d - 1] >= '0' && b.p[d - 1] <= '9' && s - d < 10)
            --d;
        if (d == s || (d > 0 && !isSpace(b.p[d - 1])))
            continue;
        const quint32 number = QByteArray(reinterpret_cast<const char *>(b.p + d), int(s - d)).toUInt();
        if (number == 0 || number > 8000000)
            continue;
        objects[number] = {d, generation, -1, 0}; // a later one (an update) replaces an earlier
        const QByteArray head(reinterpret_cast<const char *>(b.p + obj), int(std::min<quint64>(b.n - obj, 1024)));
        const QByteArray flat = QByteArray(head).replace(' ', "").replace('\n', "").replace('\r', "");
        if (flat.contains("/Type/Catalog"))
            root = number;
        if (flat.contains("/Producer") || flat.contains("/Creator"))
            info = flat.contains("/Type") ? info : number;
        if (flat.contains("/Type/ObjStm"))
            streamsAt[number] = obj;
    }
    // The objects packed inside object streams (PDF 1.5 and later): the stream's start lists them.
    int packed = 0;
    for (auto it = streamsAt.cbegin(); it != streamsAt.cend(); ++it) {
        const quint64 obj = it.value();
        const QByteArray head(reinterpret_cast<const char *>(b.p + obj), int(std::min<quint64>(b.n - obj, 2048)));
        const auto number = [&head](const char *key) {
            const qsizetype at = head.indexOf(key);
            return at < 0 ? -1 : head.mid(at + qsizetype(strlen(key)), 16).trimmed().split(' ').first().split('/').first().split('>').first().toInt();
        };
        const int count = number("/N"), first = number("/First");
        const qsizetype streamWord = head.indexOf("stream");
        if (count <= 0 || count > 100000 || first < 0 || streamWord < 0)
            continue;
        quint64 data = obj + quint64(streamWord) + 6;
        if (b.u8(data) == '\r')
            ++data;
        if (b.u8(data) == '\n')
            ++data;
        const quint64 end = b.find(data, "endstream");
        if (end >= b.n)
            continue;
        QByteArray list;
        quint32 crc = 0;
        quint64 size = 0;
        if (head.contains("/FlateDecode"))
            inflated(b.p + data, end - data, true, &crc, &size, &list, quint64(first) + 64);
        else
            list = QByteArray(reinterpret_cast<const char *>(b.p + data), int(std::min<quint64>(end - data, quint64(first))));
        const QList<QByteArray> numbers = list.left(first).simplified().split(' ');
        QByteArray all;
        if (head.contains("/FlateDecode"))
            inflated(b.p + data, end - data, true, &crc, &size, &all, 64 * 1024 * 1024);
        for (int k = 0; k + 1 < numbers.size() && k / 2 < count; k += 2) {
            const quint32 inside = numbers[k].toUInt();
            if (inside == 0 || inside > 8000000 || (objects.contains(inside) && objects[inside].stream < 0))
                continue; // one written out on its own takes its place
            objects[inside] = {0, 0, qint64(it.key()), quint32(k / 2)};
            ++packed;
            // The catalog can be in here too.
            const qsizetype from = first + numbers[k + 1].toInt();
            const QByteArray obj = all.mid(from, 512).replace(' ', "").replace('\n', "").replace('\r', "");
            if (obj.contains("/Type/Catalog"))
                root = inside;
        }
    }
    if (objects.isEmpty() || root < 0)
        return fail(tr("Its objects, or the one that says where its pages are, couldn't be found"));
    if (!write(out, b.p, b.n))
        return fail(tr("The repaired copy couldn't be written"));
    const quint32 last = objects.lastKey();
    QByteArray tail = "\n";
    const quint64 tableAt = b.n + 1;
    if (packed == 0) {
        // A table of the usual kind.
        tail += "xref\n0 " + QByteArray::number(last + 1) + "\n0000000000 65535 f \n";
        for (quint32 i = 1; i <= last; ++i) {
            const auto it = objects.constFind(i);
            tail += it == objects.cend() ? QByteArray("0000000000 65535 f \n")
                                         : QByteArray::number(it->at).rightJustified(10, '0') + ' ' + QByteArray::number(it->generation).rightJustified(5, '0') + " n \n";
        }
        tail += "trailer\n<< /Size " + QByteArray::number(last + 1) + " /Root " + QByteArray::number(root) + " 0 R";
        if (info > 0)
            tail += " /Info " + QByteArray::number(info) + " 0 R";
        tail += " >>\nstartxref\n" + QByteArray::number(tableAt) + "\n%%EOF\n";
    } else {
        // Objects in object streams can only be listed in a cross-reference stream.
        const quint32 self = last + 1;
        QByteArray rows;
        const auto row = [&rows](quint32 type, quint64 field2, quint32 field3) {
            rows += char(type);
            for (int k = 7; k >= 0; --k)
                rows += char(field2 >> (8 * k));
            rows += char(field3 >> 8);
            rows += char(field3);
        };
        row(0, 0, 0xFFFF);
        for (quint32 i = 1; i <= last; ++i) {
            const auto it = objects.constFind(i);
            if (it == objects.cend())
                row(0, 0, 0);
            else if (it->stream >= 0)
                row(2, quint64(it->stream), it->index);
            else
                row(1, it->at, it->generation);
        }
        row(1, tableAt, 0);
        tail += QByteArray::number(self) + " 0 obj\n<< /Type /XRef /Size " + QByteArray::number(self + 1) + " /W [1 8 2] /Root " + QByteArray::number(root) + " 0 R";
        if (info > 0)
            tail += " /Info " + QByteArray::number(info) + " 0 R";
        tail += " /Length " + QByteArray::number(rows.size()) + " >>\nstream\n" + rows + "\nendstream\nendobj\nstartxref\n" + QByteArray::number(tableAt)
              + "\n%%EOF\n";
    }
    if (!write(out, tail))
        return fail(tr("The repaired copy couldn't be written"));
    Result r;
    r.ok = true;
    r.done << QCoreApplication::translate("repair", "Rebuilt the table that says where its %n object(s) are", nullptr, int(objects.size()));
    if (packed)
        r.done << QCoreApplication::translate("repair", "%n of them are packed inside object streams", nullptr, packed);
    return r;
}

// --- gzip: what still unpacks, packed again

Result gzip(const Bytes &b, QIODevice &out)
{
    if (!(b.u8(0) == 0x1F && b.u8(1) == 0x8B && b.u8(2) == 8))
        return fail(tr("It doesn't start like a gzip file"));
    quint64 p = 10;
    const quint32 flags = b.u8(3);
    if (flags & 4)
        p += 2 + quint64(b.le16(p));
    QByteArray name;
    for (const quint32 field : {8u, 16u}) {
        if (!(flags & field))
            continue;
        const quint64 zero = b.find(p, QByteArrayView("\0", 1));
        if (field == 8)
            name = QByteArray(reinterpret_cast<const char *>(b.p + p), int(std::min<quint64>(zero - p, 1024)));
        p = zero + 1;
    }
    if (flags & 2)
        p += 2;
    if (p >= b.n)
        return fail(tr("It's cut short before anything in it"));
    // Unpacked as far as it goes.
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
        return fail(tr("It can't be unpacked"));
    z_stream pack{};
    deflateInit2(&pack, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    QByteArray header("\x1F\x8B\x08\x00\x00\x00\x00\x00\x00\xFF", 10);
    if (!name.isEmpty()) {
        header[3] = 8;
        header += name + '\0';
    }
    write(out, header);
    QByteArray buffer(256 * 1024, Qt::Uninitialized), packedOut(512 * 1024, Qt::Uninitialized);
    quint32 crc = 0;
    quint64 size = 0;
    z.next_in = const_cast<Bytef *>(b.p + p);
    z.avail_in = uInt(std::min<quint64>(b.n - p, 0xFFFFFFFFu));
    int result = Z_OK;
    while (result == Z_OK) {
        z.next_out = reinterpret_cast<Bytef *>(buffer.data());
        z.avail_out = uInt(buffer.size());
        result = inflate(&z, Z_NO_FLUSH);
        const uInt made = uInt(buffer.size()) - z.avail_out;
        if (made == 0 && result != Z_STREAM_END && z.avail_in == 0)
            break;
        crc = quint32(::crc32(crc, reinterpret_cast<const Bytef *>(buffer.constData()), made));
        size += made;
        pack.next_in = reinterpret_cast<Bytef *>(buffer.data());
        pack.avail_in = made;
        do {
            pack.next_out = reinterpret_cast<Bytef *>(packedOut.data());
            pack.avail_out = uInt(packedOut.size());
            deflate(&pack, Z_NO_FLUSH);
            write(out, reinterpret_cast<const uchar *>(packedOut.constData()), quint64(packedOut.size()) - pack.avail_out);
        } while (pack.avail_out == 0);
    }
    const bool whole = result == Z_STREAM_END;
    const bool checks = whole && b.le32(p + z.total_in) == crc && b.le32(p + z.total_in + 4) == quint32(size);
    inflateEnd(&z);
    int finish = Z_OK;
    do {
        pack.next_out = reinterpret_cast<Bytef *>(packedOut.data());
        pack.avail_out = uInt(packedOut.size());
        finish = deflate(&pack, Z_FINISH);
        write(out, reinterpret_cast<const uchar *>(packedOut.constData()), quint64(packedOut.size()) - pack.avail_out);
    } while (finish == Z_OK);
    deflateEnd(&pack);
    write(out, le32(crc) + le32(quint32(size)));
    if (checks)
        return fail(tr("It's fine: it unpacks whole and its checksum matches"));
    if (size == 0)
        return fail(tr("Nothing in it could be unpacked"));
    Result r;
    r.ok = true;
    r.done << tr("Kept the %1 bytes that could still be unpacked, packed again whole").arg(size);
    return r;
}

// --- WAV and AVI: sizes put right to what's there

Result riff(const Bytes &b, QFileDevice &out)
{
    if (!b.is(0, "RIFF") || b.n < 12)
        return fail(tr("It doesn't start like a WAV or AVI file"));
    if (!write(out, b.p, b.n))
        return fail(tr("The repaired copy couldn't be written"));
    int fixed = 0;
    const auto put = [&out, &fixed](quint64 at, quint32 value) {
        out.seek(qint64(at));
        out.write(le32(value));
        ++fixed;
    };
    if (b.le32(4) != quint32(std::min<quint64>(b.n - 8, 0xFFFFFFFFu)))
        put(4, quint32(std::min<quint64>(b.n - 8, 0xFFFFFFFFu)));
    // Each chunk that runs past the end, and the chunks inside lists, to what's there.
    const auto walk = [&](quint64 from, quint64 to, auto &self) -> void {
        for (quint64 at = from; at + 8 <= to;) {
            const quint64 size = b.le32(at + 4);
            if (at + 8 + size > to) {
                put(at + 4, quint32(to - at - 8));
                if (b.is(at, "LIST"))
                    self(at + 12, to, self);
                return;
            }
            if (b.is(at, "LIST"))
                self(at + 12, at + 8 + size, self);
            at += 8 + size + (size & 1);
        }
    };
    walk(12, b.n, walk);
    out.seek(qint64(b.n));
    if (fixed == 0)
        return fail(tr("Its sizes are right already"));
    Result r;
    r.ok = true;
    r.done << tr("Put its sizes right to what's there: it was cut off while it was being made");
    return r;
}

// --- SQLite: sqlite3's own .recover, into a new database

} // namespace

// sqlite3 runs where DiskAnvil does, not on the host: the Flatpak's runtime has its own, and the
// files it works on are where this process sees them. Windows' comes with DiskAnvil.
QString sqlite3()
{
#ifdef Q_OS_WIN
    const QString program = host::windowsProgram(QStringLiteral("sqlite3"));
    return program == QLatin1String("sqlite3") ? QString() : program;
#else
    return QStandardPaths::findExecutable(QStringLiteral("sqlite3"));
#endif
}

namespace {

Result sqlite(const QString &in, const QString &out)
{
    const QString program = sqlite3();
    if (program.isEmpty())
        return fail(tr("This needs the sqlite3 program, which isn't on this computer"));
    const QString sql = out + QStringLiteral(".sql");
    QFile::remove(out);
    const int recovered = QProcess::execute(program, {in, QStringLiteral(".output ") + sql, QStringLiteral(".recover")});
    const int read = recovered == 0 && QFileInfo(sql).size() > 0 ? QProcess::execute(program, {out, QStringLiteral(".read ") + sql}) : -1;
    QFile::remove(sql);
    if (read != 0 || !QFileInfo::exists(out))
        return fail(tr("sqlite3 couldn't find anything to recover in it"));
    Result r;
    r.ok = true;
    r.done << tr("Copied the tables and rows sqlite3 could still find into a new database");
    return r;
}

} // namespace

bool knows(int type)
{
    return !what(type).isEmpty();
}

QString what(int type)
{
    const QString id = idOf(type);
    if (id == QLatin1String("png"))
        return tr("Puts its checksums right and its end back");
    if (id == QLatin1String("jpg"))
        return tr("Puts its end back, or borrows a lost start from another photo from the same camera");
    if (any(id, {"zip", "docx", "xlsx", "pptx", "odt", "ods", "odp", "epub", "jar", "apk"}))
        return tr("Rebuilds the list of what's inside it from the entries themselves");
    if (id == QLatin1String("pdf"))
        return tr("Rebuilds the table that says where everything in it is");
    if (id == QLatin1String("gz"))
        return tr("Keeps what still unpacks, packed again whole");
    if (any(id, {"wav", "avi"}))
        return tr("Puts its sizes right, for a recording that was cut off");
    if (id == QLatin1String("sqlite"))
        return tr("Copies what can still be read into a new database (with sqlite3)");
    if (any(id, {"mp4", "mov", "3gp", "m4a"}))
        return tr("Rebuilds the index of its frames, lost when a recording stopped, from a clip that plays");
    return {};
}

QString referenceWanted(int type)
{
    const QString id = idOf(type);
    if (id == QLatin1String("jpg"))
        return tr("Pick a Photo That Opens, From the Same Camera");
    if (any(id, {"mp4", "mov", "3gp", "m4a"}))
        return tr("Pick a Clip That Plays, Made With the Same Camera and Settings");
    return {};
}

Result run(int type, const QString &in, const QString &out, const QString &reference, const std::function<bool(int)> &progress)
{
    const QString id = idOf(type);
    if (!knows(type))
        return fail(tr("This kind of file can't be repaired here"));
    if (id == QLatin1String("sqlite"))
        return sqlite(in, out);
    Input input, lent;
    QString why;
    if (!input.open(in, &why))
        return fail(tr("It couldn't be read: %1").arg(why));
    if (!reference.isEmpty() && !lent.open(reference, &why))
        return fail(tr("The file to borrow from couldn't be read: %1").arg(why));
    const Bytes b{input.p, input.n}, ref{lent.p, lent.n};
    QSaveFile file(out);
    if (!file.open(QIODevice::WriteOnly))
        return fail(tr("The repaired copy couldn't be made: %1").arg(file.errorString()));
    Result r;
    if (id == QLatin1String("png"))
        r = png(b, file);
    else if (id == QLatin1String("jpg"))
        r = jpeg(b, ref, QFileInfo(reference).fileName(), file);
    else if (any(id, {"zip", "docx", "xlsx", "pptx", "odt", "ods", "odp", "epub", "jar", "apk"}))
        r = zip(b, file, progress);
    else if (id == QLatin1String("pdf"))
        r = pdf(b, file, progress);
    else if (id == QLatin1String("gz"))
        r = gzip(b, file);
    else if (any(id, {"wav", "avi"}))
        r = riff(b, file);
    else if (any(id, {"mp4", "mov", "3gp", "m4a"})) {
        if (!lent.p) {
            r.needsReference = true;
            r.why = tr("Rebuilding its index needs a clip that plays, made with the same camera and settings, to learn how it stores its frames");
        } else {
            const mp4fix::Fixed fixed = mp4fix::rebuild(input.p, input.n, lent.p, lent.n, file, progress);
            r.ok = fixed.ok;
            r.done = fixed.done;
            r.why = fixed.why;
        }
    }
    if (!r.ok) {
        file.cancelWriting();
        return r;
    }
    if (!file.commit())
        return fail(tr("The repaired copy couldn't be written: %1").arg(file.errorString()));
    return r;
}

QVector<qint64> pdfVersions(const QByteArray &pdf)
{
    QVector<qint64> ends;
    if (!pdf.startsWith("%PDF-"))
        return ends;
    // Each version ends with "startxref", where its table is, and "%%EOF".
    for (qsizetype at = pdf.indexOf("%%EOF"); at >= 0; at = pdf.indexOf("%%EOF", at + 5)) {
        const qsizetype from = std::max<qsizetype>(0, at - 1024);
        if (pdf.lastIndexOf("startxref", at) < from)
            continue;
        qsizetype end = at + 5;
        while (end < pdf.size() && (pdf[end] == '\r' || pdf[end] == '\n') && end < at + 7)
            ++end;
        ends << end;
    }
    // A PDF made for the web ("linearized") has a small table for its first page, ended the same
    // way: that isn't a version of its own.
    if (ends.size() > 1 && pdf.left(1024).contains("/Linearized"))
        ends.removeFirst();
    return ends;
}

} // namespace repair
