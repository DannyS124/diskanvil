// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A world map with pins, drawn from the outlines DiskAnvil carries (places.h), so it works without
// the internet: for where found photos and films were taken. The wheel zooms, dragging moves it,
// pins close together become one with a number, and a click on a pin says which files it holds.

#include <QPainterPath>
#include <QPointF>
#include <QVector>
#include <QWidget>

class PlacesMap : public QWidget
{
    Q_OBJECT
public:
    struct Pin {
        double latitude = 0, longitude = 0;
        int index = -1; // whatever the caller keeps it by
    };

    explicit PlacesMap(QWidget *parent = nullptr);
    void setPins(QVector<Pin> pins);
    // Every pin in view (the whole world when there are none).
    void showAll();
    // A spot, `span` degrees across.
    void showSpot(double latitude, double longitude, double span);
    QSize sizeHint() const override { return {720, 420}; }

Q_SIGNALS:
    // A pin, or a group of them, was clicked: what it holds, and where.
    void picked(const QVector<int> &indexes, double latitude, double longitude);

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool event(QEvent *event) override;

private:
    struct Group {
        QPointF at; // on screen
        QVector<int> pins;
        double latitude = 0, longitude = 0; // their middle
    };
    QVector<Group> groups() const;
    int groupAt(const QPointF &point, const QVector<Group> &all) const;
    QPointF toScreen(double latitude, double longitude) const;
    QPointF toMap(const QPointF &screen) const; // x longitude, y latitude
    void zoom(double factor, const QPointF &around);
    void clampView();

    QVector<Pin> m_pins;
    QPainterPath m_land, m_borders; // in degrees
    QPointF m_centre{10, 25};       // longitude, latitude
    double m_degreesPerPixel = 0.5;
    bool m_dragging = false, m_dragged = false;
    QPointF m_dragFrom, m_dragCentre;
};

// Asks, then shows a spot on OpenStreetMap's website in the web browser. Nothing goes online unless
// the person says yes.
void openOnOpenStreetMap(QWidget *parent, double latitude, double longitude);
// "geo:53.80080,-1.54910" (the Map line places::facts() gives), as a spot; false when it isn't one.
bool parseGeo(const QString &uri, double *latitude, double *longitude);
