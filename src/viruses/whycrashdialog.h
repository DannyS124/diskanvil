// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Why Did It Crash?: the picked Windows' crashes and sudden stops (whycrash.h), read on a thread:
// what they have in common at the top, then each one, newest first, with what it means.

#include "whycrash.h"

#include <QDialog>

#include <memory>

class QLabel;
class QPushButton;
class QTextBrowser;
class QTreeWidget;

class WhyCrashDialog : public QDialog
{
    Q_OBJECT
public:
    // `root`: where that Windows is mounted (empty: the running one).
    explicit WhyCrashDialog(const QString &root, QWidget *parent = nullptr);
    ~WhyCrashDialog() override;

    // The tests: a report to show instead of reading one.
    void showReport(const whycrash::Report &report);
    const whycrash::Report &report() const { return m_report; }

signals:
    void shown();

private:
    void showIncident(int index);

    QLabel *m_summary;
    QTreeWidget *m_list;
    QTextBrowser *m_detail;
    QPushButton *m_save;
    whycrash::Report m_report;
    std::shared_ptr<std::atomic<bool>> m_cancel = std::make_shared<std::atomic<bool>>(false);
};
