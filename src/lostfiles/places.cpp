// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "places.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QLocale>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace places {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("places", text);
}

struct City {
    QString name, region;
    QString country; // its code
    double latitude, longitude;
};

// The list, sorted by latitude (it's kept that way in the file).
const QVector<City> &cities()
{
    static const QVector<City> list = [] {
        QVector<City> out;
        QFile file(QStringLiteral(":/places/cities.tsv"));
        if (!file.open(QIODevice::ReadOnly))
            return out;
        const QList<QByteArray> lines = file.readAll().split('\n');
        out.reserve(lines.size());
        for (const QByteArray &line : lines) {
            const QList<QByteArray> f = line.split('\t');
            if (f.size() < 5)
                continue;
            out.push_back({QString::fromUtf8(f[0]), QString::fromUtf8(f[2]), QString::fromLatin1(f[1]), f[3].toDouble(), f[4].toDouble()});
        }
        std::sort(out.begin(), out.end(), [](const City &a, const City &b) { return a.latitude < b.latitude; });
        return out;
    }();
    return list;
}

const QHash<QString, QString> &countries()
{
    static const QHash<QString, QString> names = [] {
        QHash<QString, QString> out;
        QFile file(QStringLiteral(":/places/countries.tsv"));
        if (file.open(QIODevice::ReadOnly)) {
            for (const QByteArray &line : file.readAll().split('\n')) {
                const qsizetype tab = line.indexOf('\t');
                if (tab > 0)
                    out.insert(QString::fromLatin1(line.left(tab)), QString::fromUtf8(line.mid(tab + 1)));
            }
        }
        return out;
    }();
    return names;
}

QVector<Ring> ringsFrom(const QString &resource)
{
    QFile file(resource);
    return file.open(QIODevice::ReadOnly) ? readRings(file.readAll()) : QVector<Ring>();
}

constexpr double kFarthest = 250; // km

} // namespace

double kilometres(double latitude1, double longitude1, double latitude2, double longitude2)
{
    constexpr double radius = 6371.0088, toRadians = std::numbers::pi / 180;
    const double a = std::pow(std::sin((latitude2 - latitude1) * toRadians / 2), 2)
                   + std::cos(latitude1 * toRadians) * std::cos(latitude2 * toRadians) * std::pow(std::sin((longitude2 - longitude1) * toRadians / 2), 2);
    return 2 * radius * std::asin(std::min(1.0, std::sqrt(a)));
}

Place nearest(double latitude, double longitude)
{
    Place best;
    if (!std::isfinite(latitude) || !std::isfinite(longitude) || std::abs(latitude) > 90 || std::abs(longitude) > 180)
        return best;
    const QVector<City> &list = cities();
    // Only the ones within reach north and south need measuring (a degree of latitude is 111 km).
    const double band = kFarthest / 111.0 + 0.1;
    auto it = std::lower_bound(list.cbegin(), list.cend(), latitude - band, [](const City &c, double v) { return c.latitude < v; });
    for (; it != list.cend() && it->latitude <= latitude + band; ++it) {
        const double km = kilometres(latitude, longitude, it->latitude, it->longitude);
        if (km <= kFarthest && (!best.valid() || km < best.kilometres)) {
            best.name = it->name;
            best.region = it->region;
            best.countryCode = it->country;
            best.latitude = it->latitude;
            best.longitude = it->longitude;
            best.kilometres = km;
        }
    }
    if (best.valid())
        best.country = countries().value(best.countryCode, best.countryCode);
    return best;
}

QString describe(const Place &place)
{
    if (!place.valid())
        return QString();
    QString where = place.name;
    if (!place.region.isEmpty() && place.region != place.name)
        where += QStringLiteral(", ") + place.region;
    if (!place.country.isEmpty())
        where += QStringLiteral(", ") + place.country;
    if (place.kilometres < 2)
        return where;
    return tr("near %1 (%2 km away)").arg(where, QLocale().toString(place.kilometres, 'f', 0));
}

QString coordinates(double latitude, double longitude)
{
    return tr("%1° %2, %3° %4")
        .arg(QLocale().toString(std::abs(latitude), 'f', 5), latitude < 0 ? tr("S") : tr("N"), QLocale().toString(std::abs(longitude), 'f', 5),
             longitude < 0 ? tr("W") : tr("E"));
}

QStringList facts(double latitude, double longitude, bool hasAltitude, double altitude)
{
    QStringList out;
    const QString where = describe(nearest(latitude, longitude));
    if (!where.isEmpty())
        out << tr("Where") + QLatin1Char('\t') + where;
    QString numbers = coordinates(latitude, longitude);
    if (hasAltitude)
        numbers += QLatin1String(", ") + (altitude < 0 ? tr("%1 m below the sea").arg(qRound64(-altitude)) : tr("%1 m up").arg(qRound64(altitude)));
    out << tr("Coordinates") + QLatin1Char('\t') + numbers;
    out << tr("Map") + QStringLiteral("\tgeo:%1,%2").arg(QString::number(latitude, 'f', 6), QString::number(longitude, 'f', 6));
    return out;
}

QString mapLink(double latitude, double longitude)
{
    const QString lat = QString::number(latitude, 'f', 6), lon = QString::number(longitude, 'f', 6);
    return QStringLiteral("https://www.openstreetmap.org/?mlat=%1&mlon=%2#map=15/%1/%2").arg(lat, lon);
}

QVector<Ring> readRings(const QByteArray &data)
{
    QVector<Ring> out;
    if (!data.startsWith(QByteArrayView("DAMAP1\0\0", 8)))
        return out;
    const auto *p = reinterpret_cast<const uchar *>(data.constData());
    qsizetype at = 8;
    while (at + 4 <= data.size()) {
        const quint32 count = qFromLittleEndian<quint32>(p + at);
        at += 4;
        if (count == 0 || quint64(count) * 4 > quint64(data.size() - at))
            break;
        Ring ring;
        ring.reserve(qsizetype(count));
        for (quint32 i = 0; i < count; ++i, at += 4)
            ring.push_back(QPointF(qFromLittleEndian<qint16>(p + at) / 100.0, qFromLittleEndian<qint16>(p + at + 2) / 100.0));
        out.push_back(std::move(ring));
    }
    return out;
}

const QVector<Ring> &land()
{
    static const QVector<Ring> rings = ringsFrom(QStringLiteral(":/places/land.bin"));
    return rings;
}

const QVector<Ring> &borders()
{
    static const QVector<Ring> rings = ringsFrom(QStringLiteral(":/places/borders.bin"));
    return rings;
}

} // namespace places
