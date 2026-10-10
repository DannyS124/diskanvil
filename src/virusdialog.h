// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jobui.h"
#include "virusscan.h"

#include <QDialog>

class QButtonGroup;
class QLabel;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QThread;
class QTreeWidget;
class UDisks;

// Scan for Viruses: pick a drive (Windows' first) or a folder, scan it against the antivirus's
// list, and delete what's found if you want to. On DiskForge Live that's a Windows that isn't
// running, so nothing it does can hide; on a PC, its other drives. Drives are mounted read-only
// for the scan, and only made writable again to delete what was ticked.
class VirusDialog : public QDialog
{
    Q_OBJECT
public:
    explicit VirusDialog(UDisks *udisks, QWidget *parent = nullptr);
    ~VirusDialog() override;

    // Scans a folder straight away (and the tests' way in).
    void scanFolder(const QString &folder);
    // Deletes the ticked files after asking (the tests answer the question).
    void deleteTicked();
    // Closing stops the scan and unmounts what this window mounted.
    void reject() override;

private:
    void refreshEngine();
    void fillTargets();
    void updateList();
    void start();
    void chooseFolder();
    void mountThenScan(const QString &objectPath, const QString &title);
    void startScan(const QStringList &folders, const QString &title);
    void addHit(const virus::Detection &d);
    void scanFinished(bool completed, int scanned, int unreadable, const QString &problem);
    QString shownPath(const QString &path) const;
    void updateButtons();
    void stopWorkers();
    void log(const QStringList &lines);
    void loadLiveList();

    UDisks *m_udisks;
    virus::Engine m_engine;
    QStackedWidget *m_pages;
    // Picking
    QLabel *m_engineInfo;
    QPushButton *m_update;
    QTreeWidget *m_targets;
    QButtonGroup *m_depth;
    QPushButton *m_scan;
    QLabel *m_pickNote;
    // Scanning and what it found
    QLabel *m_status;
    QProgressBar *m_bar;
    QLabel *m_phase;
    QPushButton *m_stop;
    QTreeWidget *m_hits;
    QLabel *m_hitInfo;
    QPushButton *m_delete;
    QPushButton *m_again;

    QStringList m_folders;   // what's being scanned
    QString m_title;
    QString m_volumePath;    // the volume being scanned, when it's a drive
    bool m_mountedReadOnly = false; // by this window, for the scan
    QStringList m_mountedHere; // volumes this window mounted, unmounted again when it closes
    virus::Scan *m_job = nullptr;
    virus::Update *m_updateJob = nullptr;
    QThread *m_thread = nullptr;
    bool m_busy = false;
    int m_found = 0;
};
