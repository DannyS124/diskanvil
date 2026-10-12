// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "placesmap.h"

#include "places.h"

#include <QDesktopServices>
#include <QHash>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {

constexpr double kGroupPixels = 22; // pins closer than this on screen are one

QColor mix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(float(a.redF() * (1 - t) + b.redF() * t), float(a.greenF() * (1 - t) + b.greenF() * t),
                            float(a.blueF() * (1 - t) + b.blueF() * t));
}

QPainterPath pathOf(const QVector<places::Ring> &rings, bool closed)
{
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    for (const places::Ring &ring : rings) {
        if (ring.size() < 2)
            continue;
        path.moveTo(ring.first());
        for (qsizetype i = 1; i < ring.size(); ++i)
            path.lineTo(ring[i]);
        if (closed)
            path.closeSubpath();
    }
    return path;
}

} // namespace

PlacesMap::PlacesMap(QWidget *parent)
    : QWidget(parent)
    , m_land(pathOf(places::land(), true))
    , m_borders(pathOf(places::borders(), false))
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 200);
    setCursor(Qt::OpenHandCursor);
}

void PlacesMap::setPins(QVector<Pin> pins)
{
    m_pins = std::move(pins);
    update();
}

void PlacesMap::showAll()
{
    if (m_pins.isEmpty()) {
        m_centre = QPointF(10, 25);
        m_degreesPerPixel = 360.0 / std::max(width(), 320);
        clampView();
        update();
        return;
    }
    double north = -90, south = 90, east = -180, west = 180;
    for (const Pin &p : std::as_const(m_pins)) {
        north = std::max(north, p.latitude);
        south = std::min(south, p.latitude);
        east = std::max(east, p.longitude);
        west = std::min(west, p.longitude);
    }
    showSpot((north + south) / 2, (east + west) / 2, std::max({east - west, (north - south) * 1.6, 0.05}) * 1.3);
}

void PlacesMap::showSpot(double latitude, double longitude, double span)
{
    m_centre = QPointF(longitude, latitude);
    m_degreesPerPixel = span / std::max(width(), 320);
    clampView();
    update();
}

void PlacesMap::clampView()
{
    m_degreesPerPixel = std::clamp(m_degreesPerPixel, 0.00002, 400.0 / std::max(width(), 320));
    m_centre.setY(std::clamp(m_centre.y(), -85.0, 85.0));
    double x = std::fmod(m_centre.x() + 180, 360);
    if (x < 0)
        x += 360;
    m_centre.setX(x - 180);
}

QPointF PlacesMap::toScreen(double latitude, double longitude) const
{
    // Plain longitude and latitude, east-west squeezed by the latitude in the middle so shapes
    // there look right.
    const double squeeze = std::cos(std::clamp(m_centre.y(), -70.0, 70.0) * std::numbers::pi / 180);
    double dx = longitude - m_centre.x();
    if (dx > 180)
        dx -= 360;
    else if (dx < -180)
        dx += 360;
    return {width() / 2.0 + dx * squeeze / m_degreesPerPixel, height() / 2.0 - (latitude - m_centre.y()) / m_degreesPerPixel};
}

QPointF PlacesMap::toMap(const QPointF &screen) const
{
    const double squeeze = std::cos(std::clamp(m_centre.y(), -70.0, 70.0) * std::numbers::pi / 180);
    return {m_centre.x() + (screen.x() - width() / 2.0) * m_degreesPerPixel / squeeze,
            m_centre.y() - (screen.y() - height() / 2.0) * m_degreesPerPixel};
}

