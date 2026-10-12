// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Where a spot on Earth is, in words and on a map, without going online: the nearest town or city
// from GeoNames' list of those with over 15,000 people (built into DiskAnvil; CC BY 4.0, see
// data/places/ATTRIBUTION.txt), and the land and borders from Natural Earth (public domain) to
// draw a map with.

#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>

namespace places {

struct Place {
    QString name, region, country, countryCode;
    double latitude = 0, longitude = 0;
    double kilometres = -1; // from the spot asked about; -1 when there's none near
    bool valid() const { return kilometres >= 0; }
};

// The nearest town or city to a spot, within 250 km (farther out it's sea, or the wilds, and a
// name would mislead).
Place nearest(double latitude, double longitude);
// "Leeds, England, United Kingdom", or "near Leeds, ... (12 km)" when it's a way out of town.
QString describe(const Place &place);
// "53.80085° N, 1.54907° W".
QString coordinates(double latitude, double longitude);
// The spot on OpenStreetMap's website, for the user to open if they want (nothing goes online
// by itself).
QString mapLink(double latitude, double longitude);
// The lines to show for a spot: "Where" (the place's name, when there's one near),
// "Coordinates" (with its height, when it's known) and "Map" (a geo: link to it, which windows
// show as a way to open it on a map), as "label\tvalue".
QStringList facts(double latitude, double longitude, bool hasAltitude = false, double altitude = 0);
// How far apart two spots are, over the Earth's surface.
double kilometres(double latitude1, double longitude1, double latitude2, double longitude2);

// The outlines to draw a world map with: x is the longitude, y the latitude, in degrees.
using Ring = QVector<QPointF>;
const QVector<Ring> &land();
const QVector<Ring> &borders();
// Rings from a map file (the format is in data/places/ATTRIBUTION.txt). Exposed for the tests.
QVector<Ring> readRings(const QByteArray &data);

} // namespace places
