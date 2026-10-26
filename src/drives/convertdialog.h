// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "partrecover.h"
#include "tableconvert.h"
#include "udisks.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;

// Change a drive's partition table between MBR and GPT, keeping every partition and what's in
// it: what will change is shown first, the drive's name is typed to go ahead, and afterwards the
// old table can be put back with Undo (it's also kept in Recover Partitions' list).
class ConvertTableDialog : public QDialog
{
    Q_OBJECT
public:
    ConvertTableDialog(UDisks *udisks, const Disk &disk, QWidget *parent = nullptr);
    QString target() const { return m_plan.to.table; } // "gpt" or "dos"

    // The tests: change it, or put it back, without the questions.
    void change();
    void undo();

signals:
    void done(bool ok, const QString &message);

private:
    void write(const recover::Layout &layout, bool undoing);
    void waitFor(const QString &table, int partitions, bool undoing);
    void updateState();

    UDisks *m_udisks;
    QString m_blockPath, m_device, m_driveKey;
    recover::Layout m_before;
    tableconvert::Plan m_plan;
    QLabel *m_changes, *m_status;
    QLineEdit *m_confirm;
    QPushButton *m_change, *m_undo;
    bool m_busy = false, m_changed = false;
};
