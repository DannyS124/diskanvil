// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ole.h"

#include "textkinds.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QLocale>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QTimeZone>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace ole {

namespace {

constexpr quint32 kLastSector = 0xFFFFFFFA; // numbers from here up mean "end", "free" and the like
constexpr qint64 kMostStream = 64 * 1024 * 1024;

QString tr(const char *text)
{
    return QCoreApplication::translate("ole", text);
}

// A view of bytes that never reads past their end: what isn't there reads as zero.
struct Bytes {
    const uchar *p;
    quint64 n;
    explicit Bytes(const QByteArray &d)
        : p(reinterpret_cast<const uchar *>(d.constData()))
        , n(quint64(d.size()))
    {
    }
    bool has(quint64 at, quint64 len) const { return at <= n && len <= n - at; }
    quint32 u8(quint64 at) const { return has(at, 1) ? p[at] : 0; }
    quint32 le16(quint64 at) const { return has(at, 2) ? qFromLittleEndian<quint16>(p + at) : 0; }
    quint32 le32(quint64 at) const { return has(at, 4) ? qFromLittleEndian<quint32>(p + at) : 0; }
    quint64 le64(quint64 at) const { return has(at, 8) ? qFromLittleEndian<quint64>(p + at) : 0; }
    double f64(quint64 at) const
    {
        const quint64 bits = le64(at);
        double v;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    }
    QByteArray mid(quint64 at, quint64 len) const
    {
        if (at >= n)
            return {};
        return QByteArray(reinterpret_cast<const char *>(p + at), qsizetype(std::min(len, n - at)));
    }
    QString utf16(quint64 at, quint64 chars) const
    {
        const QByteArray b = mid(at, chars * 2);
        return QString::fromUtf16(reinterpret_cast<const char16_t *>(b.constData()), b.size() / 2);
    }
};

QString decodeCodepage(QByteArray bytes, quint32 codepage)
{
    if (const qsizetype zero = bytes.indexOf('\0'); zero >= 0)
        bytes.truncate(zero);
    if (codepage == 65001)
        return QString::fromUtf8(bytes);
    QStringDecoder decoder(QByteArray("windows-" + QByteArray::number(codepage)).constData());
    if (codepage != 0 && decoder.isValid())
        return decoder(bytes);
    QStringDecoder western("windows-1252");
    return western.isValid() ? QString(western(bytes)) : QString::fromLatin1(bytes);
}

QDateTime fromFileTime(quint64 ticks)
{
    if (ticks == 0)
        return {};
    const QDateTime t = QDateTime(QDate(1601, 1, 1), QTime(0, 0), QTimeZone::UTC).addMSecs(qint64(ticks / 10000));
    return t.date().year() > 1980 && t.date().year() < 2200 ? t : QDateTime();
}

QString when(const QDateTime &t)
{
    if (!t.isValid())
        return {};
    const QDateTime local = t.toLocalTime();
    return QLocale().toString(local.date(), QLocale::LongFormat) + QStringLiteral(", ") + QLocale().toString(local.time(), QLocale::ShortFormat);
}

// Characters Word and PowerPoint use for their own purposes, made into plain text.
QString plain(const QString &raw, int most)
{
    QString out;
    out.reserve(std::min<qsizetype>(raw.size(), most));
    int hidden = 0; // inside a field's instructions ({ PAGE } and the like): not shown
    QVector<bool> fields;
    for (const QChar c : raw) {
        if (out.size() >= most)
            break;
        const char16_t u = c.unicode();
        if (u == 0x13) { // a field starts with its instructions
            fields << false;
            ++hidden;
            continue;
        }
        if (u == 0x14) { // then what it shows
            if (!fields.isEmpty() && !fields.last()) {
                fields.last() = true;
                --hidden;
            }
            continue;
        }
        if (u == 0x15) {
            if (!fields.isEmpty()) {
                if (!fields.last())
                    --hidden;
                fields.removeLast();
            }
            continue;
        }
        if (hidden > 0)
            continue;
        if (u == '\r' || u == 0x0B || u == 0x0C)
            out += QLatin1Char('\n');
        else if (u == 0x07)
            out += QLatin1Char('\t'); // the end of a table cell
        else if (u == 0x1E)
            out += QLatin1Char('-');
        else if (u >= 0x20 || u == '\t' || u == '\n')
            out += c;
    }
    static const QRegularExpression blankLines(QStringLiteral("\n{3,}"));
    out.replace(blankLines, QStringLiteral("\n\n"));
    return out.trimmed();
}

// --- Property sets ([MS-OLEPS]): SummaryInformation and DocumentSummaryInformation

struct Properties {
    QHash<quint32, QString> text;
    QHash<quint32, QDateTime> time;
    QHash<quint32, qint64> number;
};

Properties readProperties(const QByteArray &stream)
{
    Properties out;
    const Bytes b(stream);
    if (b.le16(0) != 0xFFFE || b.le32(24) == 0)
        return out;
    const quint64 section = b.le32(44);
    const quint32 count = std::min<quint32>(b.le32(section + 4), 1000);
    quint32 codepage = 1252;
    for (quint32 i = 0; i < count; ++i) {
        if (b.le32(section + 8 + 8ull * i) == 1)
            codepage = b.le16(section + b.le32(section + 12 + 8ull * i) + 4);
    }
    for (quint32 i = 0; i < count; ++i) {
        const quint32 id = b.le32(section + 8 + 8ull * i);
        const quint64 at = section + b.le32(section + 12 + 8ull * i);
        switch (b.le32(at) & 0xFFFF) {
        case 0x1E: // a string in the code page
            out.text.insert(id, decodeCodepage(b.mid(at + 8, std::min<quint32>(b.le32(at + 4), 4096)), codepage).simplified());
            break;
        case 0x1F: // UTF-16
            out.text.insert(id, b.utf16(at + 8, std::min<quint32>(b.le32(at + 4), 2048)).remove(QChar(0)).simplified());
            break;
        case 0x40:
            out.time.insert(id, fromFileTime(b.le64(at + 4)));
            break;
        case 0x03:
            out.number.insert(id, qint32(b.le32(at + 4)));
            break;
        case 0x02:
            out.number.insert(id, qint16(b.le16(at + 4)));
            break;
        default:
            break;
        }
    }
    return out;
}

// --- Word ([MS-DOC]): the text is in pieces, listed in the table stream's piece table

QString wordText(const File &f, int most)
{
    const QByteArray word = f.stream(QStringLiteral("WordDocument"));
    const Bytes w(word);
    if (w.le16(0) != 0xA5EC)
        return {};
    const quint32 flags = w.le16(10);
    if (flags & 0x0100) // locked with a password
        return {};
    if (w.le16(2) < 101) { // Word 6 and 95: the text in one run, in the code page
        const quint32 from = w.le32(24), to = w.le32(28);
        return to > from ? plain(decodeCodepage(w.mid(from, std::min<quint32>(to - from, quint32(most) * 2)), 1252), most) : QString();
    }
    const QByteArray table = f.stream(flags & 0x0200 ? QStringLiteral("1Table") : QStringLiteral("0Table"));
    const Bytes t(table);
    const quint32 ccpText = w.le32(76), fcClx = w.le32(0x1A2), lcbClx = w.le32(0x1A6);
    if (lcbClx == 0 || !t.has(fcClx, lcbClx))
        return {};
    quint64 at = fcClx;
    const quint64 end = quint64(fcClx) + lcbClx;
    for (int prcs = 0; at < end && t.u8(at) == 0x01 && prcs < 100000; ++prcs) // formatting changes before it
        at += 3 + quint64(qint16(t.le16(at + 1)) < 0 ? 0 : t.le16(at + 1));
    if (at + 5 > end || t.u8(at) != 0x02)
        return {};
    const quint32 lcb = t.le32(at + 1);
    at += 5;
    if (lcb < 16 || (lcb - 4) % 12 != 0 || !t.has(at, lcb))
        return {};
    const quint32 pieces = (lcb - 4) / 12;
    QString raw;
    for (quint32 i = 0; i < pieces && raw.size() < most * 2; ++i) {
        const quint32 cpStart = t.le32(at + 4ull * i), cpEnd = t.le32(at + 4ull * (i + 1));
        if (cpEnd <= cpStart || (ccpText && cpStart >= ccpText))
            continue; // the main text only: not the footnotes, headers and the like after it
        const quint32 chars = std::min<quint32>((ccpText ? std::min(cpEnd, ccpText) : cpEnd) - cpStart, quint32(most) * 2);
        const quint32 fc = t.le32(at + 4ull * (pieces + 1) + 8ull * i + 2);
        if (fc & 0x40000000) // one byte a character, in Windows' western code page
            raw += decodeCodepage(w.mid((fc & 0x3FFFFFFF) / 2, chars), 1252);
        else
            raw += w.utf16(fc & 0x3FFFFFFF, chars);
    }
    return plain(raw, most);
}

// --- Excel ([MS-XLS]): records; the text of the cells is in a shared table of strings

struct Record {
    quint32 type;
    quint64 at; // where its data starts in the stream
    quint32 size;
};

QVector<Record> records(const Bytes &b)
{
    QVector<Record> out;
    for (quint64 at = 0; b.has(at, 4) && out.size() < 4000000;) {
        const quint32 type = b.le16(at), size = b.le16(at + 2);
        if (!b.has(at + 4, size))
            break;
        out.push_back({type, at + 4, size});
        at += 4 + quint64(size);
    }
    return out;
}

// A string that may run on into the CONTINUE records after its own: where it crosses into the
// next one, a byte says whether the rest is one or two bytes a character.
class Strings
{
public:
    Strings(const Bytes &b, const QVector<Record> &all, qsizetype first)
    {
        for (qsizetype i = first; i < all.size() && (i == first || all[i].type == 0x003C); ++i) {
            m_starts << m_data.size();
            m_data += b.mid(all[i].at, all[i].size);
        }
    }
    bool more() const { return m_at < m_data.size(); }
    quint32 u8()
    {
        const quint32 v = m_at < m_data.size() ? quint8(m_data[m_at]) : 0;
        ++m_at;
        return v;
    }
    quint32 u16()
    {
        const quint32 lo = u8();
        return lo | u8() << 8;
    }
    quint32 u32()
    {
        const quint32 lo = u16();
        return lo | u16() << 16;
    }
    void skip(quint64 n) { m_at += qsizetype(std::min<quint64>(n, quint64(m_data.size()))); }
    QString chars(quint32 count, bool wide)
    {
        QString out;
        while (count > 0 && m_at < m_data.size()) {
            const auto next = std::upper_bound(m_starts.cbegin(), m_starts.cend(), m_at);
            const qsizetype until = next == m_starts.cend() ? m_data.size() : *next;
            const quint32 n = std::min<quint32>(count, quint32((until - m_at) / (wide ? 2 : 1)));
            const QByteArray piece = m_data.mid(m_at, qsizetype(n) * (wide ? 2 : 1));
            out += wide ? QString::fromUtf16(reinterpret_cast<const char16_t *>(piece.constData()), piece.size() / 2) : QString::fromLatin1(piece);
            m_at += piece.size();
            count -= n;
            if (count > 0) {
                if (m_at != until || until >= m_data.size())
                    break;
                wide = m_data[m_at++] & 1; // into the next record
            }
        }
        return out;
    }

private:
    QByteArray m_data;
    QVector<qsizetype> m_starts;
    qsizetype m_at = 0;
};

QString number(double v)
{
    return QString::number(v, 'g', 15);
}

QString excelError(quint32 code)
{
    switch (code) {
    case 0x00:
        return QStringLiteral("#NULL!");
    case 0x07:
        return QStringLiteral("#DIV/0!");
    case 0x0F:
        return QStringLiteral("#VALUE!");
    case 0x17:
        return QStringLiteral("#REF!");
    case 0x1D:
        return QStringLiteral("#NAME?");
    case 0x24:
        return QStringLiteral("#NUM!");
    default:
        return QStringLiteral("#N/A");
    }
}

double rk(quint32 v)
{
    double out;
    if (v & 2) {
        out = double(qint32(v) >> 2);
    } else {
        const quint64 bits = quint64(v & 0xFFFFFFFC) << 32;
        std::memcpy(&out, &bits, sizeof out);
    }
    return v & 1 ? out / 100 : out;
}

struct Workbook {
    bool locked = false, macroSheets = false;
    QString text;
};

Workbook excel(const File &f, int most)
{
    Workbook out;
    QByteArray stream = f.stream(QStringLiteral("Workbook"));
    if (stream.isEmpty())
        stream = f.stream(QStringLiteral("Book")); // Excel 5 and 95
    const Bytes b(stream);
    const QVector<Record> all = records(b);
    if (all.isEmpty() || all[0].type != 0x0809)
        return out;
    const bool biff8 = b.le16(all[0].at) == 0x0600;
    quint32 codepage = 1252;
    QStringList shared;
    QHash<quint64, QString> sheetNames; // by where the sheet starts
    QStringList sheetOrder;
    struct Sheet {
        QString name;
        QMap<quint32, QMap<quint32, QString>> cells;
    };
    QVector<Sheet> sheets;
    const auto shortString = [&](quint64 at, bool lengthIs16) {
        const quint32 count = lengthIs16 ? b.le16(at) : b.u8(at);
        at += lengthIs16 ? 2 : 1;
        if (!biff8)
            return decodeCodepage(b.mid(at, count), codepage);
        const bool wide = b.u8(at) & 1;
        return wide ? b.utf16(at + 1, count) : QString::fromLatin1(b.mid(at + 1, count));
    };
    int cells = 0;
    for (qsizetype i = 0; i < all.size() && cells < 200000; ++i) {
        const Record &r = all[i];
        switch (r.type) {
        case 0x002F: // FILEPASS: locked with a password
            out.locked = true;
            return out;
        case 0x0042:
            codepage = b.le16(r.at);
            break;
        case 0x0085: { // BOUNDSHEET: a sheet's name, and where it starts
            const quint32 kind = b.u8(r.at + 5);
            if (kind == 1)
                out.macroSheets = true;
            sheetNames.insert(b.le32(r.at), shortString(r.at + 6, false));
            break;
        }
        case 0x0809: // BOF: a sheet starts
            if (b.le16(r.at + 2) == 0x0010 || b.le16(r.at + 2) == 0x0040)
                sheets.push_back({sheetNames.value(r.at - 4, tr("Sheet %1").arg(sheets.size() + 1)), {}});
            if (b.le16(r.at + 2) == 0x0040)
                out.macroSheets = true;
            break;
        case 0x00FC: { // SST: the shared strings
            Strings t(b, all, i);
            t.skip(4); // how many times they're used in all; then how many there are
            const quint32 count = std::min<quint32>(t.u32(), 1000000);
            for (quint32 k = 0; k < count && t.more(); ++k) {
                const quint32 chars = t.u16(), flags = t.u8();
                const quint32 runs = flags & 8 ? t.u16() : 0;
                const quint32 extra = flags & 4 ? t.u32() : 0;
                shared << t.chars(chars, flags & 1);
                t.skip(4ull * runs + extra);
            }
            break;
        }
        default:
            break;
        }
        if (sheets.isEmpty())
            continue;
        QMap<quint32, QMap<quint32, QString>> &into = sheets.last().cells;
        const quint32 row = b.le16(r.at), col = b.le16(r.at + 2);
        switch (r.type) {
        case 0x00FD: // LABELSST
            into[row][col] = shared.value(qsizetype(b.le32(r.at + 6)));
            ++cells;
            break;
        case 0x0204: // LABEL
            into[row][col] = shortString(r.at + 6, true);
            ++cells;
            break;
        case 0x0203: // NUMBER
            into[row][col] = number(b.f64(r.at + 6));
            ++cells;
            break;
        case 0x027E: // RK
            into[row][col] = number(rk(b.le32(r.at + 6)));
            ++cells;
            break;
        case 0x00BD: { // MULRK: several in a row
            const quint32 last = b.le16(r.at + r.size - 2);
            for (quint32 c = col, k = 0; c <= last && k < 256 && 4 + 6 * k + 6 <= r.size; ++c, ++k)
                into[row][c] = number(rk(b.le32(r.at + 4 + 6 * k + 2)));
            ++cells;
            break;
        }
        case 0x0006: // FORMULA: what it came to, a number or (in the STRING after it) words
            if (b.le16(r.at + 12) != 0xFFFF)
                into[row][col] = number(b.f64(r.at + 6));
            else if (b.u8(r.at + 6) == 0 && i + 1 < all.size() && all[i + 1].type == 0x0207)
                into[row][col] = shortString(all[i + 1].at, true);
            else if (b.u8(r.at + 6) == 1)
                into[row][col] = b.u8(r.at + 8) ? QStringLiteral("TRUE") : QStringLiteral("FALSE");
            else if (b.u8(r.at + 6) == 2)
                into[row][col] = excelError(b.u8(r.at + 8));
            ++cells;
            break;
        case 0x0205: // BOOLERR
            into[row][col] = b.u8(r.at + 7) ? excelError(b.u8(r.at + 6)) : b.u8(r.at + 6) ? QStringLiteral("TRUE") : QStringLiteral("FALSE");
            ++cells;
            break;
        default:
            break;
        }
    }
    QString text;
    for (const Sheet &sheet : std::as_const(sheets)) {
        if (sheet.cells.isEmpty() || text.size() >= most)
            continue;
        text += QStringLiteral("[%1]\n").arg(sheet.name);
        for (auto row = sheet.cells.cbegin(); row != sheet.cells.cend() && text.size() < most; ++row) {
            QStringList line;
            for (auto c = row->cbegin(); c != row->cend() && c.key() < 64; ++c) {
                while (quint32(line.size()) < c.key())
                    line << QString();
                line << c.value().simplified();
            }
            text += line.join(QLatin1Char('\t')) + QLatin1Char('\n');
        }
        text += QLatin1Char('\n');
    }
    out.text = text.trimmed().left(most);
    return out;
}

// --- PowerPoint ([MS-PPT]): records inside records; the text is in the text atoms

struct Slides {
    bool macros = false;
    QString text;
};

void walkPowerPoint(const Bytes &b, quint64 from, quint64 to, int depth, int most, Slides &out, int &seen)
{
    for (quint64 at = from; at + 8 <= to && out.text.size() < most && ++seen < 2000000;) {
        const quint32 head = b.le16(at), type = b.le16(at + 2);
        const quint64 body = at + 8, end = std::min<quint64>(body + b.le32(at + 4), to);
        if (type == 0x0400 && b.le32(body) != 0 && b.le32(body + 4) != 0) // VBAInfoAtom: where the macros are, and that there are some
            out.macros = true;
        if ((head & 0x0F) == 0x0F) {
            // In, except the masters' and handouts' own placeholder words.
            const bool master = type == 0x03F8 || type == 0x0FC9 || (type == 0x0FF0 && (head >> 4) != 0);
            if (!master && depth < 32)
                walkPowerPoint(b, body, end, depth + 1, most, out, seen);
        } else if (type == 0x0FA0) { // TextCharsAtom: UTF-16 ("*" alone is where the slide's number goes)
            const QString words = b.utf16(body, (end - body) / 2);
            if (words != QLatin1String("*"))
                out.text += words + QLatin1Char('\n');
        } else if (type == 0x0FA8) { // TextBytesAtom: one byte a character
            out.text += QString::fromLatin1(b.mid(body, end - body)) + QLatin1Char('\n');
        } else if (type == 0x03F3 && !out.text.isEmpty()) { // the next slide's text starts
            out.text += QLatin1Char('\n');
        }
        at = end > at ? end : at + 8;
    }
}

Slides powerPoint(const File &f, int most)
{
    Slides out;
    const QByteArray stream = f.stream(QStringLiteral("PowerPoint Document"));
    int seen = 0;
    walkPowerPoint(Bytes(stream), 0, quint64(stream.size()), 0, most * 2, out, seen);
    out.text = plain(out.text, most);
    return out;
}

// --- Outlook ([MS-OXMSG]): each property of the email in a stream named after its number

QString mailString(const File &f, const QString &storage, quint32 id)
{
    const QString base = (storage.isEmpty() ? QString() : storage + QLatin1Char('/')) + QStringLiteral("__substg1.0_")
                       + QStringLiteral("%1").arg(id, 4, 16, QLatin1Char('0')).toUpper();
    const QByteArray wide = f.stream(base + QStringLiteral("001F"), 8 * 1024 * 1024);
    if (!wide.isEmpty())
        return QString::fromUtf16(reinterpret_cast<const char16_t *>(wide.constData()), wide.size() / 2).remove(QChar(0)).trimmed();
    return decodeCodepage(f.stream(base + QStringLiteral("001E"), 8 * 1024 * 1024), 1252).trimmed();
}

QDateTime mailTime(const File &f, quint32 tag)
{
    const QByteArray props = f.stream(QStringLiteral("__properties_version1.0"), 1024 * 1024);
    const Bytes b(props);
    for (quint64 at = 32; b.has(at, 16); at += 16) { // after the email's 32-byte header
        if (b.le32(at) == tag)
            return fromFileTime(b.le64(at + 8));
    }
    return {};
}

struct Mail {
    QString from, to, cc, subject, body;
    QDateTime sent;
    QStringList attached;
};

Mail mail(const File &f)
{
    Mail m;
    const QString name = mailString(f, {}, 0x0C1A), smtp = mailString(f, {}, 0x5D01), address = mailString(f, {}, 0x0C1F);
    const QString email = smtp.contains(QLatin1Char('@')) ? smtp : address.contains(QLatin1Char('@')) ? address : QString();
    m.from = name.isEmpty() ? email : email.isEmpty() || email == name ? name : QStringLiteral("%1 <%2>").arg(name, email);
    m.to = mailString(f, {}, 0x0E04);
    m.cc = mailString(f, {}, 0x0E03);
    m.subject = mailString(f, {}, 0x0037);
    m.sent = mailTime(f, 0x00390040);
    if (!m.sent.isValid())
        m.sent = mailTime(f, 0x0E060040); // when it arrived
    m.body = mailString(f, {}, 0x1000);
    if (m.body.isEmpty()) { // only as a web page: its words without the markup
        m.body = texts::fromHtml(QString::fromUtf8(f.stream(QStringLiteral("__substg1.0_10130102"), 8 * 1024 * 1024)));
    }
    for (const File::Entry &e : f.entries()) {
        if (!e.storage || e.path.contains(QLatin1Char('/')) || !e.path.startsWith(QLatin1String("__attach_version1.0_#")))
            continue;
        QString file = mailString(f, e.path, 0x3707);
        if (file.isEmpty())
            file = mailString(f, e.path, 0x3704);
        if (file.isEmpty())
            file = tr("an attachment");
        m.attached << file;
    }
    return m;
}

} // namespace

