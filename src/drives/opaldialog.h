// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Self-Encrypting Drive (opal.h): a drive asked whether it encrypts, whether its locking is on and
// whether it's locked; and, for one locked with a password nobody has, a reset with the PSID from
// its label, which erases it. Asking and resetting take root, so they run through DiskAnvil itself
// as root (DiskAnvil Live's sudo, pkexec on a PC) or, in the Windows version, right here.

#include "opal.h"

#include <QDialog>

#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QWidget;
class UDisks;

class OpalDialog : public QDialog
{
    Q_OBJECT
public:
    OpalDialog(UDisks *udisks, const QString &preferredDisk = {}, QWidget *parent = nullptr);

    // The tests: what runs `diskanvil --drive-opal ...` (arguments after the program, the input,
    // ok), instead of running it.
    static std::function<QString(const QStringList &args, const QByteArray &input, bool *ok)> runForTest;
    static bool allowLoopDevicesForTest;

signals:
    void asked(); // the drive answered, or didn't (the tests)
    void reset(bool done);

private:
    void fillTargets(const QString &preferred);
    const struct Disk *target() const;
    void ask();
    void updateState();
    void revert();
    QString run(const QStringList &args, const QByteArray &input, bool *ok);

    UDisks *m_udisks;
    QComboBox *m_targets;
    QPushButton *m_ask;
    QLabel *m_state;
    QWidget *m_resetBox;
    QLineEdit *m_psid;
    QLabel *m_warning;
    QLineEdit *m_confirm;
    QPushButton *m_reset;
    opal::Discovery m_found;
    QString m_askedFor;
};
