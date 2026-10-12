// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Windows Drivers and Services: everything that starts with a Windows, newest first, with who
// made it. When Windows stops starting (a blue screen right after something was installed),
// the newest driver is the usual suspect: switched off, with a backup and a note saying how to
// put it back. Works on a Windows on a drive (DiskAnvil Live, or another drive) and on the
// running one (the Windows version).

#include "winstartup.h"

#include <QDateTime>
#include <QDialog>

class QCheckBox;
class QLabel;
class QPushButton;
class QTreeWidget;
class QThread;

class DriversDialog : public QDialog
{
    Q_OBJECT
public:
    // `root`: where the Windows is mounted; empty for the running one.
    DriversDialog(const QString &root, QWidget *parent = nullptr);
    ~DriversDialog() override;

    struct Row {
        int entry = -1;       // in the report
        QDateTime changed;    // its file's date
        QString maker;        // the company in its version resource, else its signer
        QString description;
        QString signer;
        bool windowsOwn = false;
    };

private:
    void load();
    void fill();
    void switchOff();

    QString m_root;
    QTreeWidget *m_list;
    QCheckBox *m_hideWindows;
    QLabel *m_status;
    QPushButton *m_switchOff;
    QThread *m_thread = nullptr;
    startup::Report m_report;
    QVector<Row> m_rows;
    QHash<int, QString> m_done; // entry -> what happened when it was switched off
};
