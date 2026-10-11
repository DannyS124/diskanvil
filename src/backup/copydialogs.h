// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Clone Drive, Back Up, Restore Backup and Rescue Copy. All of them keep only object
// paths and look the drive up again whenever they act on it.

#include "clone.h"
#include "imagebackup.h"
#include "jobui.h"
#include "rescuecopy.h"
#include "udisks.h"

#include <QDialog>

#include <functional>

class BackupJob;
class BlockMapWidget;
class CloneJob;
class RestoreJob;
class QCheckBox;
class QComboBox;
class QTimer;
class QSpinBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;

class CloneDialog : public QDialog
{
    Q_OBJECT
public:
    CloneDialog(UDisks *udisks, const QString &sourceBlockPath, QWidget *parent = nullptr);
    ~CloneDialog() override;

protected:
    void reject() override;

private:
    void fillTargets();
    void updateState();
    void start();
    void setRunning(bool running);
    void stopThread();
    void afterCopy();
    void waitForPartitions();
    void nextStep();
    void finish(bool ok, const QString &message);

    UDisks *m_udisks;
    QString m_source;
    QComboBox *m_targets;
    QLabel *m_plan;
    QRadioButton *m_replace;
    QRadioButton *m_keepBoth;
    QCheckBox *m_grow;
    QCheckBox *m_verify;
    QLabel *m_warning;
    QLineEdit *m_confirm;
    QPushButton *m_start;
    QProgressBar *m_progress;
    QLabel *m_phase;
    PhaseProgress m_meter;
    QThread *m_thread = nullptr;
    CloneJob *m_job = nullptr;
    bool m_running = false;

    // After the copy: re-read the table, give new IDs, grow the last partition.
    QString m_target;
    QVector<Volume> m_sourceVolumes;
    bool m_newIds = false;
    bool m_growLast = false;
    QString m_copied;
    QStringList m_notes;
    QList<std::function<bool()>> m_steps; // true: started a UDisks operation, wait for it
    QMetaObject::Connection m_opConn;
};

class BackupDialog : public QDialog
{
    Q_OBJECT
public:
    // objectPath: a whole drive's block path or a partition's object path
    BackupDialog(UDisks *udisks, const QString &objectPath, QWidget *parent = nullptr);
    ~BackupDialog() override;

protected:
    void reject() override;

private:
    void updateState();
    void start();
    void setRunning(bool running);

    UDisks *m_udisks;
    QString m_object;
    QLineEdit *m_file;
    QLabel *m_info;
    QLabel *m_problem;
    QPushButton *m_start;
    QProgressBar *m_progress;
    QLabel *m_phase;
    PhaseProgress m_meter;
    QThread *m_thread = nullptr;
    BackupJob *m_job = nullptr;
    bool m_running = false;
};

class RestoreDialog : public QDialog
{
    Q_OBJECT
public:
    RestoreDialog(UDisks *udisks, const QString &objectPath, QWidget *parent = nullptr);
    ~RestoreDialog() override;

protected:
    void reject() override;

private:
    void updateState();
    void start();
    void setRunning(bool running);

    UDisks *m_udisks;
    QString m_object;
    QLineEdit *m_file;
    QLabel *m_info;
    QCheckBox *m_checkFirst;
    QCheckBox *m_verify;
    QLabel *m_warning;
    QLineEdit *m_confirm;
    QPushButton *m_start;
    QProgressBar *m_progress;
    QLabel *m_phase;
    PhaseProgress m_meter;
    QThread *m_thread = nullptr;
    RestoreJob *m_job = nullptr;
    bool m_running = false;
    BackupInfo m_backup;
};

class RescueDialog : public QDialog
{
    Q_OBJECT
public:
    RescueDialog(UDisks *udisks, const QString &sourceBlockPath, QWidget *parent = nullptr);
    ~RescueDialog() override;

protected:
    void reject() override;

private:
    void fillTargets();
    QString mapPath() const;
    void showSavedMap();
    void updateState();
    void start();
    void setRunning(bool running);
    void showBlocks(const QVector<RescueMap::Block> &blocks);
    void applyCoolDown(); // to the running copy
    void checkHeat();

    UDisks *m_udisks;
    QString m_source;
    // Going easy on the drive
    QCheckBox *m_heat;
    QSpinBox *m_hot;
    QSpinBox *m_cool;
    QCheckBox *m_rest;
    QSpinBox *m_restSeconds;
    QSpinBox *m_restErrors;
    QCheckBox *m_limit;
    QSpinBox *m_rate;
    QLabel *m_heatStatus;
    QTimer *m_heatTimer;
    bool m_paused = false;
    QRadioButton *m_toFile;
    QRadioButton *m_toDrive;
    QLineEdit *m_file;
    QPushButton *m_browse;
    QComboBox *m_targets;
    QLabel *m_info;
    QLabel *m_resume;
    BlockMapWidget *m_map;
    QLabel *m_stats;
    QProgressBar *m_progress;
    QLabel *m_warning;
    QLineEdit *m_confirm;
    QPushButton *m_start;
    QThread *m_thread = nullptr;
    RescueCopy *m_job = nullptr;
    bool m_running = false;
};
