// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QTimer>

// How DiskAnvil Live is online, from NetworkManager over D-Bus, for the icon on the bar: by
// cable, by Wi-Fi (which network, how strong), or not at all. Networks are joined in the
// terminal (diskanvil-live-wifi), which the icon opens.
class Network : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool wifiPresent READ wifiPresent NOTIFY changed)
    Q_PROPERTY(bool wifiOn READ wifiOn NOTIFY changed)
    Q_PROPERTY(bool wired READ wired NOTIFY changed)
    Q_PROPERTY(QString joined READ joined NOTIFY changed)
    Q_PROPERTY(int strength READ strength NOTIFY changed)
public:
    explicit Network(QObject *parent = nullptr);

    bool available() const { return m_available; }
    bool wifiPresent() const { return m_wifiPresent; }
    bool wifiOn() const { return m_wifiOn; }
    bool wired() const { return m_wired; }
    QString joined() const { return m_joined; }
    int strength() const { return m_strength; }

    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void changed();

private:
    QTimer m_poll;
    bool m_available = false;
    bool m_wifiPresent = false;
    bool m_wifiOn = false;
    bool m_wired = false;
    QString m_joined;
    int m_strength = 0;
};
