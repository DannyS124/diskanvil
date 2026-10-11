// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jobui.h"
#include "stickcheck.h"
#include "udisks.h"

#include <QDialog>

class BlockMapWidget;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QThread;
class UsbPrep;

// Check a USB Stick: writes to the whole stick and reads it back, to find bad spots and fake
// sticks. A fake one can then be made safe to use: one partition that ends where the stick
// really ends.
class StickCheckDialog : public QDialog
{
    Q_OBJECT
public:
    StickCheckDialog(UDisks *udisks, const QString &preferredDisk, QWidget *parent = nullptr);
    ~StickCheckDialog() override;

    // For the tests: offer loop devices too.
    static bool allowLoopDevicesForTest;
    // For the previews: show a result without running a check.
    void showResultForPreview(const stickcheck::Result &result) { showResult(result); }

protected:
    void reject() override;

private:
    void fillTargets(const QString &preferred);
    void updateState();
    const Disk *target() const;
    stickcheck::Mode mode() const;
    void start();
    void showResult(const stickcheck::Result &result);
    // One partition, as big as the stick really is.
    void makePartition();

    UDisks *m_udisks;
    QComboBox *m_targets;
    QRadioButton *m_quick;
    QRadioButton *m_full;
    QRadioButton *m_twice;
    QLabel *m_warning;
    QLineEdit *m_confirm;
    BlockMapWidget *m_map;
    QLabel *m_phase;
    QProgressBar *m_progress;
    QLabel *m_result;
    QPushButton *m_start;
    QPushButton *m_safe;
    PhaseProgress m_meter;

    QString m_diskPath;
    stickcheck::Result m_last;
    QThread *m_thread = nullptr;
    stickcheck::Checker *m_checker = nullptr;
    UsbPrep *m_prep = nullptr;
    bool m_running = false;
};
