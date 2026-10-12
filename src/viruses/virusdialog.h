// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "bootcheck.h"
#include "jobui.h"
#include "virusscan.h"
#include "winstartup.h"

#include <QDialog>
#include <QHash>
#include <QSet>

#include <functional>

class QButtonGroup;
class QCheckBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QThread;
class QTreeWidget;
class UDisks;

// Scan for Viruses: pick a drive (Windows' first) or a folder, scan it against the antivirus's
// list, and delete what's found if you want to. On DiskAnvil Live that's a Windows that isn't
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
    // Switches the ticked startup entries off after asking, with a backup first.
    void switchOffTicked();
    // Checks the EFI partition mounted at `esp`, with `windows` where that Windows is (the
    // tests' way in; a scan finds the partition itself).
    void checkEfiPartition(const QString &esp, const QString &windows);
    // Puts Windows' own copies back over the ticked boot files, after asking.
    void putBackTicked();
    // Asks CIRCL's hashlookup whether it knows the programs that look odd, after asking.
    void lookUpOnline();
    // The tests leave fwupd out: its answer is about the PC they run on.
    static inline bool askFwupd = true;
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
    void runScan(const QStringList &alsoScan);
    void showStartup();
    void showStartupDetails();
    void checkBoot(bool running);
    void showBoot();
    void showBootDetails();
    // Unmounts the volume and mounts it again, writable; `then` gets where it is now.
    void remountWritable(const QString &objectPath, const std::function<void(const QString &mount)> &then);
    void addHit(const virus::Detection &d);
    void scanFinished(bool completed, int scanned, int unreadable, const QString &problem);
    QString shownPath(const QString &path) const;
    void updateButtons();
    void stopWorkers();
    void log(const QStringList &lines);
    // Runs `then` once the drive being scanned can be written to: at once, unless this window
    // mounted it read-only, which it then mounts again. `from` and `to` are where the drive was
    // and is now (the same unless it moved).
    void whenWritable(const std::function<void(const QString &from, const QString &to)> &then);
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
    // What starts with Windows
    QTabWidget *m_tabs;
    QTreeWidget *m_startupList;
    QLabel *m_startupInfo;
    QLabel *m_startupDetails;
    QCheckBox *m_showAll;
    startup::Report m_startup;
    QString m_windowsRoot; // where that Windows is; empty for the one that's running
    QSet<int> m_ticked;    // startup entries ticked to switch off
    QHash<int, QPair<bool, QString>> m_switched; // what happened when they were: done, and how
    QPushButton *m_switchOff;
    // What starts the PC before Windows
    QTreeWidget *m_bootList;
    QLabel *m_bootInfo;
    QLabel *m_bootDetails;
    QPushButton *m_putBack;
    QPushButton *m_lookUp;
    QHash<QString, QString> m_online; // a file here, and what the online check said about it
    boot::Report m_boot;
    QSet<int> m_bootTicked;
    QHash<int, QPair<bool, QString>> m_putBackDone;
    QString m_espPath;  // the EFI partition looked at, and where it's mounted
    QString m_espMount;
    bool m_espReadOnly = false;
    QThread *m_startupThread = nullptr;
    bool m_stopped = false;

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
