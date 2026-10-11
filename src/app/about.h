// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class AboutDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AboutDialog(const QString &udisksVersion, QWidget *parent = nullptr);
};

// The handbook (data/help.html), opened with F1.
class HelpWindow : public QDialog
{
    Q_OBJECT
public:
    explicit HelpWindow(QWidget *parent = nullptr);
};
