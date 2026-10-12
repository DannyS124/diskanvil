// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later
#include "exif.h"

#include <QCoreApplication>
#include <QLocale>
#include <QRegularExpression>
#include <QTimeZone>

#include <algorithm>
#include <cmath>

namespace exif {
namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("exif", text);
}

// The TIFF structure EXIF is made of, read in the byte order its header says. Anything that
// points outside the bytes there are is skipped, not followed.
class Tiff
{
public:
    explicit Tiff(QByteArrayView data)
        : m_d(data)
    {
    }

    bool open()
    {
        if (m_d.size() < 8)
            return false;
        if (m_d[0] == 'I' && m_d[1] == 'I')
            m_little = true;
        else if (m_d[0] == 'M' && m_d[1] == 'M')
            m_little = false;
        else
            return false;
        // 42 is TIFF; Olympus and Panasonic raw files put their own number there.
        const quint16 magic = u16(2);
        if (magic != 42 && magic != 0x4F52 && magic != 0x5352 && magic != 0x55)
            return false;
        m_first = u32(4);
        return m_first >= 8;
    }

    qint64 first() const { return m_first; }

    struct Entry {
        quint16 tag = 0;
        quint16 type = 0;
        quint32 count = 0;
        qint64 value = 0; // where its bytes are
    };

    QVector<Entry> folder(qint64 at) const
    {
        QVector<Entry> out;
        if (!has(at, 2))
            return out;
        const int n = u16(at);
        if (n == 0 || n > 1000 || !has(at + 2, qint64(n) * 12))
            return out;
        for (int i = 0; i < n; ++i) {
            const qint64 e = at + 2 + qint64(i) * 12;
            Entry x;
            x.tag = u16(e);
            x.type = u16(e + 2);
            x.count = u32(e + 4);
            const int size = typeSize(x.type);
            if (size == 0 || x.count == 0 || x.count > (1u << 24))
                continue;
            const qint64 bytes = qint64(size) * x.count;
            x.value = bytes <= 4 ? e + 8 : qint64(u32(e + 8));
            if (has(x.value, bytes))
                out << x;
        }
        return out;
    }

    // Text up to its end or the first zero, printable characters only.
    QString text(const Entry &e) const
    {
        if (e.type != 2 && e.type != 7)
            return {};
        const QByteArrayView bytes = m_d.sliced(e.value, std::min<qint64>(e.count, 256));
        const qsizetype end = bytes.indexOf('\0');
        QString s = QString::fromUtf8(end >= 0 ? bytes.first(end) : bytes);
        s.removeIf([](QChar c) { return !c.isPrint(); });
        return s.simplified();
    }

    quint32 number(const Entry &e, int i = 0) const
    {
        if (quint32(i) >= e.count)
            return 0;
        switch (e.type) {
        case 1:
        case 7:
            return quint8(m_d[e.value + i]);
        case 3:
            return u16(e.value + 2 * i);
        case 4:
        case 13:
            return u32(e.value + 4 * i);
        default:
            return 0;
        }
    }

    double fraction(const Entry &e, int i = 0) const
    {
        if ((e.type != 5 && e.type != 10) || quint32(i) >= e.count)
            return 0;
        const qint64 at = e.value + 8 * i;
        const double top = e.type == 5 ? double(u32(at)) : double(qint32(u32(at)));
        const double bottom = e.type == 5 ? double(u32(at + 4)) : double(qint32(u32(at + 4)));
        return bottom == 0 ? 0 : top / bottom;
    }

private:
    static int typeSize(quint16 type)
    {
        switch (type) {
        case 1: case 2: case 6: case 7:
            return 1;
        case 3: case 8:
            return 2;
        case 4: case 9: case 11: case 13:
            return 4;
        case 5: case 10: case 12:
            return 8;
        default:
            return 0;
        }
    }

    bool has(qint64 at, qint64 n) const { return at >= 0 && n >= 0 && at <= m_d.size() && n <= m_d.size() - at; }

    quint16 u16(qint64 at) const
    {
        if (!has(at, 2))
            return 0;
        const auto b0 = quint8(m_d[at]), b1 = quint8(m_d[at + 1]);
        return m_little ? quint16(b0 | b1 << 8) : quint16(b0 << 8 | b1);
    }

    quint32 u32(qint64 at) const
    {
        if (!has(at, 4))
            return 0;
        const quint32 lo = u16(m_little ? at : at + 2), hi = u16(m_little ? at + 2 : at);
        return lo | hi << 16;
    }

    QByteArrayView m_d;
    bool m_little = true;
    qint64 m_first = 0;
};

