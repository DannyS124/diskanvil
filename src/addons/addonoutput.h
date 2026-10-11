// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Runs an add-on's command and shows what it prints, for actions with "output": "window".
// The command gets its own session (process group), so Stop ends everything it started,
// and no file descriptors besides its output.

#include "outputfilter.h"

#include <QDialog>
#include <QProcess>
#include <QStringList>
#include <QTimer>

class QLabel;
class QPlainTextEdit;
class QPushButton;

class AddonOutputWindow : public QDialog
{
    Q_OBJECT
public:
    AddonOutputWindow(const QString &title, const QStringList &command, QWidget *parent = nullptr);
    ~AddonOutputWindow() override;
    bool isRunning() const { return m_process.state() != QProcess::NotRunning; }
    void stop();
    QString text() const; // everything shown so far

protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;

private:
    void readOutput();
    void flush();
    void finished(int code, QProcess::ExitStatus status);
    void signalGroup(int signal);

    QProcess m_process;
    OutputFilter m_filter;
    QStringList m_pending;
    int m_skipped = 0;
    QString m_shownCurrent; // the unfinished line shown as the last block
    bool m_stopped = false;
    QString m_title;
    QPlainTextEdit *m_text;
    QLabel *m_status;
    QPushButton *m_stop;
    QTimer m_flush;
    QTimer m_kill;
};
