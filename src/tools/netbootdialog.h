// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Start Other PCs from This One (netboot.h): the window. Pick the network card, Start, and it lists
// the PCs that asked and how far each has got. Stop, or closing it, ends it.

#include <QDialog>
#include <QMap>

class QComboBox;
class QLabel;
class QProcess;
class QPushButton;
class QTimer;
namespace netboot {
class FileServer;
}

class NetBootDialog : public QDialog
{
    Q_OBJECT
public:
    explicit NetBootDialog(QWidget *parent = nullptr);
    ~NetBootDialog() override;

    bool serving() const { return m_dnsmasq != nullptr; }

protected:
    void done(int result) override;

private:
    void start();
    void stop();
    void readDnsmasq();
    void showPcs();

    QComboBox *m_cards = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_pcs = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_stop = nullptr;
    QProcess *m_dnsmasq = nullptr;
    netboot::FileServer *m_server = nullptr;
    QTimer *m_refresh = nullptr;
    QStringList m_order;              // the PCs, as they turned up
    QMap<QString, QString> m_state;   // a PC (its address, or its network card's) and how far it's got
    QString m_said;                   // dnsmasq's last words, for when it stops
};
