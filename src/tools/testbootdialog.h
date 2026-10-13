// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Try Starting It (testboot.h): a drive or a disk image started in a virtual machine.

#include "udisks.h"

#include <QDialog>

class QLabel;
class QPushButton;
class QRadioButton;
class QSpinBox;

class TestBootDialog : public QDialog
{
    Q_OBJECT
public:
    // A drive (`disk`), or a disk image file (`image`).
    TestBootDialog(UDisks *udisks, const Disk *disk, const QString &image, QWidget *parent = nullptr);

    // Whether it starts the UEFI way, from its partition table: GPT, or an EFI partition on MBR.
    static bool looksUefi(const QByteArray &firstSectors);
    // For the tests: no window, its serial port to this file.
    static inline QString serialForTest;

private:
    void start();
    void memoryChanged();

    UDisks *m_udisks;
    Disk m_disk;
    bool m_isDisk = false;
    QString m_image;
    QRadioButton *m_uefi;
    QRadioButton *m_bios;
    QSpinBox *m_memory;
    QLabel *m_note;
    QPushButton *m_start;
    QString m_baseNote; // what's said about the drive, before what's said about memory
    qint64 m_free = -1; // MiB free on the PC when the window opened
};
