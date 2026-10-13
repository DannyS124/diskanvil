// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Who's Using It?: the programs keeping a drive from unmounting (holders.h), each with Close It
// (asked politely) and Stop It (straight away); Look as Admin for other users' programs, and
// Try Again, which unmounts it.

#include "holders.h"
#include "udisks.h"

#include <QDialog>
#include <functional>

class QLabel;
class QPushButton;
class QTreeWidget;

class HoldersDialog : public QDialog
{
    Q_OBJECT
public:
    HoldersDialog(UDisks *udisks, const Volume &volume, QWidget *parent = nullptr);

    void look(bool asAdmin = false);
    // The tests: what's shown, and signals and root commands caught instead of sent.
    const holders::Report &report() const { return m_report; }
    std::function<bool(const QStringList &command, QString *said)> runAsRoot;
    std::function<bool(qint64 pid, int signal)> sendSignal;

private:
    void showReport();
    void stop(bool force);

    UDisks *m_udisks;
    Volume m_volume;
    QLabel *m_summary;
    QTreeWidget *m_list;
    QPushButton *m_close, *m_kill, *m_admin, *m_again;
    holders::Report m_report;
};