// "2024:07:14 13:22:05", the way EXIF writes times, with "+02:00" when the camera said where.
QDateTime parseTime(const QString &text, const QString &offset, bool *hasZone)
{
    const QDateTime plain = QDateTime::fromString(text.left(19), QStringLiteral("yyyy:MM:dd HH:mm:ss"));
    if (!plain.isValid() || plain.date().year() < 1900)
        return {};
    *hasZone = false;
    static const QRegularExpression zone(QStringLiteral("^([+-])(\\d\\d):(\\d\\d)$"));
    const QRegularExpressionMatch m = zone.match(offset);
    if (m.hasMatch()) {
        const int seconds = (m.captured(2).toInt() * 60 + m.captured(3).toInt()) * 60 * (m.captured(1) == QLatin1String("-") ? -1 : 1);
        if (std::abs(seconds) <= 14 * 3600) {
            *hasZone = true;
            return QDateTime(plain.date(), plain.time(), QTimeZone::fromSecondsAheadOfUtc(seconds));
        }
    }
    return plain;
}

QString trimmedNumber(double value, int decimals)
{
    QString s = QLocale().toString(value, 'f', decimals);
    const QChar point = QLocale().decimalPoint().front();
    if (s.contains(point)) {
        while (s.endsWith(QLatin1Char('0')))
            s.chop(1);
        if (s.endsWith(point))
            s.chop(1);
    }
    return s;
}

} // namespace

bool Info::empty() const
{
    return make.isEmpty() && model.isEmpty() && lens.isEmpty() && software.isEmpty() && artist.isEmpty() && !taken.isValid()
        && exposure <= 0 && aperture <= 0 && iso == 0 && focalLength <= 0 && width == 0 && height == 0 && !hasLocation;
}

Info fromTiff(QByteArrayView tiff)
{
    Info info;
    Tiff t(tiff);
    if (!t.open())
        return info;
    QString dateTime, original, offset;
    qint64 exifAt = -1, gpsAt = -1;
    for (const Tiff::Entry &e : t.folder(t.first())) {
        switch (e.tag) {
        case 0x010F: info.make = t.text(e); break;
        case 0x0110: info.model = t.text(e); break;
        case 0x0112: info.orientation = int(t.number(e)); break;
        case 0x0131: info.software = t.text(e); break;
        case 0x0132: dateTime = t.text(e); break;
        case 0x013B: info.artist = t.text(e); break;
        case 0x8769: exifAt = t.number(e); break;
        case 0x8825: gpsAt = t.number(e); break;
        default: break;
        }
    }
    // Each folder of tags is read once: one pointing back at another can't make a loop.
    if (exifAt > 0 && exifAt != t.first()) {
        for (const Tiff::Entry &e : t.folder(exifAt)) {
            switch (e.tag) {
            case 0x829A: info.exposure = t.fraction(e); break;
            case 0x829D: info.aperture = t.fraction(e); break;
            case 0x8827: info.iso = int(t.number(e)); break;
            case 0x9003: original = t.text(e); break;
            case 0x9011: offset = t.text(e); break;
            case 0x920A: info.focalLength = t.fraction(e); break;
            case 0xA002: info.width = int(t.number(e)); break;
            case 0xA003: info.height = int(t.number(e)); break;
            case 0xA434: info.lens = t.text(e); break;
            default: break;
            }
        }
    }
    if (gpsAt > 0 && gpsAt != t.first() && gpsAt != exifAt) {
        QString latRef, lonRef;
        double lat = -1, lon = -1, alt = 0;
        bool hasAlt = false, below = false;
        for (const Tiff::Entry &e : t.folder(gpsAt)) {
            const auto degrees = [&t, &e] { return e.count >= 3 ? t.fraction(e, 0) + t.fraction(e, 1) / 60 + t.fraction(e, 2) / 3600 : -1.0; };
            switch (e.tag) {
            case 1: latRef = t.text(e); break;
            case 2: lat = degrees(); break;
            case 3: lonRef = t.text(e); break;
            case 4: lon = degrees(); break;
            case 5: below = t.number(e) == 1; break;
            case 6: alt = t.fraction(e); hasAlt = e.count >= 1; break;
            default: break;
            }
        }
        // 0, 0 is what cameras without a fix write: not a place.
        if (lat >= 0 && lat <= 90 && lon >= 0 && lon <= 180 && (lat > 0 || lon > 0)) {
            info.hasLocation = true;
            info.latitude = latRef.startsWith(QLatin1Char('S')) ? -lat : lat;
            info.longitude = lonRef.startsWith(QLatin1Char('W')) ? -lon : lon;
            if (hasAlt && alt < 100000) {
                info.hasAltitude = true;
                info.altitude = below ? -alt : alt;
            }
        }
    }
    info.taken = parseTime(original.isEmpty() ? dateTime : original, offset, &info.takenHasZone);
    if (info.exposure < 0 || info.exposure > 100000)
        info.exposure = 0;
    if (info.aperture < 0 || info.aperture > 1000)
        info.aperture = 0;
    if (info.focalLength < 0 || info.focalLength > 100000)
        info.focalLength = 0;
    if (info.width < 0 || info.height < 0 || info.width > 1000000 || info.height > 1000000)
        info.width = info.height = 0;
    return info;
}

