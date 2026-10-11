// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jobui.h"
#include "udisks.h"
#include "windowsusb.h"

#include <QDialog>
#include <QTimer>

class IsoMount;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class WindowsUsbJob;

// Make a Windows USB: a Windows 10 or 11 install stick from Microsoft's ISO, with the Windows 11
// options Rufus has (no TPM or Secure Boot check, no Microsoft account, a local account...).
// The ISO is mounted read-only as soon as it's picked, to see what's in it.
class WindowsUsbDialog : public QDialog
{
    Q_OBJECT
public:
    WindowsUsbDialog(UDisks *udisks, const QString &preferredDisk, QWidget *parent = nullptr, const QString &iso = QString());
    ~WindowsUsbDialog() override;

    // For the tests: offer loop devices as sticks too.
    static bool allowLoopDevicesForTest;
    // The newest Windows ISO in Downloads, or empty.
    static QString findIso();

protected:
    void reject() override;

private:
    void fillTargets(const QString &preferred);
    void openIso();
    void updateState();
    const Disk *target() const;
    windowsusb::Options options() const;
    void saveOptions() const;
    void start();
    void done(bool ok, const QString &message, bool shownAlready);
    // Unmounts the ISO, then runs `then`.
    void closeIso(const std::function<void()> &then);

    UDisks *m_udisks;
    QLineEdit *m_iso;
    QLabel *m_isoInfo;
    QComboBox *m_targets;
    QCheckBox *m_skipChecks;
    QCheckBox *m_noAccount;
    QCheckBox *m_localUser;
    QLineEdit *m_userName;
    QCheckBox *m_privacy;
    QCheckBox *m_region;
    QCheckBox *m_noBitLocker;
    QLabel *m_warning;
    QLineEdit *m_confirm;
    QLabel *m_phase;
    QProgressBar *m_progress;
    QPushButton *m_make;
    PhaseProgress m_meter;

    IsoMount *m_mount = nullptr;
    QString m_mounted;     // the ISO path that's mounted, once it's ready
    QString m_isoLabel;
    windowsusb::Info m_info;
    QString m_isoError;
    WindowsUsbJob *m_job = nullptr;
    windowsusb::Options m_region0; // this PC's region, for the label
    bool m_closing = false;
    QTimer m_openTimer;
};
