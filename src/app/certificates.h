// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Certificates of erasure (erasure.h): one for a finished Wipe Disk or Secure Erase, and the
// list of all of them.

#include "erasure.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
class UDisks;

class CertificateDialog : public QDialog
{
    Q_OBJECT
public:
    CertificateDialog(UDisks *udisks, const erasure::Record &record, QWidget *parent = nullptr);
    QString html() const;

private:
    void check();

    UDisks *m_udisks;
    erasure::Record m_record;
    QLabel *m_summary;
    QLineEdit *m_by;
    QLineEdit *m_for;
    QPlainTextEdit *m_notes;
    QPushButton *m_check;
};

class CertificatesDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CertificatesDialog(UDisks *udisks, QWidget *parent = nullptr);

private:
    void fill();

    UDisks *m_udisks;
    QTreeWidget *m_list;
};
