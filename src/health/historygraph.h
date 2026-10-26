// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "healthhistory.h"

#include <QDialog>
#include <QWidget>

// One line of a drive's health history, small: its name and newest value on top, the line under
// it, the first and last day along the bottom. A line that went up where up is bad is drawn in
// the warning colour.
class HistoryGraph : public QWidget
{
    Q_OBJECT
public:
    explicit HistoryGraph(const healthhistory::Series &series, QWidget *parent = nullptr);
    QSize sizeHint() const override { return {260, 96}; }
    QString valueText(double v) const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    healthhistory::Series m_series;
};

class QCheckBox;
class QLabel;

// A drive's health history: a graph for each thing it reports, and the daily background check
// (for every drive) switched on or off.
class HealthHistoryDialog : public QDialog
{
    Q_OBJECT
public:
    HealthHistoryDialog(const Disk &disk, QWidget *parent = nullptr);
    int graphCount() const { return m_graphs; }

private:
    QCheckBox *m_daily;
    QLabel *m_dailyNote;
    int m_graphs = 0;
};
