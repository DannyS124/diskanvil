// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Are My Files Still OK?: a folder picked, its fingerprints recorded, and checked again later
// (fingerprints.h), with what's damaged first.

#include "fingerprints.h"

#include <QDialog>
#include <QPointer>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTreeWidget;

class FingerprintsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit FingerprintsDialog(const QString &folder = {}, QWidget *parent = nullptr);
    ~FingerprintsDialog() override;

    void record();
    void check();

signals:
    void ran(bool ok); // a record or check finished (the tests)

private:
    void folderChanged();
    void start(fingerprints::Job::Mode mode);
    void finished(bool ok, const QVector<fingerprints::Result> &results, const QString &book, const QString &error);
    void saveReport();

    QLineEdit *m_folder;
    QLabel *m_state;
    QPushButton *m_record;
    QPushButton *m_check;
    QPushButton *m_stop;
    QPushButton *m_save;
    QProgressBar *m_progress;
    QTreeWidget *m_results;
    QPointer<fingerprints::Job> m_job;
    fingerprints::Job::Mode m_mode = fingerprints::Job::Mode::Check;
    QVector<fingerprints::Result> m_last;
};
