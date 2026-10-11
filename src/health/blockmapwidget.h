// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "blockmapdata.h"

#include <QWidget>

#include <optional>

// The grid of small squares that fills in green, orange and red as a scan or rescue
// goes along, like the surface view in Victoria or HD Tune.
class BlockMapWidget : public QWidget
{
    Q_OBJECT
public:
    struct Area {
        quint64 start = 0;
        quint64 end = 0;
        QString name;
    };

    explicit BlockMapWidget(QWidget *parent = nullptr);

    void reset(quint64 size, bool rotational);
    BlockMapData &data() { return m_data; }
    const BlockMapData &data() const { return m_data; }
    // Partitions, named in the tooltip.
    void setAreas(const QVector<Area> &areas) { m_areas = areas; }
    // Rescue Copy calls its tried-but-not-finished cells "to retry"; a scan has none.
    void setShowPending(bool show) { m_showPending = show; }
    // Words of its own for the legend and tooltips (Check a USB Stick writes before it reads,
    // so "pending" is "written, not checked yet"). Shows the pending colour too.
    void setLabels(const QString &good, const QString &pending, const QString &bad, const QString &unread);
    void addSamples(const QVector<ReadSample> &samples);
    void refresh() { update(); }

    static QColor colour(BlockMapData::State state, const QPalette &palette);

    QSize sizeHint() const override { return {560, 260}; }
    QSize minimumSizeHint() const override { return {320, 140}; }

protected:
    void paintEvent(QPaintEvent *event) override;
    bool event(QEvent *event) override;

private:
    struct Grid {
        int columns = 1;
        int cell = 1; // square size including the 1 px gap
        QRect area;
    };
    Grid grid() const;
    int legendHeight() const;
    int cellAt(const QPoint &pos) const;

    BlockMapData m_data;
    QVector<Area> m_areas;
    bool m_showPending = false;
    struct Labels {
        QString good, pending, bad, unread;
    };
    std::optional<Labels> m_labels;
};
