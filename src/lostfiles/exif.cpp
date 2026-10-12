// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later
#include "exif.h"

#include "places.h"

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
    // The folder after the one at `at` (0 when there's none).
    qint64 next(qint64 at) const
    {
        const int n = u16(at);
        return n > 0 && has(at + 2, qint64(n) * 12 + 4) ? qint64(u32(at + 2 + qint64(n) * 12)) : 0;
    }

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
    QString dateTime, original, offset, changedOffset;
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
            case 0x9010: changedOffset = t.text(e); break;
            case 0x9011: offset = t.text(e); break;
            case 0xA430: info.owner = t.text(e); break;
            case 0xA431: info.serial = t.text(e); break;
            case 0xA435: info.lensSerial = t.text(e); break;
            case 0x920A: info.focalLength = t.fraction(e); break;
            case 0xA002: info.width = int(t.number(e)); break;
            case 0xA003: info.height = int(t.number(e)); break;
            case 0xA434: info.lens = t.text(e); break;
            default: break;
            }
        }
    }
    if (gpsAt > 0 && gpsAt != t.first() && gpsAt != exifAt) {
        QString latRef, lonRef, directionRef, gpsDate;
        double lat = -1, lon = -1, alt = 0, direction = -1;
        bool hasAlt = false, below = false;
        double hms[3] = {-1, -1, -1};
        for (const Tiff::Entry &e : t.folder(gpsAt)) {
            const auto degrees = [&t, &e] { return e.count >= 3 ? t.fraction(e, 0) + t.fraction(e, 1) / 60 + t.fraction(e, 2) / 3600 : -1.0; };
            switch (e.tag) {
            case 1: latRef = t.text(e); break;
            case 2: lat = degrees(); break;
            case 3: lonRef = t.text(e); break;
            case 4: lon = degrees(); break;
            case 5: below = t.number(e) == 1; break;
            case 6: alt = t.fraction(e); hasAlt = e.count >= 1; break;
            case 7:
                for (int i = 0; i < 3 && quint32(i) < e.count; ++i)
                    hms[i] = t.fraction(e, i);
                break;
            case 0x10: directionRef = t.text(e); break;
            case 0x11: direction = e.count >= 1 ? t.fraction(e) : -1; break;
            case 0x1D: gpsDate = t.text(e); break;
            default: break;
            }
        }
        if (direction >= 0 && direction <= 360) {
            info.hasDirection = true;
            info.direction = direction;
            info.directionMagnetic = directionRef.startsWith(QLatin1Char('M'));
        }
        const QDate day = QDate::fromString(gpsDate.left(10), QStringLiteral("yyyy:MM:dd"));
        if (day.isValid() && day.year() >= 1990 && hms[0] >= 0 && hms[0] < 24 && hms[1] >= 0 && hms[1] < 60 && hms[2] >= 0 && hms[2] < 61)
            info.satelliteTime = QDateTime(day, QTime(int(hms[0]), int(hms[1]), int(std::min(59.0, hms[2]))), QTimeZone::UTC);
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
    bool changedHasZone = false;
    if (!original.isEmpty())
        info.changed = parseTime(dateTime, changedOffset.isEmpty() ? offset : changedOffset, &changedHasZone);
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

namespace {

Info readExif(QByteArrayView d, QByteArrayView *xmp);

} // namespace

Info read(const QByteArray &file)
{
    const QByteArrayView d(file);
    QByteArrayView xmp;
    Info info = readExif(d, &xmp);
    // XMP: a JPEG's own APP1 segment for it, or anywhere in the start of the others.
    if (xmp.isEmpty()) {
        const QByteArrayView head = d.first(std::min<qsizetype>(d.size(), 8 * 1024 * 1024));
        const qsizetype start = head.indexOf(QByteArrayView("<x:xmpmeta"));
        const qsizetype end = start >= 0 ? head.indexOf(QByteArrayView("</x:xmpmeta>"), start) : -1;
        if (end > start)
            xmp = head.sliced(start, end - start);
    }
    if (!xmp.isEmpty())
        readXmp(xmp.first(std::min<qsizetype>(xmp.size(), 512 * 1024)), info);
    return info;
}

namespace {

Info readExif(QByteArrayView d, QByteArrayView *xmp)
{
    // TIFF, and most cameras' raw files (CR2, NEF, ARW, DNG, ORF, RW2...), are TIFF themselves.
    if (d.size() >= 8 && ((d[0] == 'I' && d[1] == 'I') || (d[0] == 'M' && d[1] == 'M'))) {
        const Info info = fromTiff(d);
        if (!info.empty())
            return info;
    }
    // A JPEG: the APP1 segment that starts with "Exif\0\0", before the picture itself.
    if (d.size() > 4 && quint8(d[0]) == 0xFF && quint8(d[1]) == 0xD8) {
        qsizetype at = 2, exifAt = -1;
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
            static const QByteArrayView xmpMark("http://ns.adobe.com/xap/1.0/\0", 29);
            if (marker == 0xE1 && length >= 14 && d.sliced(at + 4, 6) == QByteArrayView("Exif\0\0", 6) && exifAt < 0)
                exifAt = at;
            else if (marker == 0xE1 && length > 2 + xmpMark.size() && d.sliced(at + 4, xmpMark.size()) == xmpMark)
                *xmp = d.sliced(at + 4 + xmpMark.size(), length - 2 - xmpMark.size());
            at += 2 + length;
        }
        if (exifAt < 0)
            return {};
        return fromTiff(d.sliced(exifAt + 10, (quint8(d[exifAt + 2]) << 8 | quint8(d[exifAt + 3])) - 8));
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

// An XMP field, written either way XMP allows: as an attribute, or as an element of its own.
QString xmpField(QByteArrayView xml, const char *name)
{
    const QString text = QString::fromUtf8(xml);
    const QRegularExpression field(QStringLiteral("%1\\s*(?:=\\s*\"([^\"]*)\"|>([^<]*)<)").arg(QRegularExpression::escape(QLatin1String(name))));
    const QRegularExpressionMatch m = field.match(text);
    if (!m.hasMatch())
        return {};
    QString value = m.captured(1).isNull() ? m.captured(2) : m.captured(1);
    value.replace(QLatin1String("&amp;"), QLatin1String("&")).replace(QLatin1String("&quot;"), QLatin1String("\"")).replace(QLatin1String("&lt;"), QLatin1String("<"));
    return value.simplified().left(200);
}

} // namespace

void readXmp(QByteArrayView xmp, Info &info)
{
    if (info.xmpTool.isEmpty())
        info.xmpTool = xmpField(xmp, "xmp:CreatorTool");
    // xmpMM:History: what each program did, and when.
    const qsizetype start = xmp.indexOf(QByteArrayView("xmpMM:History"));
    const qsizetype end = start >= 0 ? xmp.indexOf(QByteArrayView("</xmpMM:History>"), start) : -1;
    if (end <= start)
        return;
    const QByteArrayView history = xmp.sliced(start, end - start);
    for (qsizetype at = history.indexOf(QByteArrayView("<rdf:li")); at >= 0 && info.history.size() < 30;) {
        const qsizetype next = history.indexOf(QByteArrayView("<rdf:li"), at + 7);
        const QByteArrayView item = history.sliced(at, (next < 0 ? history.size() : next) - at);
        const QString action = xmpField(item, "stEvt:action"), agent = xmpField(item, "stEvt:softwareAgent");
        const QDateTime when = QDateTime::fromString(xmpField(item, "stEvt:when").left(19), Qt::ISODate);
        if (!action.isEmpty()) {
            QString line = action;
            if (!agent.isEmpty())
                line = tr("%1 with %2").arg(action, agent);
            if (when.isValid())
                line += QStringLiteral(", ") + QLocale().toString(when, QLocale::ShortFormat);
            info.history << line;
        }
        at = next;
    }
}

QStringList editSigns(const Info &i)
{
    QStringList signs;
    // Editing programs set the "changed" time; cameras set it to when the photo was taken.
    if (i.changed.isValid() && i.taken.isValid() && i.changed.toSecsSinceEpoch() > i.taken.toSecsSinceEpoch() + 60)
        signs << tr("it was changed on %1, after it was taken").arg(QLocale().toString(i.changed, QLocale::ShortFormat));
    static const QRegularExpression editors(
        QStringLiteral("photoshop|lightroom|gimp|paint\\.net|affinity|pixelmator|snapseed|picasa|canva|luminar|capture one|darktable|"
                       "rawtherapee|acdsee|photoscape|fotor|polarr|vsco|facetune|meitu|photo editor|faceapp"),
        QRegularExpression::CaseInsensitiveOption);
    for (const QString &tool : {i.software, i.xmpTool}) {
        if (!tool.isEmpty() && editors.match(tool).hasMatch()) {
            signs << tr("it was saved by %1, an editing program").arg(tool);
            break;
        }
    }
    int edits = 0;
    for (const QString &h : i.history)
        edits += !h.startsWith(QLatin1String("created"));
    if (edits > 0)
        signs << QCoreApplication::translate("exif", "its own record lists %n change(s) made to it", nullptr, edits);
    return signs;
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
        for (const QString &line : places::facts(i.latitude, i.longitude, i.hasAltitude, i.altitude))
            out << line;
    }
    if (i.hasDirection) {
        static const char *const points[] = {QT_TRANSLATE_NOOP("exif", "north"), QT_TRANSLATE_NOOP("exif", "north-east"),
                                             QT_TRANSLATE_NOOP("exif", "east"), QT_TRANSLATE_NOOP("exif", "south-east"),
                                             QT_TRANSLATE_NOOP("exif", "south"), QT_TRANSLATE_NOOP("exif", "south-west"),
                                             QT_TRANSLATE_NOOP("exif", "west"), QT_TRANSLATE_NOOP("exif", "north-west")};
        const QString point = tr(points[int(std::fmod(i.direction + 22.5, 360.0) / 45) % 8]);
        add(tr("Facing"), i.directionMagnetic ? tr("%1 (%2° from magnetic north)").arg(point).arg(qRound(i.direction))
                                              : tr("%1 (%2°)").arg(point).arg(qRound(i.direction)));
    }
    if (i.satelliteTime.isValid()) {
        QString when = QLocale().toString(i.satelliteTime, QLocale::ShortFormat) + QStringLiteral(" UTC");
        // The camera's clock against the satellites': its time zone, or how far out it was.
        if (i.taken.isValid()) {
            const qint64 apart = QDateTime(i.taken.date(), i.taken.time(), QTimeZone::UTC).toSecsSinceEpoch() - i.satelliteTime.toSecsSinceEpoch();
            const qint64 quarters = qRound64(apart / 900.0);
            if (!i.takenHasZone && std::abs(apart - quarters * 900) <= 120 && std::abs(quarters) <= 56)
                when += QStringLiteral(" · ") + tr("the camera's clock was on UTC%1%2:%3")
                                                    .arg(quarters < 0 ? QLatin1Char('-') : QLatin1Char('+'))
                                                    .arg(std::abs(quarters) / 4, 2, 10, QLatin1Char('0'))
                                                    .arg(std::abs(quarters) % 4 * 15, 2, 10, QLatin1Char('0'));
            else if (i.takenHasZone && std::abs(i.taken.toSecsSinceEpoch() - i.satelliteTime.toSecsSinceEpoch()) > 120)
                when += QStringLiteral(" · ") + tr("the camera's clock was %1 min out").arg(qRound64((i.taken.toSecsSinceEpoch() - i.satelliteTime.toSecsSinceEpoch()) / 60.0));
        }
        add(tr("Satellite time"), when);
    }
    add(tr("Camera serial"), i.serial);
    add(tr("Lens serial"), i.lensSerial);
    add(tr("Owner"), i.owner);
    add(tr("Edited with"), i.software);
    if (!i.xmpTool.isEmpty() && i.xmpTool != i.software)
        add(tr("Made with"), i.xmpTool);
    add(tr("By"), i.artist);
    if (!i.history.isEmpty())
        add(tr("Its history"), i.history.join(QStringLiteral("; ")));
    const QStringList signs = editSigns(i);
    if (!signs.isEmpty())
        add(tr("Changed since"), signs.join(QStringLiteral("; ")));
    return out;
}

} // namespace exif
