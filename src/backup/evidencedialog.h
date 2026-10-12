// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Make an Evidence Image (ewf.h): a drive or partition into an .E01, read once, checked, and a
// case report (PDF) next to it.

#include "ewf.h"
#include "udisks.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QThread;

class EvidenceDialog : public QDialog
{
    Q_OBJECT
public:
    // `volume`: a partition of `disk`, or null for the whole drive.
    EvidenceDialog(UDisks *udisks, const Disk &disk, const Volume *volume, QWidget *parent = nullptr);
    ~EvidenceDialog() override;

    // The case report, as a page of HTML.
    static QString reportHtml(const ewf::Case &details, const ewf::Result &result, const QString &source, const QString &file,
                              const QString &checked);

protected:
    void reject() override;

private:
    void updateState();
    void start();

    UDisks *m_udisks;
    Disk m_disk;
    QString m_objectPath, m_device, m_title;
    quint64 m_size = 0;
    QLineEdit *m_case, *m_evidence, *m_description, *m_examiner, *m_file;
    QPlainTextEdit *m_notes;
    QLabel *m_status;
    QProgressBar *m_bar;
    QPushButton *m_start;
    QThread *m_thread = nullptr;
    ewf::Acquire *m_job = nullptr;
    bool m_problemShown = false; // the status line says what's wrong with the form
};