QVector<PlacesMap::Group> PlacesMap::groups() const
{
    QVector<Group> out;
    QHash<quint64, QVector<int>> cells; // group numbers by the grid cell they're in
    const auto key = [](int x, int y) { return quint64(quint32(x)) << 32 | quint32(y); };
    for (int i = 0; i < m_pins.size(); ++i) {
        const QPointF at = toScreen(m_pins[i].latitude, m_pins[i].longitude);
        if (at.x() < -40 || at.y() < -40 || at.x() > width() + 40 || at.y() > height() + 40)
            continue;
        const int cx = int(std::floor(at.x() / kGroupPixels)), cy = int(std::floor(at.y() / kGroupPixels));
        int found = -1;
        for (int dx = -1; dx <= 1 && found < 0; ++dx) {
            for (int dy = -1; dy <= 1 && found < 0; ++dy) {
                for (const int g : cells.value(key(cx + dx, cy + dy))) {
                    if (QLineF(out[g].at, at).length() < kGroupPixels) {
                        found = g;
                        break;
                    }
                }
            }
        }
        if (found < 0) {
            found = int(out.size());
            out.push_back({at, {}, 0, 0});
            cells[key(cx, cy)] << found;
        }
        Group &g = out[found];
        g.pins << i;
        g.latitude += (m_pins[i].latitude - g.latitude) / g.pins.size();
        g.longitude += (m_pins[i].longitude - g.longitude) / g.pins.size();
    }
    for (Group &g : out)
        g.at = toScreen(g.latitude, g.longitude);
    return out;
}

int PlacesMap::groupAt(const QPointF &point, const QVector<Group> &all) const
{
    int best = -1;
    double nearest = 14;
    for (int i = 0; i < all.size(); ++i) {
        const double d = QLineF(all[i].at, point).length();
        if (d < nearest) {
            nearest = d;
            best = i;
        }
    }
    return best;
}

void PlacesMap::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette &pal = palette();
    const QColor sea = mix(pal.color(QPalette::Base), pal.color(QPalette::Highlight), 0.14);
    const QColor land = mix(pal.color(QPalette::Window), pal.color(QPalette::Text), 0.12);
    const QColor border = mix(pal.color(QPalette::Window), pal.color(QPalette::Text), 0.38);
    p.fillRect(rect(), sea);

    const double squeeze = std::cos(std::clamp(m_centre.y(), -70.0, 70.0) * std::numbers::pi / 180);
    QPen line(border, 0.8);
    line.setCosmetic(true);
    // The world, and its copies either side when zoomed out far enough to see past 180°.
    for (const double shift : {-360.0, 0.0, 360.0}) {
        QTransform t;
        t.translate(width() / 2.0, height() / 2.0);
        t.scale(squeeze / m_degreesPerPixel, -1.0 / m_degreesPerPixel);
        t.translate(-m_centre.x() + shift, -m_centre.y());
        const QRectF seen = t.inverted().mapRect(QRectF(rect()));
        if (seen.right() < -180 || seen.left() > 180)
            continue;
        p.setTransform(t);
        p.fillPath(m_land, land);
        p.strokePath(m_borders, line);
    }
    p.resetTransform();

    // The pins: one, or a group with how many.
    const QColor pin = pal.color(QPalette::Highlight), pinText = pal.color(QPalette::HighlightedText);
    QFont small = font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() * 0.85));
    small.setBold(true);
    p.setFont(small);
    for (const Group &g : groups()) {
        const double r = g.pins.size() == 1 ? 6 : 9 + std::min(6.0, std::log10(double(g.pins.size())) * 3);
        p.setPen(QPen(mix(pin, Qt::black, 0.35), 1.2));
        p.setBrush(pin);
        p.drawEllipse(g.at, r, r);
        if (g.pins.size() > 1) {
            p.setPen(pinText);
            p.drawText(QRectF(g.at.x() - r, g.at.y() - r, 2 * r, 2 * r), Qt::AlignCenter, QString::number(g.pins.size()));
        }
    }
    // The scale, bottom left.
    const double km = 111.32 * m_degreesPerPixel * 100; // across 100 pixels, along the middle latitude
    p.setPen(pal.color(QPalette::Text));
    p.setFont(font());
    const QString scale = km >= 10 ? tr("%1 km").arg(qRound64(km)) : km >= 1 ? tr("%1 km").arg(km, 0, 'f', 1) : tr("%1 m").arg(qRound64(km * 1000));
    p.drawLine(QPointF(10, height() - 12), QPointF(110, height() - 12));
    p.drawText(QPointF(116, height() - 8), scale);
}

void PlacesMap::zoom(double factor, const QPointF &around)
{
    const QPointF before = toMap(around);
    m_degreesPerPixel /= factor;
    clampView();
    const QPointF after = toMap(around);
    m_centre += before - after;
    clampView();
    update();
}

