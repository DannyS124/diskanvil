// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "network.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusVariant>

namespace {

const QString kService = QStringLiteral("org.freedesktop.NetworkManager");
const QString kPath = QStringLiteral("/org/freedesktop/NetworkManager");

QVariant nmProperty(const QString &path, const QString &interface, const QString &name)
{
    QDBusInterface props(kService, path, QStringLiteral("org.freedesktop.DBus.Properties"), QDBusConnection::systemBus());
    const QDBusReply<QDBusVariant> reply = props.call(QStringLiteral("Get"), interface, name);
    return reply.isValid() ? reply.value().variant() : QVariant();
}

} // namespace

Network::Network(QObject *parent)
    : QObject(parent)
{
    // Often enough for the icon to follow joining and leaving in the terminal.
    m_poll.setInterval(5000);
    connect(&m_poll, &QTimer::timeout, this, &Network::refresh);
    m_poll.start();
    refresh();
}

void Network::refresh()
{
    QDBusInterface nm(kService, kPath, kService, QDBusConnection::systemBus());
    m_available = nm.isValid();
    m_wifiPresent = m_wired = false;
    m_joined.clear();
    m_strength = 0;
    if (m_available) {
        m_wifiOn = nmProperty(kPath, kService, QStringLiteral("WirelessEnabled")).toBool();
        const QDBusReply<QList<QDBusObjectPath>> devices = nm.call(QStringLiteral("GetDevices"));
        for (const QDBusObjectPath &device : devices.value()) {
            const QString d = device.path();
            const uint type = nmProperty(d, kService + QStringLiteral(".Device"), QStringLiteral("DeviceType")).toUInt();
            const uint state = nmProperty(d, kService + QStringLiteral(".Device"), QStringLiteral("State")).toUInt();
            if (type == 1 && state == 100) // ethernet, connected
                m_wired = true;
            if (type != 2)
                continue;
            m_wifiPresent = true;
            if (state != 100)
                continue;
            const QString ap = qvariant_cast<QDBusObjectPath>(nmProperty(d, kService + QStringLiteral(".Device.Wireless"), QStringLiteral("ActiveAccessPoint"))).path();
            if (ap.isEmpty() || ap == QLatin1String("/"))
                continue;
            m_joined = QString::fromUtf8(nmProperty(ap, kService + QStringLiteral(".AccessPoint"), QStringLiteral("Ssid")).toByteArray());
            m_strength = nmProperty(ap, kService + QStringLiteral(".AccessPoint"), QStringLiteral("Strength")).toInt();
        }
    }
    Q_EMIT changed();
}
