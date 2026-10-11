// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jobui.h"
#include "rescuestick.h"
#include "udisks.h"

#include <QDialog>

#include <functional>
#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QThread;
class UsbPrep;

// Make a DiskForge Live USB: puts the DiskForge Live ISO onto a USB stick.
// Unlike Write Image to USB, the stick gets a normal FAT32 partition with the ISO's files on
// it, so the rescue system can keep its logs there and any PC can read them. The steps:
// a new MBR table, one FAT32 partition marked bootable, mount it, copy and check the files,
// unmount. It also opens the logs folder of a stick that already has DiskForge Live on it.
class RescueUsbDialog : public QDialog
{
    Q_OBJECT
public:
    RescueUsbDialog(UDisks *udisks, const QString &preferredDisk, QWidget *parent = nullptr);
    ~RescueUsbDialog() override;

    // For the tests: offer loop devices as sticks too.
    static bool allowLoopDevicesForTest;
    // For the tests: opens the stick (writable) for the BIOS boot code instead of UDisks, which
    // wants the admin password even for the user's own loop devices.
    static std::function<int(const QString &diskPath)> openStickForTest;
    // The newest rescue ISO in Downloads (or the home folder), or empty.
    static QString findImage();

protected:
    void reject() override;

private:
    void fillTargets(const QString &preferred);
    void inspectImage();
    void updateState();
    void openLogs();
    const Disk *target() const;
    const Volume *rescueVolume(const Disk &disk) const; // a FAT32 "DISKFORGE" (or older "BLUESPARK", "DFRESCUE") partition

    void start();
    void copyFiles(const QString &mountPoint);
    void writeBiosBoot();
    QString readyText(bool bios) const;
    void finish(bool ok, const QString &message, bool alreadyShown = false);

    UDisks *m_udisks;
    QLineEdit *m_image;
    QLabel *m_imageInfo;
    QComboBox *m_targets;
    QLabel *m_stickInfo;
    QPushButton *m_openLogs;
    QLabel *m_warning;
    QLineEdit *m_confirm;
    QCheckBox *m_bios;
    QPushButton *m_make;
    QProgressBar *m_progress;
    QLabel *m_phase;
    PhaseProgress m_meter;

    rescue::Image m_inspected;
    QString m_inspectedPath;
    QString m_diskPath;   // the stick being made
    QString m_failure;    // why the copy failed, shown once the stick is unmounted
    UsbPrep *m_prep = nullptr;
    QThread *m_thread = nullptr;
    rescue::StickWriter *m_writer = nullptr;
    QByteArray m_biosBoot, m_biosCore; // from the image, for old BIOS PCs
    bool m_running = false;
    bool m_copyDone = false;
};