File::File(const QByteArray &data)
    : m_data(data)
{
    const Bytes b(m_data);
    if (!b.has(0, 512) || std::memcmp(b.p, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) != 0)
        return;
    const quint32 shift = b.le16(30);
    if ((shift != 9 && shift != 12) || b.le16(32) != 6)
        return;
    m_shift = int(shift);
    const quint64 sector = 1ull << shift;
    const quint32 fatSectors = std::min<quint32>(b.le32(44), 1u << 16);
    // Where the FAT is: the first 109 of its sectors are listed in the header, the rest in a chain.
    QVector<quint32> fatAt;
    for (int i = 0; i < 109 && quint32(fatAt.size()) < fatSectors; ++i)
        fatAt << b.le32(76 + 4ull * i);
    QSet<quint32> seen;
    for (quint32 next = b.le32(68), d = 0; d < b.le32(72) && next < kLastSector && quint32(fatAt.size()) < fatSectors && !seen.contains(next); ++d) {
        seen.insert(next);
        const quint64 at = (quint64(next) + 1) * sector;
        for (quint64 k = 0; k + 1 < sector / 4 && quint32(fatAt.size()) < fatSectors; ++k)
            fatAt << b.le32(at + 4 * k);
        next = b.le32(at + sector - 4);
    }
    for (const quint32 s : std::as_const(fatAt)) {
        const quint64 at = (quint64(s) + 1) * sector;
        for (quint64 k = 0; k < sector / 4; ++k) // a FAT sector that isn't there reads as free
            m_fat << (s < kLastSector && b.has(at + 4 * k, 4) ? b.le32(at + 4 * k) : 0xFFFFFFFF);
    }
    // The directory: 128 bytes an entry, kept as a tree (each entry has its siblings either side).
    const QByteArray directory = chain(b.le32(48), ~quint64(0), false, 16 * 1024 * 1024);
    const Bytes d(directory);
    struct Raw {
        QString name;
        quint32 type, left, right, child, start;
        quint64 size;
    };
    QVector<Raw> raw;
    for (quint64 at = 0; d.has(at, 128); at += 128) {
        const quint32 length = std::min<quint32>(d.le16(at + 64), 64);
        quint64 size = d.le64(at + 120);
        if (shift == 9)
            size &= 0xFFFFFFFF; // version 3 files can leave garbage in the high half
        raw.push_back({d.utf16(at, length >= 2 ? length / 2 - 1 : 0), d.u8(at + 66), d.le32(at + 68), d.le32(at + 72), d.le32(at + 76),
                       d.le32(at + 116), size});
    }
    if (raw.isEmpty() || raw[0].type != 5)
        return;
    m_miniCutoff = 4096;
    // The small streams are in the mini stream (the root's), in 64-byte pieces with their own FAT.
    m_miniStream = chain(raw[0].start, raw[0].size, false, kMostStream);
    const QByteArray miniFat = chain(b.le32(60), quint64(b.le32(64)) * sector, false, 16 * 1024 * 1024);
    for (qsizetype at = 0; at + 4 <= miniFat.size(); at += 4)
        m_miniFat << qFromLittleEndian<quint32>(miniFat.constData() + at);
    QVector<QPair<quint32, QString>> todo{{raw[0].child, QString()}};
    QSet<quint32> visited;
    while (!todo.isEmpty()) {
        const auto [i, parent] = todo.takeLast();
        if (i >= quint32(raw.size()) || visited.contains(i))
            continue;
        visited.insert(i);
        const Raw &r = raw[qsizetype(i)];
        if (r.type == 1 || r.type == 2) {
            const QString path = parent.isEmpty() ? r.name : parent + QLatin1Char('/') + r.name;
            m_entries.push_back({path, r.type == 1, r.start, r.size});
            if (r.type == 1)
                todo.push_back({r.child, path});
        }
        todo.push_back({r.left, parent});
        todo.push_back({r.right, parent});
    }
    m_valid = true;
}

