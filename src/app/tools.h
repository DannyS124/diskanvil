// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "blockmapdata.h"
#include "drivepower.h"
#include "firmware.h"
#include "isomode.h"
#include "udisks.h"

#include <QDialog>
#include <QMap>
#include <QSet>
#include <QVariantMap>

#include <functional>

class IsoCopy;
class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class Systemd;
class QTimer;
class QGroupBox;
class QLineEdit;
class QListWidget;
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

// Unlocks a VeraCrypt (or TrueCrypt) volume through UDisks: its password, and what VeraCrypt
// asks besides when it was made that way: a PIM, keyfiles, a hidden volume, Windows' system
// encryption.
class VeraCryptDialog : public QDialog
{
    Q_OBJECT
public:
    explicit VeraCryptDialog(const QString &volumeName, QWidget *parent = nullptr);
    QString password() const;
    QVariantMap options() const; // UDisks' unlock options

private:
    QLineEdit *m_password;
    QSpinBox *m_pim;
    QCheckBox *m_hidden;
    QCheckBox *m_system;
    QListWidget *m_keyfiles;
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

// Formats a memory card the SD Card Association's way (sdcard.h). Everything on it goes:
// type-to-confirm, like a wipe.
class SdCardDialog : public QDialog
{
    Q_OBJECT
public:
    SdCardDialog(UDisks *udisks, const Disk &disk, int diskNumber, QWidget *parent = nullptr);
    ~SdCardDialog() override;

protected:
    void reject() override; // not while it writes

private:
    void start();

    UDisks *m_udisks;
    Disk m_disk;
    QLineEdit *m_label;
    QLineEdit *m_confirm;
    QPushButton *m_start;
    QProgressBar *m_progress;
    QLabel *m_status;
    QThread *m_thread = nullptr;
    QMetaObject::Connection m_openConn;
    bool m_running = false;
};

// Switches an ATA drive that can to another logical sector size (hdparm --set-sector-size), on
// DiskAnvil Live. Everything on it goes: type-to-confirm.
class SectorSizeDialog : public QDialog
{
    Q_OBJECT
public:
    SectorSizeDialog(UDisks *udisks, const Disk &disk, const drivepower::SectorSizes &sizes, QWidget *parent = nullptr);
    // For the tests: runs instead of the command (nothing reaches a drive). True for done.
    static inline std::function<bool(const QStringList &command, QString *said)> runForTest;

private:
    void change();

    UDisks *m_udisks;
    Disk m_disk;
    QComboBox *m_size;
    QLineEdit *m_confirm;
    QPushButton *m_change;
};

class HealthDialog : public QDialog
{
    Q_OBJECT
public:
    HealthDialog(UDisks *udisks, const QString &blockPath, QWidget *parent = nullptr);
    // What the window shows, as a page of HTML: the drive report.
    QString reportHtml() const;

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
    QPushButton *m_report = nullptr; // the drive report, as a PDF
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
    QPushButton *m_pdf = nullptr; // the results, as a PDF
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
    // For the tests: a scan's reads, as if it had just finished; and loop devices count as SSDs.
    void showScanForTest(const QVector<ReadSample> &samples);
    static inline bool loopIsSsdForTest = false;

protected:
    void reject() override;

private:
    void startScan();
    void scanFinished(bool completed, const QVector<quint64> &bad, int logical);
    void startRepair();
    void startRefresh();
    bool isSsd() const;
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
    QPushButton *m_refresh; // SSDs: write slow pieces back (experimental)
    QVector<ReadSample> m_slowReads; // reads that took long enough to maybe be slow for this drive
    QVector<QPair<quint64, quint64>> m_slow; // after the scan: the pieces that were
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
