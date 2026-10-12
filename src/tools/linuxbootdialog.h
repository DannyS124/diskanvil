// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Repair Linux Boot (linuxboot.h): finds the Linux installs on the drives, says what repairing
// one does, and does it, showing what its tools say.

#include "linuxboot.h"

#include <QDialog>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class UDisks;

class LinuxBootDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LinuxBootDialog(UDisks *udisks, QWidget *parent = nullptr);
    ~LinuxBootDialog() override;

protected:
    void reject() override;

private:
    void lookNext();
    void lookAt(const Volume &volume);
    void showInstall(int row);
    void repair();

    UDisks *m_udisks;
    QListWidget *m_list;
    QLabel *m_plan;
    QPlainTextEdit *m_output;
    QPushButton *m_repair;
    QStringList m_toLook;            // volumes (object paths) still to look at
    QStringList m_mountedHere;       // the ones mounted to look, unmounted again at the end
    QVector<linuxboot::Install> m_installs;
    QProcess *m_run = nullptr;
};
