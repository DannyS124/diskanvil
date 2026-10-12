// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Make Room for Windows Recovery (winre.h), on the Windows that's running.

#include "winre.h"

#include <QDialog>

#include <functional>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class UDisks;

class WinReDialog : public QDialog
{
    Q_OBJECT
public:
    explicit WinReDialog(UDisks *udisks, QWidget *parent = nullptr);

private:
    void look();
    void makeRoom();
    // Runs the programs one after another, the output shown; `then` with whether each worked.
    void runSteps(const QVector<QStringList> &steps, const std::function<void(const QVector<bool> &)> &then, QVector<bool> done = {});

    UDisks *m_udisks;
    QLabel *m_state;
    QPlainTextEdit *m_output;
    QPushButton *m_fix;
    winre::Plan m_plan;
    bool m_running = false;
};
