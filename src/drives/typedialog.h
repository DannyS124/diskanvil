// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Partition type and flags. The type is a label other systems and installers go by (it
// doesn't change what's on the partition); the flags are the GPT attributes or the MBR
// boot flag.

#include "udisks.h"

#include <QDialog>
#include <QMap>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

class PartitionTypeDialog : public QDialog
{
    Q_OBJECT
public:
    PartitionTypeDialog(const Disk &disk, const Volume &volume, QWidget *parent = nullptr);
    QString type() const; // the chosen type
    quint64 flags() const; // what to set: the chosen flags plus any others it already had
    bool typeChanged() const;
    bool flagsChanged() const;

private:
    void refresh();

    QString m_table;
    QString m_oldType;
    quint64 m_oldFlags;
    bool m_driveBoots = false; // has an EFI System or BIOS boot partition
    QComboBox *m_types;
    QLineEdit *m_other;
    QLabel *m_problem;
    QMap<quint64, QCheckBox *> m_flags;
    QLabel *m_warning;
    QPushButton *m_ok = nullptr;
};