void PlacesMap::wheelEvent(QWheelEvent *event)
{
    const double steps = event->angleDelta().y() / 120.0;
    if (steps != 0)
        zoom(std::pow(1.3, steps), event->position());
    event->accept();
}

void PlacesMap::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_dragging = true;
    m_dragged = false;
    m_dragFrom = event->position();
    m_dragCentre = m_centre;
    setCursor(Qt::ClosedHandCursor);
}

void PlacesMap::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging)
        return;
    const QPointF moved = event->position() - m_dragFrom;
    if (moved.manhattanLength() > 3)
        m_dragged = true;
    const double squeeze = std::cos(std::clamp(m_dragCentre.y(), -70.0, 70.0) * std::numbers::pi / 180);
    m_centre = m_dragCentre + QPointF(-moved.x() * m_degreesPerPixel / squeeze, moved.y() * m_degreesPerPixel);
    clampView();
    update();
}

void PlacesMap::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging || event->button() != Qt::LeftButton)
        return;
    m_dragging = false;
    setCursor(Qt::OpenHandCursor);
    if (m_dragged)
        return;
    const QVector<Group> all = groups();
    const int g = groupAt(event->position(), all);
    if (g < 0)
        return;
    QVector<int> indexes;
    for (const int i : all[g].pins)
        indexes << m_pins[i].index;
    Q_EMIT picked(indexes, all[g].latitude, all[g].longitude);
}

void PlacesMap::mouseDoubleClickEvent(QMouseEvent *event)
{
    zoom(event->button() == Qt::RightButton ? 0.5 : 2, event->position());
}

void PlacesMap::keyPressEvent(QKeyEvent *event)
{
    const QPointF middle(width() / 2.0, height() / 2.0);
    switch (event->key()) {
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoom(1.5, middle);
        break;
    case Qt::Key_Minus:
        zoom(1 / 1.5, middle);
        break;
    case Qt::Key_0:
    case Qt::Key_Home:
        showAll();
        break;
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down: {
        const double step = 60 * m_degreesPerPixel;
        m_centre += QPointF(event->key() == Qt::Key_Left ? -step : event->key() == Qt::Key_Right ? step : 0,
                            event->key() == Qt::Key_Up ? step : event->key() == Qt::Key_Down ? -step : 0);
        clampView();
        update();
        break;
    }
    default:
        QWidget::keyPressEvent(event);
    }
}

bool PlacesMap::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        const QVector<Group> all = groups();
        const int g = groupAt(help->pos(), all);
        if (g < 0) {
            QToolTip::hideText();
            event->ignore();
            return true;
        }
        const QString where = places::describe(places::nearest(all[g].latitude, all[g].longitude));
        QToolTip::showText(help->globalPos(), tr("%n file(s)", nullptr, int(all[g].pins.size()))
                                                   + (where.isEmpty() ? QString() : QStringLiteral("\n") + where),
                           this);
        return true;
    }
    return QWidget::event(event);
}

void openOnOpenStreetMap(QWidget *parent, double latitude, double longitude)
{
    const QString title = QCoreApplication::translate("PlacesMap", "Open on OpenStreetMap");
    const QString link = places::mapLink(latitude, longitude);
    if (QMessageBox::question(parent, title,
                              QCoreApplication::translate("PlacesMap", "This opens openstreetmap.org in your web browser at %1, so that website is told "
                                                                       "this spot. Nothing else is sent. Open it?")
                                  .arg(places::coordinates(latitude, longitude)),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes)
        return;
    if (!QDesktopServices::openUrl(QUrl(link)))
        QMessageBox::information(parent, title, QCoreApplication::translate("PlacesMap", "No web browser opened. The link is:\n%1").arg(link));
}

bool parseGeo(const QString &uri, double *latitude, double *longitude)
{
    if (!uri.startsWith(QLatin1String("geo:")))
        return false;
    const QStringList parts = uri.mid(4).section(QLatin1Char(';'), 0, 0).split(QLatin1Char(','));
    bool okLat = false, okLon = false;
    *latitude = parts.value(0).toDouble(&okLat);
    *longitude = parts.value(1).toDouble(&okLon);
    return okLat && okLon && std::abs(*latitude) <= 90 && std::abs(*longitude) <= 180;
}
