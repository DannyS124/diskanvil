// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// "What This PC Did" (activity.h): what a Windows was, and what it was used for, newest first,
// filtered by kind and words, and saved as a report.

#include "activity.h"

#include <QDialog>

#include <atomic>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QThread;
class QTreeWidget;

class ActivityDialog : public QDialog
{
    Q_OBJECT
public:
    // `root`: a drive with Windows on it; empty in the Windows version: the Windows that's running.
    explicit ActivityDialog(const QString &root, QWidget *parent = nullptr);
    ~ActivityDialog() override;
    // For the tests.
    void waitUntilRead();
    int shown() const;

private:
    void fill();
    void saveReport();
    QVector<activity::Event> visible() const;

    QString m_root;
    activity::Report m_report;
    QLabel *m_overview;
    QComboBox *m_kind;
    QLineEdit *m_words;
    QTreeWidget *m_list;
    QLabel *m_status;
    QPushButton *m_save;
    QThread *m_thread = nullptr;
    std::atomic<bool> m_stop{false};
};
