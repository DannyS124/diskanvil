// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Inspect Partition Table: the MBR, both GPT headers and the partition list as they are on
// the drive, whether their checksums are right, and any sector in hex. Read-only: the drive
// is opened like for a benchmark, so nothing gets unmounted (the system disk works too).

#include "gpt.h"
#include "udisks.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

class TableInspectorDialog : public QDialog
{
    Q_OBJECT
public:
    TableInspectorDialog(UDisks *udisks, const Disk &disk, QWidget *parent = nullptr);
    // An image file (or device) that's already open; takes the fd.
    TableInspectorDialog(int fd, const QString &title, QWidget *parent = nullptr);
    ~TableInspectorDialog() override;
    const gpt::Report &report() const { return m_report; }
    bool isReady() const { return m_fd >= 0; }

signals:
    void ready();

private:
    void build(const QString &title);
    void load(int fd);
    void fill();
    void addHeader(const QString &title, const gpt::Header &h);
    void showSector(quint64 lba);

    QString m_device;
    int m_fd = -1;
    gpt::Report m_report;
    QLabel *m_summary;
    QTreeWidget *m_tree;
    QLineEdit *m_lba;
    QPlainTextEdit *m_hex;
    QList<QPushButton *> m_jumps;
};