QByteArray File::chain(quint32 start, quint64 size, bool mini, qint64 most) const
{
    const QVector<quint32> &fat = mini ? m_miniFat : m_fat;
    const QByteArray &from = mini ? m_miniStream : m_data;
    const quint64 sector = mini ? 64 : 1ull << m_shift;
    const quint64 want = std::min<quint64>(size, quint64(most));
    QByteArray out;
    quint32 s = start;
    // As many steps as the FAT has sectors at most: a chain that loops ends there.
    for (qsizetype steps = 0; s < kLastSector && quint64(out.size()) < want && steps <= fat.size(); ++steps) {
        const quint64 at = mini ? quint64(s) * sector : (quint64(s) + 1) * sector;
        const quint64 n = std::min<quint64>(sector, want - quint64(out.size()));
        if (at >= quint64(from.size()))
            break; // cut short here
        out.append(from.constData() + at, qsizetype(std::min<quint64>(n, quint64(from.size()) - at)));
        if (s >= quint32(fat.size()))
            break;
        s = fat[qsizetype(s)];
    }
    return out;
}

bool File::has(const QString &path) const
{
    return std::any_of(m_entries.cbegin(), m_entries.cend(), [&path](const Entry &e) { return e.path.compare(path, Qt::CaseInsensitive) == 0; });
}

