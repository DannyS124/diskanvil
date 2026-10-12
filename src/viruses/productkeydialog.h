// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Windows Product Key: the key in the PC's firmware, and the ones Windows installs here were
// set up with (productkey.h), to copy before reinstalling.

#include <QDialog>

class QGridLayout;
class QLabel;
class UDisks;

class ProductKeyDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ProductKeyDialog(UDisks *udisks, QWidget *parent = nullptr);

private:
    void showFirmware(const QString &key, const QString &note);
    void readFirmware(bool askForRights);
    void addWindows(const QString &title, const QString &edition, const QString &key, const QString &problem);
    void lookElsewhere();

    UDisks *m_udisks;
    QLabel *m_firmware;
    QWidget *m_firmwareCopy = nullptr;
    QWidget *m_firmwareShow = nullptr;
    QGridLayout *m_installs;
    QLabel *m_none = nullptr; // while no Windows is listed
    QStringList m_shown; // Windows roots listed already
};
