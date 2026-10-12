// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "naming.h"

#include "exif.h"
#include "filetypes.h"
#include "mediainfo.h"
#include "ole.h"
#include "textkinds.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QTimeZone>
#include <QXmlStreamReader>

#include <cstring>
#include <QtEndian>

#include <zlib.h>

namespace naming {

namespace {

constexpr qint64 kMiB = 1024 * 1024;

QString stamp(const QDateTime &t)
{
    return t.toString(QStringLiteral("yyyy-MM-dd HH.mm.ss"));
}

qint64 msecs(const QDateTime &t)
{
    return t.isValid() && t.date().year() > 1980 ? t.toMSecsSinceEpoch() : 0;
}

// A name from a title or the first words: one line, no more than 80 characters.
QString shortened(QString text)
{
    text = text.section(QLatin1Char('\n'), 0, 0).simplified();
    if (text.size() > 80) {
        text.truncate(80);
        const qsizetype space = text.lastIndexOf(QLatin1Char(' '));
        if (space > 40)
            text.truncate(space);
    }
    return text;
}

// --- PDF: the Info dictionary's /Title and /CreationDate, in the open at the end (or start)

QString pdfString(const QByteArray &d, qsizetype at)
{
    while (at < d.size() && (d[at] == ' ' || d[at] == '\r' || d[at] == '\n'))
        ++at;
    QByteArray raw;
    if (at < d.size() && d[at] == '(') {
        int depth = 1;
        for (++at; at < d.size() && depth > 0 && raw.size() < 2048; ++at) {
            const char c = d[at];
            if (c == '\\' && at + 1 < d.size()) {
                const char e = d[++at];
                if (e >= '0' && e <= '7') { // up to three octal digits
                    int v = e - '0';
                    for (int k = 0; k < 2 && at + 1 < d.size() && d[at + 1] >= '0' && d[at + 1] <= '7'; ++k)
                        v = v * 8 + (d[++at] - '0');
                    raw += char(v);
                } else {
                    raw += e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
                }
                continue;
            }
            if (c == '(')
                ++depth;
            else if (c == ')' && --depth == 0)
                break;
            raw += c;
        }
    } else if (at < d.size() && d[at] == '<') {
        const qsizetype end = d.indexOf('>', at);
        if (end < 0 || end - at > 4096)
            return {};
        raw = QByteArray::fromHex(d.mid(at + 1, end - at - 1));
    } else {
        return {};
    }
    if (raw.startsWith("\xFE\xFF")) { // UTF-16, big-endian
        QString out;
        for (qsizetype i = 2; i + 1 < raw.size(); i += 2)
            out += QChar(char16_t(quint8(raw[i]) << 8 | quint8(raw[i + 1])));
        return out;
    }
    if (raw.startsWith("\xEF\xBB\xBF"))
        return QString::fromUtf8(raw.mid(3));
    return QString::fromLatin1(raw);
}

QDateTime pdfDate(const QString &s)
{
    // D:YYYYMMDDHHmmSS, then Z or +HH'mm' (parts may be left off from the end)
    static const QRegularExpression date(QStringLiteral("^D?:?(\\d{4})(\\d{2})?(\\d{2})?(\\d{2})?(\\d{2})?(\\d{2})?([Z+-])?(\\d{2})?'?(\\d{2})?"));
    const QRegularExpressionMatch m = date.match(s.trimmed());
    if (!m.hasMatch())
        return {};
    const auto part = [&m](int i, int fallback) { return m.captured(i).isEmpty() ? fallback : m.captured(i).toInt(); };
    QDateTime t(QDate(part(1, 0), part(2, 1), part(3, 1)), QTime(part(4, 0), part(5, 0), part(6, 0)));
    if (!m.captured(7).isEmpty()) {
        const int offset = (part(8, 0) * 60 + part(9, 0)) * 60 * (m.captured(7) == QLatin1String("-") ? -1 : 1);
        t.setTimeZone(m.captured(7) == QLatin1String("Z") ? QTimeZone::UTC : QTimeZone::fromSecondsAheadOfUtc(offset));
    }
    return t;
}

Inside pdf(quint64 size, const Read &read)
{
    Inside out;
    const qint64 part = 256 * 1024;
    for (const QByteArray &d : {read(size > quint64(part) ? size - quint64(part) : 0, part), read(0, part)}) {
        const qsizetype title = d.lastIndexOf("/Title");
        const qsizetype created = d.lastIndexOf("/CreationDate");
        if (out.name.isEmpty() && title >= 0)
            out.name = shortened(pdfString(d, title + 6));
        if (out.when == 0 && created >= 0)
            out.when = msecs(pdfDate(pdfString(d, created + 13)));
        if (!out.name.isEmpty())
            break;
    }
    return out;
}

// --- Office Open XML and OpenDocument: the ZIP's own summary file (docProps/core.xml, meta.xml)

QByteArray zipMember(quint64 size, const Read &read, const QByteArray &wanted)
{
    const qint64 tailSize = qint64(std::min<quint64>(size, 64 * 1024 + 22));
    const QByteArray tail = read(size - quint64(tailSize), tailSize);
    const qsizetype eocd = tail.lastIndexOf("PK\x05\x06");
    if (eocd < 0 || eocd + 22 > tail.size())
        return {};
    const auto le16 = [](const QByteArray &b, qsizetype at) { return at + 2 <= b.size() ? qFromLittleEndian<quint16>(b.constData() + at) : 0u; };
    const auto le32 = [](const QByteArray &b, qsizetype at) { return at + 4 <= b.size() ? qFromLittleEndian<quint32>(b.constData() + at) : 0u; };
    const quint32 dirSize = le32(tail, eocd + 12), dirAt = le32(tail, eocd + 16);
    if (dirSize > 16 * kMiB || quint64(dirAt) + dirSize > size)
        return {};
    const QByteArray dir = read(dirAt, dirSize);
    for (qsizetype at = 0; at + 46 <= dir.size() && le32(dir, at) == 0x02014b50;) {
        const quint32 method = le16(dir, at + 10), packed = le32(dir, at + 20), unpacked = le32(dir, at + 24);
        const quint32 nameLength = le16(dir, at + 28), extra = le16(dir, at + 30), comment = le16(dir, at + 32), local = le32(dir, at + 42);
        if (dir.mid(at + 46, nameLength) == wanted && unpacked <= kMiB && packed <= 4 * kMiB) {
            const QByteArray head = read(local, 30);
            if (le32(head, 0) != 0x04034b50)
                return {};
            const QByteArray data = read(local + 30 + le16(head, 26) + le16(head, 28), packed);
            if (method == 0)
                return data;
            if (method != 8)
                return {};
            QByteArray out(qsizetype(unpacked), Qt::Uninitialized);
            z_stream z{};
            if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
                return {};
            z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
            z.avail_in = uInt(data.size());
            z.next_out = reinterpret_cast<Bytef *>(out.data());
            z.avail_out = uInt(out.size());
            const int result = inflate(&z, Z_FINISH);
            out.truncate(qsizetype(z.total_out));
            inflateEnd(&z);
            return result == Z_STREAM_END ? out : QByteArray();
        }
        at += 46 + qsizetype(nameLength) + extra + comment;
    }
    return {};
}

Inside officeZip(const QString &id, quint64 size, const Read &read)
{
    const bool odf = id.startsWith(QLatin1String("od"));
    const QByteArray xml = zipMember(size, read, odf ? QByteArrayLiteral("meta.xml") : QByteArrayLiteral("docProps/core.xml"));
    Inside out;
    QXmlStreamReader r(xml);
    r.setNamespaceProcessing(true);
    QString current;
    while (!r.atEnd()) {
        const auto token = r.readNext();
        if (token == QXmlStreamReader::StartElement) {
            current = r.name().toString();
        } else if (token == QXmlStreamReader::Characters && !r.isWhitespace()) {
            if (current == QLatin1String("title") && out.name.isEmpty())
                out.name = shortened(r.text().toString());
            else if ((current == QLatin1String("created") || current == QLatin1String("creation-date")) && out.when == 0)
                out.when = msecs(QDateTime::fromString(r.text().toString().trimmed().left(19), Qt::ISODate));
        } else if (token == QXmlStreamReader::EndElement) {
            current.clear();
        }
    }
    return out;
}

} // namespace

Inside fromContents(int type, quint64 size, const Read &read)
{
    Inside out;
    if (type < 0 || type >= filetypes::types().size() || size == 0)
        return out;
    const filetypes::Type &t = filetypes::types()[type];
    const QString &id = t.id;
    if (t.category == filetypes::Category::Picture && id != QLatin1String("svg") && id != QLatin1String("psd")) {
        const exif::Info info = exif::read(read(0, qint64(std::min<quint64>(size, kMiB))));
        if (info.taken.isValid()) {
            const QString camera = info.model.startsWith(info.make, Qt::CaseInsensitive) || info.make.isEmpty() ? info.model
                                 : info.model.isEmpty() ? info.make : info.make + QLatin1Char(' ') + info.model;
            out.name = (stamp(info.taken) + QLatin1Char(' ') + camera.simplified()).trimmed();
            out.when = msecs(info.taken);
        }
        return out;
    }
    if (t.category == filetypes::Category::Music || t.category == filetypes::Category::Video) {
        const mediainfo::Info info = mediainfo::read(type, read(0, qint64(std::min<quint64>(size, 8 * kMiB))), size);
        if (!info.artist.isEmpty() && !info.title.isEmpty())
            out.name = shortened(info.artist + QStringLiteral(" - ") + info.title);
        else if (!info.title.isEmpty())
            out.name = shortened(info.title);
        else if (info.recorded.isValid())
            out.name = stamp(info.recorded.toLocalTime());
        out.when = msecs(info.recorded);
        return out;
    }
    if (id == QLatin1String("doc") || id == QLatin1String("xls") || id == QLatin1String("ppt") || id == QLatin1String("msg")) {
        if (size > 32 * quint64(kMiB))
            return out;
        const ole::File file(read(0, qint64(size)));
        const ole::Summary s = ole::summary(file);
        out.name = shortened(!s.title.isEmpty() ? s.title : !s.subject.isEmpty() ? s.subject : ole::text(file, 200));
        out.when = msecs(s.created);
        return out;
    }
    if (id == QLatin1String("eml") || id == QLatin1String("rtf") || id == QLatin1String("html")) {
        const QByteArray head = read(0, qint64(std::min<quint64>(size, kMiB)));
        const texts::Heading h = texts::heading(type, head);
        out.name = shortened(!h.title.isEmpty() || id != QLatin1String("rtf") ? h.title : texts::text(type, head, 200));
        out.when = msecs(h.when);
        return out;
    }
    if (id == QLatin1String("pdf"))
        return pdf(size, read);
    if (id == QLatin1String("docx") || id == QLatin1String("xlsx") || id == QLatin1String("pptx") || id == QLatin1String("odt")
        || id == QLatin1String("ods") || id == QLatin1String("odp"))
        return officeZip(id, size, read);
    return out;
}

Spot where(int type, quint64 size, const Read &read)
{
    Spot spot;
    if (type < 0 || type >= filetypes::types().size() || size == 0)
        return spot;
    const filetypes::Type &t = filetypes::types()[type];
    if (t.category == filetypes::Category::Picture) {
        // EXIF is near the start: a JPEG's first segments, a HEIC's or a raw file's first megabytes.
        const exif::Info info = exif::read(read(0, qint64(std::min<quint64>(size, 2 * kMiB))));
        if (info.hasLocation) {
            spot = {true, info.latitude, info.longitude, info.taken.isValid() ? info.taken.toMSecsSinceEpoch() : 0};
        }
    } else if (t.category == filetypes::Category::Video && (t.id == QLatin1String("mp4") || t.id == QLatin1String("mov") || t.id == QLatin1String("3gp"))) {
        const QByteArray moov = mediainfo::mp4Moov([&read](quint64 at, char *buf, qint64 len) {
            const QByteArray part = read(at, len);
            memcpy(buf, part.constData(), size_t(part.size()));
            return qint64(part.size());
        }, size);
        const mediainfo::Info info = mediainfo::read(type, moov, size);
        if (info.hasLocation) {
            const QDateTime when = info.made.isValid() ? info.made : info.recorded;
            spot = {true, info.latitude, info.longitude, when.isValid() ? when.toMSecsSinceEpoch() : 0};
        }
    }
    return spot;
}

} // namespace naming