QByteArray File::stream(const QString &path, qint64 most) const
{
    for (const Entry &e : m_entries) {
        if (!e.storage && e.path.compare(path, Qt::CaseInsensitive) == 0)
            return chain(e.start, e.size, e.size < m_miniCutoff, std::min(most, kMostStream));
    }
    return {};
}

QString kind(const File &file)
{
    if (file.has(QStringLiteral("WordDocument")))
        return QStringLiteral("doc");
    if (file.has(QStringLiteral("Workbook")) || file.has(QStringLiteral("Book")))
        return QStringLiteral("xls");
    if (file.has(QStringLiteral("PowerPoint Document")))
        return QStringLiteral("ppt");
    if (file.has(QStringLiteral("__properties_version1.0"))
        || std::any_of(file.entries().cbegin(), file.entries().cend(), [](const File::Entry &e) { return e.path.startsWith(QLatin1String("__substg1.0_")); }))
        return QStringLiteral("msg");
    return {};
}

bool locked(const File &file)
{
    if (file.has(QStringLiteral("EncryptedPackage")) || file.has(QStringLiteral("EncryptedSummary")))
        return true; // a password-protected Word, Excel or PowerPoint file of 2007 or later, or a locked .ppt
    const QString k = kind(file);
    if (k == QLatin1String("doc"))
        return Bytes(file.stream(QStringLiteral("WordDocument"), 64)).le16(10) & 0x0100;
    if (k == QLatin1String("xls"))
        return excel(file, 0).locked;
    return false;
}

