// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Save My Files (savefiles.h): pick the drive, tick the people's folders and bookmarks, pick
// where they go.

#include "savefiles.h"

#include <QDialog>

#include <atomic>

class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QThread;
class QTreeWidget;
class UDisks;

class SaveFilesDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SaveFilesDialog(UDisks *udisks, QWidget *parent = nullptr);
    ~SaveFilesDialog() override;

protected:
    void reject() override;

private:
    void fillDrives();
    void chooseDrive();
    void lookAt(const QString &root, const QString &disk);
    void showFound();
    void updateTarget();
    void start();

    UDisks *m_udisks;
    QStackedWidget *m_pages;
    QListWidget *m_drives;
    QLabel *m_driveNote;
    QTreeWidget *m_tree;
    QLineEdit *m_to;
    QLabel *m_toNote;
    QPushButton *m_start;
    QProgressBar *m_bar;
    QLabel *m_progress;
    QLabel *m_summary;
    QPushButton *m_stop;
    QPushButton *m_openFolder;
    QString m_root;
    QString m_disk; // the source drive's device, so nothing is saved back onto it
    savefiles::Found m_found;
    QThread *m_thread = nullptr;
    savefiles::Saver *m_saver = nullptr;
    QString m_savedTo;
    std::atomic<bool> m_stopLooking{false};
};
