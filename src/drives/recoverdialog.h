// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Recover Partitions: put a partition table back from the drive's own backup copy or from a
// layout DiskForge remembered. Only the table is written; what's in the partitions isn't
// touched. The table that's there now is remembered first, so this can be undone too.

#include "partrecover.h"
#include "partscan.h"
#include "udisks.h"

#include <QDialog>
#include <QElapsedTimer>

#include <atomic>
#include <memory>

class DiskMap;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QThread;

class RecoverDialog : public QDialog
{
    Q_OBJECT
public:
    RecoverDialog(UDisks *udisks, const Disk &disk, QWidget *parent = nullptr);
    // With the table already read (previews and tests): nothing is opened to look.
    RecoverDialog(UDisks *udisks, const Disk &disk, const gpt::Report &report, QWidget *parent = nullptr);
    ~RecoverDialog() override;
    // Why a drive can't have its table put back right now, or empty.
    static QString refusal(const Disk &disk);
    int sourceCount() const;

signals:
    void ready(); // the drive was read and the choices are listed
    void done(bool ok, const QString &message);

private:
    void build();
    const Disk *disk() const; // the drive as UDisks has it now, or as it was given
    struct Source {
        QString title;
        bool backup = false; // copy the backup over the main table, exactly
        bool scan = false;   // what the scan found; the ticked ones
        recover::Layout layout;
    };
    void startScan();
    void scanProgress(quint64 done, quint64 total);
    void scanFinished(const QVector<partscan::Found> &found);
    void ticked();
    void listSources(const gpt::Report &report);
    void choose(int row);
    void refresh();
    void write();
    void waitForPartitions(int expected);

    UDisks *m_udisks;
    Disk m_disk;
    QString m_blockPath;
    QString m_device;
    QString m_driveKey;
    QVector<Source> m_sources;
    QLabel *m_intro;
    QListWidget *m_list;
    DiskMap *m_before;
    DiskMap *m_after;
    QLabel *m_problem;
    QLineEdit *m_confirm;
    QPushButton *m_write = nullptr;
    bool m_busy = false;
    // Scanning for file systems
    QPushButton *m_scan = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_scanStatus = nullptr;
    QPushButton *m_stopScan = nullptr;
    QListWidget *m_found = nullptr;
    QVector<partscan::Found> m_foundList;
    QThread *m_scanThread = nullptr;
    std::shared_ptr<std::atomic<bool>> m_stopFlag;
    int m_scanFd = -1;
    quint64 m_scanSize = 0;
    QElapsedTimer m_scanTimer;
};