bool hasMacros(const File &file)
{
    for (const File::Entry &e : file.entries()) {
        for (const QString &part : e.path.split(QLatin1Char('/'))) {
            if (part.compare(QLatin1String("Macros"), Qt::CaseInsensitive) == 0 || part.compare(QLatin1String("_VBA_PROJECT_CUR"), Qt::CaseInsensitive) == 0
                || part.compare(QLatin1String("_VBA_PROJECT"), Qt::CaseInsensitive) == 0 || part.compare(QLatin1String("VBA"), Qt::CaseInsensitive) == 0)
                return true;
        }
    }
    const QString k = kind(file);
    if (k == QLatin1String("xls"))
        return excel(file, 0).macroSheets;
    if (k == QLatin1String("ppt"))
        return powerPoint(file, 1).macros;
    return false;
}

QString text(const File &file, int most)
{
    const QString k = kind(file);
    if (k == QLatin1String("doc"))
        return wordText(file, most);
    if (k == QLatin1String("xls"))
        return excel(file, most).text;
    if (k == QLatin1String("ppt"))
        return powerPoint(file, most).text;
    if (k == QLatin1String("msg")) {
        const Mail m = mail(file);
        QStringList head;
        const auto line = [&head](const QString &label, const QString &value) {
            if (!value.isEmpty())
                head << label + QStringLiteral(": ") + value.simplified();
        };
        line(tr("From"), m.from);
        line(tr("To"), m.to);
        line(tr("Cc"), m.cc);
        line(tr("Sent"), when(m.sent));
        line(tr("Subject"), m.subject);
        line(tr("Attached"), m.attached.join(QStringLiteral(", ")));
        return plain(head.join(QLatin1Char('\n')) + QStringLiteral("\n\n") + m.body, most);
    }
    return {};
}

