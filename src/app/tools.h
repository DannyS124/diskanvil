// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "firmware.h"
#include "isomode.h"
#include "udisks.h"

#include <QDialog>
#include <QMap>
#include <QSet>

class IsoCopy;
class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class Systemd;
class QTimer;
class QGroupBox;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSpinBox;
class BlockMapWidget;
class QThread;
class QTreeWidget;

// "Encrypt with a passphrase" checkbox plus passphrase and confirm fields.
class EncryptionFields : public QWidget
{
    Q_OBJECT
public:
    explicit EncryptionFields(QWidget *parent = nullptr);
    QString passphrase() const; // empty when not encrypting
    bool isValid() const;

signals:
    void changed();

private:
    QCheckBox *m_box;
    QWidget *m_fields;
    QLineEdit *m_pass;
    QLineEdit *m_confirm;
    QLabel *m_note;
};

class ChangePassphraseDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ChangePassphraseDialog(const QString &device, QWidget *parent = nullptr);
    QString oldPassphrase() const;
    QString newPassphrase() const;

private:
    QLineEdit *m_old;
    QLineEdit *m_new;
    QLineEdit *m_confirm;
};

// Overwrites a whole disk with zeros. Type-to-confirm, like a new partition table.
class WipeDialog : public QDialog
{
    Q_OBJECT
public:
    WipeDialog(const Disk &disk, int diskNumber, QWidget *parent = nullptr);
};

class HealthDialog : public QDialog
{
    Q_OBJECT
public:
    HealthDialog(UDisks *udisks, const QString &blockPath, QWidget *parent = nullptr);

private:
    void reload();

    UDisks *m_udisks;
    QString m_blockPath;
    QLabel *m_state;
    QLabel *m_explain;
    QFormLayout *m_form;
    QTreeWidget *m_attributes;
    QLabel *m_meaning; // what the selected attribute is
    QPushButton *m_selftest;
    QPushButton *m_longTest = nullptr;
    QPushButton *m_conveyance = nullptr;
    QPushButton *m_smartOn = nullptr;
    QPushButton *m_noise = nullptr; // what a hard drive's sounds mean
    // Seagate's own record of its use (farm.h), through smartctl.
    void checkFarm();
    QPushButton *m_farm = nullptr;
    QString m_farmText;
    bool m_farmBusy = false;
    QPushButton *m_stopTest; // while one runs
    void checkFirmware();
    firmware::Result m_firmware;
    bool m_firmwareBusy = false;

    // Btrfs: error counts and scrubs, for each Btrfs partition on the drive.
    void reloadBtrfs();
    void startScrub(const QString &device, const QString &mountPoint);
    void stopScrub(const QString &unit);
    void setMonthlyScrub(const QString &timer, bool on);
    QGroupBox *m_btrfs;
    QTimer *m_scrubPoll;
    Systemd *m_systemd;
    QMap<QString, qint64> m_errorsBefore; // scrub unit -> error count when it was started here
    QSet<QString> m_stopped;              // scrub units stopped from here
};

class BenchmarkDialog : public QDialog
{
    Q_OBJECT
public:
    BenchmarkDialog(UDisks *udisks, const Disk &disk, QWidget *parent = nullptr);
    ~BenchmarkDialog() override;

protected:
    void reject() override;

private:
    void start();
    void stop();

    UDisks *m_udisks;
    Disk m_disk;
    QString m_writeDir;
    QCheckBox *m_writeTest;
    QPushButton *m_start;
    QProgressBar *m_progress;
    QLabel *m_phase;
    QLabel *m_results;
    QThread *m_thread = nullptr;
    QObject *m_worker = nullptr;
    QMetaObject::Connection m_openConn;
};

// Finds unreadable sectors (read-only), then rewrites just those so the drive reuses
// them or swaps in spares.
class BadSectorsDialog : public QDialog
{
    Q_OBJECT
public:
    BadSectorsDialog(UDisks *udisks, const Disk &disk, QWidget *parent = nullptr);
    ~BadSectorsDialog() override;

protected:
    void reject() override;

private:
    void startScan();
    void startRepair();
    void stop();
    void setRunning(bool running);
    void showFound();

    UDisks *m_udisks;
    Disk m_disk;
    QLabel *m_status;
    QProgressBar *m_progress;
    BlockMapWidget *m_map;
    QTreeWidget *m_found;
    QLabel *m_repairNote;
    QPushButton *m_scan;
    QPushButton *m_repair;
    QVector<quint64> m_bad;
    int m_logical = 512;
    QThread *m_thread = nullptr;
    QObject *m_worker = nullptr;
    QMetaObject::Connection m_openConn;
    bool m_running = false;
};

class WriteImageDialog : public QDialog
{
    Q_OBJECT
public:
    WriteImageDialog(UDisks *udisks, const QString &preferredDisk, QWidget *parent = nullptr);
    ~WriteImageDialog() override;

    // For the tests: offer loop devices as targets too.
    static bool allowLoopDevicesForTest;

protected:
    void reject() override;

private:
    void fillTargets(const QString &preferred);
    void updateState();
    void start();
    void startCopy(); // the "copy the files" way, for ISOs
    const Disk *target() const;

    UDisks *m_udisks;
    QLineEdit *m_image;
    QLabel *m_imageInfo;
    QComboBox *m_targets;
    QLineEdit *m_sha;
    QCheckBox *m_verify;
    QLabel *m_warning;
    QLineEdit *m_confirm;
    QPushButton *m_write;
    QProgressBar *m_progress;
    QLabel *m_phase;
    QThread *m_thread = nullptr;
    QObject *m_worker = nullptr;
    QMetaObject::Connection m_openConn;
    bool m_running = false;
    QString m_described; // the image the info line is about
    quint64 m_unpacked = 0; // its size once unpacked, 0 if it doesn't say
    // ISOs can be written as they are or have their files copied.
    QFormLayout *m_form = nullptr;
    QWidget *m_modeBox;
    QRadioButton *m_asIs;
    QRadioButton *m_copyMode;
    QCheckBox *m_persist;
    QSpinBox *m_persistGb;
    QLabel *m_modeNote;
    bool m_isIso = false;
    isomode::Analysis m_analysis;
    IsoCopy *m_isoCopy = nullptr;
    QPushButton *m_toWindows = nullptr; // a Windows ISO: over to Make a Windows USB
};
