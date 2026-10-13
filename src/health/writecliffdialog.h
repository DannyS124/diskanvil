// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Write Speed Over Time (writecliff.h): a drive, where to write (its free space, or all of an
// empty drive) and how much, then the speed drawn as it goes, and what it means.

#include "writecliff.h"

#include <QDialog>
#include <QPointer>

class QComboBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QSpinBox;
class QPushButton;
class QThread;
class UDisks;
struct Disk;

namespace writecliff {
class Graph;
}

class WriteCliffDialog : public QDialog
{
    Q_OBJECT
public:
    WriteCliffDialog(UDisks *udisks, const QString &preferredDisk = {}, QWidget *parent = nullptr);
    ~WriteCliffDialog() override;

    static bool allowLoopDevicesForTest;
    const QVector<writecliff::Point> &points() const { return m_points; }

signals:
    void ended(bool completed); // (the tests)

protected:
    void reject() override;

private:
    void fillTargets(const QString &preferred);
    const Disk *target() const;
    QString freeSpaceFolder(quint64 *free) const; // a mounted, writable volume of the drive
    void updateState();
    void start();
    void done(bool completed, const QString &error);

    UDisks *m_udisks;
    QComboBox *m_targets;
    QRadioButton *m_inFree;
    QRadioButton *m_whole;
    QSpinBox *m_amount;
    QLabel *m_where;
    QLineEdit *m_confirm;
    QPushButton *m_start;
    writecliff::Graph *m_graph;
    QLabel *m_result;
    QVector<writecliff::Point> m_points;
    QPointer<writecliff::Writer> m_writer;
    QThread *m_thread = nullptr;
    bool m_running = false;
};
