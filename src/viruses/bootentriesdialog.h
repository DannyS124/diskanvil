// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Boot Menu Entries: the PC's UEFI boot entries (efiboot.h) in order, whether what each starts
// is there, and changes to them: the order, the one to start next time, switching one off,
// removing one, adding one for a loader on an EFI partition. And a report of how the PC starts.

#include "efiboot.h"

#include <QDialog>

#include <functional>

class QLabel;
class QPushButton;
class QTreeWidget;
class UDisks;

class BootEntriesDialog : public QDialog
{
    Q_OBJECT
public:
    explicit BootEntriesDialog(UDisks *udisks, QWidget *parent = nullptr);
    // For the tests: runs instead of the change (Linux: efibootmgr's command). True for done.
    static inline std::function<bool(const QStringList &command, QString *said)> runForTest;

    // How the PC starts, in plain text: the firmware, Secure Boot, the entries, the EFI
    // partitions and the loaders on them, the systems found.
    static QString report(UDisks *udisks, const efiboot::State &state);

private:
    void reload();
    void fill();
    int selectedNumber() const;
    bool change(const QStringList &command, const std::function<bool(QString *)> &onWindows);
    void move(int by);
    void saveOrder();
    void addEntry();

    UDisks *m_udisks;
    efiboot::State m_state;
    QVector<int> m_order; // as it's being rearranged
    QTreeWidget *m_list;
    QLabel *m_status;
    QPushButton *m_saveOrder;
};
