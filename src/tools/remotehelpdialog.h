// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Let Someone Help (remotehelp.h): on DiskAnvil Live, the screen (VNC) and a terminal (SSH) opened
// to someone on the same network, with passwords made up now; Stop, or closing the window, shuts
// them out again. On a PC it says what does this there.

#include <QDialog>

class QCheckBox;
class QLabel;
class QProcess;
class QPushButton;
class QTimer;

class RemoteHelpDialog : public QDialog
{
    Q_OBJECT
public:
    explicit RemoteHelpDialog(QWidget *parent = nullptr);
    ~RemoteHelpDialog() override;

    bool helping() const { return m_vnc != nullptr || m_ssh; }

protected:
    void done(int result) override;

private:
    void start();
    void stop();
    void readVnc();
    void showDetails();

    QCheckBox *m_screen = nullptr;
    QCheckBox *m_terminal = nullptr;
    QLabel *m_details = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_stop = nullptr;
    QTimer *m_addresses = nullptr;
    QProcess *m_vnc = nullptr;
    QString m_vncPassword, m_sshPassword;
    QStringList m_shown; // the addresses shown
    QStringList m_connected;
    int m_port = 0;
    bool m_ssh = false;
};
