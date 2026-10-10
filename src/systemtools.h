// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Optimize Drives, Disk Cleanup and the Btrfs snapshots view. The first two change the
// system only by starting fixed systemd units (polkit asks for the password).

#include "udisks.h"

#include <QDialog>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QThread;
class QTreeWidget;
class Systemd;

#ifdef Q_OS_WIN

// On Windows, with Windows' own tools (src/win/systemtools_win.cpp): Optimize-Volume, which
// trims SSDs and defragments hard drives, and the weekly task Windows runs it from; the
// temporary folders, the Recycle Bin, error reports and Delivery Optimization's downloads.
class OptimizeDialog : public QDialog
{
    Q_OBJECT
public:
    explicit OptimizeDialog(UDisks *udisks, QWidget *parent = nullptr);

protected:
    void reject() override;

private:
    void refresh();
    void askAboutTask();
    void optimizeNext(QStringList letters);

    UDisks *m_udisks;
    QTreeWidget *m_list;
    QLabel *m_last;
    QCheckBox *m_weekly;
    QPushButton *m_now;
    QLabel *m_status;
    bool m_running = false;
};

class CleanupDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CleanupDialog(UDisks *udisks, QWidget *parent = nullptr);
    ~CleanupDialog() override;

    struct Sizes {
        quint64 userTemp = 0;
        quint64 windowsTemp = 0;
        quint64 recycleBin = 0;
        quint64 errorReports = 0;
        quint64 delivery = 0; // Delivery Optimization's cache
    };

protected:
    void reject() override;

private:
    void measure(const QString &done = {});
    void showSizes(const Sizes &sizes);
    void clean();
    void setBusy(bool busy, const QString &text = {});

    UDisks *m_udisks;
    QCheckBox *m_userTemp;
    QCheckBox *m_windowsTemp;
    QCheckBox *m_recycle;
    QCheckBox *m_reports;
    QCheckBox *m_delivery;
    QLabel *m_userTempSize;
    QLabel *m_windowsTempSize;
    QLabel *m_recycleSize;
    QLabel *m_reportsSize;
    QLabel *m_deliverySize;
    QLabel *m_status;
    QPushButton *m_clean;
    QThread *m_thread = nullptr;
    Sizes m_sizes;
    quint64 m_before = 0;
    QStringList m_problems;
    bool m_busy = false;
};

#else

class OptimizeDialog : public QDialog
{
    Q_OBJECT
public:
    explicit OptimizeDialog(UDisks *udisks, QWidget *parent = nullptr);

private:
    void refresh();

    UDisks *m_udisks;
    Systemd *m_systemd;
    QTreeWidget *m_list;
    QLabel *m_last;
    QCheckBox *m_weekly;
    QPushButton *m_now;
    QLabel *m_status;
};

class CleanupDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CleanupDialog(UDisks *udisks, QWidget *parent = nullptr);
    ~CleanupDialog() override;

    struct Sizes {
        qint64 packages = -1;      // what paccache would free; -1 = paccache not installed
        qint64 uninstalled = -1;   // cached files of packages no longer installed
        quint64 packageCache = 0;  // all of the package cache
        quint64 journal = 0;
        quint64 userCache = 0;
        quint64 trash = 0;
    };

protected:
    void reject() override;

private:
    void measure();
    void showSizes(const Sizes &sizes);
    void clean();
    void startUnits(QStringList units, const std::function<void()> &done);
    void setBusy(bool busy, const QString &text = {});

    UDisks *m_udisks;
    Systemd *m_systemd;
    QCheckBox *m_packages;
    QCheckBox *m_uninstalled;
    QCheckBox *m_journal;
    QCheckBox *m_cache;
    QCheckBox *m_trash;
    QLabel *m_packagesSize;
    QLabel *m_uninstalledSize;
    QLabel *m_journalSize;
    QLabel *m_cacheSize;
    QLabel *m_trashSize;
    QLabel *m_note;
    QLabel *m_status;
    QPushButton *m_clean;
    QThread *m_thread = nullptr;
    Sizes m_sizes;
    quint64 m_before = 0;
    QStringList m_problems;
    bool m_busy = false;
};

#endif

Q_DECLARE_METATYPE(CleanupDialog::Sizes)

class SnapshotsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SnapshotsDialog(QWidget *parent = nullptr);

private:
    void showSnapshots();

    QTreeWidget *m_subvolumes;
    QComboBox *m_configs;
    QTreeWidget *m_snapshots;
    QLabel *m_snapperNote;
};
