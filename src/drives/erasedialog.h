// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "udisks.h"

#include <QDialog>
#include <QElapsedTimer>

class QFrame;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QTimer;

// Secure Erase: asks the drive's firmware to erase itself. Shows the frozen state and how to
// get past it (unplug and replug the drive's power, or sleep and wake), and needs the drive's
// name typed to start.
class SecureEraseDialog : public QDialog
{
    Q_OBJECT
public:
    SecureEraseDialog(UDisks *udisks, const QString &blockPath, int diskNumber, QWidget *parent = nullptr);
    UDisks::EraseMethod method() const;
    int sectorSize() const; // NVMe: the sector size to format with; 0 keeps the one it has
    // The drive to erase. It can change: unplugged and plugged back in, a drive can get a new name.
    QString blockPath() const { return m_blockPath; }
    // For the preview: shows a step of Unplug and Replug (1 unplug, 2 plug back in, 3 not back)
    // without doing anything to the drive.
    void showReplugStepForPreview(int step);

private:
    enum class Replug { Off, Preparing, Unplug, Replug, NotBack };
    void updateState();
    void setReplugStep(Replug step);
    void lookForDrive();
    QString replugText() const;

    UDisks *m_udisks;
    QString m_blockPath;
    QLabel *m_intro;
    QRadioButton *m_first;
    QRadioButton *m_second;
    QRadioButton *m_sanitize = nullptr;       // NVMe: sanitize, erasing every block
    QRadioButton *m_sanitizeCrypto = nullptr; // and its crypto erase
    QComboBox *m_sectors = nullptr;
    QLabel *m_frozen;
    QLabel *m_howTo;
    QPushButton *m_sleep;
    QPushButton *m_checkAgain;
    QLabel *m_warning;
    QLineEdit *m_confirm;
    QPushButton *m_erase;
    QPushButton *m_replug;
    QFrame *m_replugBox;
    QLabel *m_replugPicture;
    QLabel *m_replugText;
    QLabel *m_replugCommand;
    QPushButton *m_replugCopy;
    QPushButton *m_replugNext;
    QPushButton *m_replugCancel;
    QTimer *m_replugTimer;
    QElapsedTimer m_replugWaited;
    Replug m_step = Replug::Off;
    int m_ticks = 0;
    // Who the drive is, to find it again after it's plugged back in.
    QString m_serial, m_model;
    quint64 m_size = 0;
    QString m_foundAs; // it came back under this name, not frozen
    bool m_ata = true;
};
