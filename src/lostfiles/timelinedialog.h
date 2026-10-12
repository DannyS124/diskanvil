// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The timeline (timeline.h) of what Find Lost Files found: filtered by dates, words and deleted
// or not, and saved as CSV or a PDF.

#include "timeline.h"

#include <QDialog>

class QCheckBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QTableView;
class TimelineModel;

class TimelineDialog : public QDialog
{
    Q_OBJECT
public:
    TimelineDialog(const QVector<lost::Found> &files, const QString &title, QWidget *parent = nullptr);
    int shown() const; // events that pass the filters

private:
    void filter();
    QVector<timeline::Event> visible() const;

    QVector<lost::Found> m_files;
    QVector<timeline::Event> m_all;
    QString m_title;
    TimelineModel *m_model;
    QTableView *m_view;
    QDateEdit *m_from, *m_to;
    QLineEdit *m_words;
    QCheckBox *m_deletedOnly;
    QLabel *m_count;
};