Summary summary(const File &file)
{
    Summary out;
    if (kind(file) == QLatin1String("msg")) {
        const Mail m = mail(file);
        out.subject = m.subject;
        out.author = m.from;
        out.created = m.sent;
        return out;
    }
    const Properties s = readProperties(file.stream(QStringLiteral("\x05SummaryInformation"), 1024 * 1024));
    out.title = s.text.value(2);
    out.subject = s.text.value(3);
    out.author = s.text.value(4);
    out.created = s.time.value(12);
    out.changed = s.time.value(13);
    return out;
}

QStringList properties(const File &file)
{
    QStringList out;
    const auto add = [&out](const QString &label, const QString &value) {
        if (!value.trimmed().isEmpty())
            out << label + QLatin1Char('\t') + value.trimmed().left(200);
    };
    if (locked(file))
        add(tr("Locked"), tr("It needs a password to open"));
    if (kind(file) == QLatin1String("msg")) {
        const Mail m = mail(file);
        add(tr("From"), m.from);
        add(tr("To"), m.to);
        add(tr("Cc"), m.cc);
        add(tr("Subject"), m.subject);
        add(tr("Sent"), when(m.sent));
        if (!m.attached.isEmpty())
            add(tr("Attached"), m.attached.join(QStringLiteral(", ")));
        return out;
    }
    const Properties s = readProperties(file.stream(QStringLiteral("\x05SummaryInformation"), 1024 * 1024));
    const Properties d = readProperties(file.stream(QStringLiteral("\x05DocumentSummaryInformation"), 1024 * 1024));
    const auto count = [](const Properties &p, quint32 id) { return p.number.value(id) > 0 ? QString::number(p.number.value(id)) : QString(); };
    add(tr("Title"), s.text.value(2));
    add(tr("Subject"), s.text.value(3));
    add(tr("Author"), s.text.value(4));
    add(tr("Keywords"), s.text.value(5));
    add(tr("Comments"), s.text.value(6));
    add(tr("Last changed by"), s.text.value(8));
    add(tr("Created"), when(s.time.value(12)));
    add(tr("Changed"), when(s.time.value(13)));
    add(tr("Last printed"), when(s.time.value(11)));
    add(tr("Made with"), s.text.value(18));
    add(tr("Company"), d.text.value(15));
    add(tr("Pages"), count(s, 14));
    add(tr("Words"), count(s, 15));
    add(tr("Slides"), count(d, 7));
    if (hasMacros(file))
        add(tr("Macros"), tr("It has macros (small programs), so it isn't opened in its own program"));
    return out;
}

} // namespace ole