Info read(const QByteArray &file)
{
    const QByteArrayView d(file);
    // TIFF, and most cameras' raw files (CR2, NEF, ARW, DNG, ORF, RW2...), are TIFF themselves.
    if (d.size() >= 8 && ((d[0] == 'I' && d[1] == 'I') || (d[0] == 'M' && d[1] == 'M'))) {
        const Info info = fromTiff(d);
        if (!info.empty())
            return info;
    }
    // A JPEG: the APP1 segment that starts with "Exif\0\0", before the picture itself.
    if (d.size() > 4 && quint8(d[0]) == 0xFF && quint8(d[1]) == 0xD8) {
        qsizetype at = 2;
        while (at + 4 <= d.size() && quint8(d[at]) == 0xFF) {
            const quint8 marker = quint8(d[at + 1]);
            if (marker == 0xD9 || marker == 0xDA)
                break;
            if (marker == 0xFF) {
                ++at;
                continue;
            }
            if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
                at += 2;
                continue;
            }
            const int length = quint8(d[at + 2]) << 8 | quint8(d[at + 3]);
            if (length < 2 || at + 2 + length > d.size())
                break;
            if (marker == 0xE1 && length >= 14 && d.sliced(at + 4, 6) == QByteArrayView("Exif\0\0", 6))
                return fromTiff(d.sliced(at + 10, length - 8));
            at += 2 + length;
        }
        return {};
    }
    // HEIC and AVIF keep it as an item, PNG in an eXIf chunk, WebP in an EXIF one, CR3 in a CMT1 box: found by what
    // it starts with, in the first few megabytes.
    const QByteArrayView head = d.first(std::min<qsizetype>(d.size(), 8 * 1024 * 1024));
    for (const QByteArrayView mark : {QByteArrayView("Exif\0\0II*\0", 10), QByteArrayView("Exif\0\0MM\0*", 10)}) {
        const qsizetype at = head.indexOf(mark);
        if (at >= 0) {
            const Info info = fromTiff(d.sliced(at + 6));
            if (!info.empty())
                return info;
        }
    }
    // Canon's CR3 keeps it in a CMT1 box.
    for (const auto &[chunk, skip] : {std::pair{QByteArrayView("eXIf"), 4}, std::pair{QByteArrayView("EXIF"), 8}, std::pair{QByteArrayView("CMT1"), 4}}) {
        const qsizetype at = head.indexOf(chunk);
        if (at >= 0 && at + skip < d.size()) {
            const Info info = fromTiff(d.sliced(at + skip));
            if (!info.empty())
                return info;
        }
    }
    return {};
}

QString sizeLabel()
{
    return tr("Size");
}

QStringList describe(const Info &i)
{
    QStringList out;
    const auto add = [&out](const QString &label, const QString &value) {
        if (!value.trimmed().isEmpty())
            out << label + QLatin1Char('\t') + value.trimmed();
    };
    if (i.taken.isValid()) {
        QString when = QLocale().toString(i.taken.date(), QLocale::LongFormat) + QStringLiteral(", ") + i.taken.time().toString(QStringLiteral("HH:mm:ss"));
        if (i.takenHasZone)
            when += QStringLiteral(" (UTC%1)").arg(i.taken.timeZone().displayName(i.taken, QTimeZone::OffsetName).remove(QStringLiteral("UTC")));
        add(tr("Taken"), when);
    }
    // "Canon" and "Canon EOS 80D" is said once.
    const QString camera = i.model.startsWith(i.make, Qt::CaseInsensitive) || i.make.isEmpty() ? i.model
                         : i.model.isEmpty() ? i.make : i.make + QLatin1Char(' ') + i.model;
    add(tr("Camera"), camera);
    add(tr("Lens"), i.lens);
    QStringList settings;
    if (i.exposure > 0)
        settings << (i.exposure >= 1 ? tr("%1 s").arg(trimmedNumber(i.exposure, 1)) : tr("1/%1 s").arg(qRound64(1 / i.exposure)));
    if (i.aperture > 0)
        settings << QStringLiteral("f/%1").arg(trimmedNumber(i.aperture, 1));
    if (i.iso > 0)
        settings << QStringLiteral("ISO %1").arg(i.iso);
    if (i.focalLength > 0)
        settings << tr("%1 mm").arg(trimmedNumber(i.focalLength, 1));
    add(tr("Settings"), settings.join(QStringLiteral(" · ")));
    if (i.width > 0 && i.height > 0)
        add(sizeLabel(), tr("%1 × %2 pixels").arg(i.width).arg(i.height));
    if (i.hasLocation) {
        QString where = QStringLiteral("%1° %2, %3° %4")
                            .arg(QLocale().toString(std::abs(i.latitude), 'f', 5), i.latitude < 0 ? tr("S") : tr("N"),
                                 QLocale().toString(std::abs(i.longitude), 'f', 5), i.longitude < 0 ? tr("W") : tr("E"));
        if (i.hasAltitude)
            where += QLatin1String(", ") + (i.altitude < 0 ? tr("%1 m below the sea").arg(qRound64(-i.altitude))
                                                           : tr("%1 m up").arg(qRound64(i.altitude)));
        add(tr("Location"), where);
    }
    add(tr("Edited with"), i.software);
    add(tr("By"), i.artist);
    return out;
}

} // namespace exif
