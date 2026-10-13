// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "notifications.h"

#include <QDBusConnection>
#include <QTimer>

Notifications::Notifications(QObject *parent)
    : QObject(parent)
{
    new NotificationsAdaptor(this);
    QDBusConnection bus = QDBusConnection::sessionBus();
    m_registered = bus.registerService(QStringLiteral("org.freedesktop.Notifications"))
                && bus.registerObject(QStringLiteral("/org/freedesktop/Notifications"), this);
}

uint Notifications::notify(const QString &app, uint replaces, const QString &icon, const QString &summary, const QString &body, int timeout)
{
    const uint id = replaces ? replaces : m_next++;
    QVariantMap n{{QStringLiteral("id"), id}, {QStringLiteral("app"), app}, {QStringLiteral("summary"), summary.left(200)},
                  {QStringLiteral("body"), body.left(600)}, {QStringLiteral("icon"), icon}};
    bool replaced = false;
    for (QVariant &v : m_shown) {
        if (v.toMap().value(QStringLiteral("id")).toUInt() == id) {
            v = n;
            replaced = true;
        }
    }
    if (!replaced)
        m_shown << n;
    while (m_shown.size() > 4) // the oldest go
        m_shown.removeFirst();
    Q_EMIT shownChanged();
    // As long as asked (0 means until closed: then a while longer than usual), 8 seconds otherwise.
    const int ms = timeout > 0 ? std::min(timeout, 60000) : timeout == 0 ? 20000 : 8000;
    QTimer::singleShot(ms, this, [this, id] { dismiss(id); });
    return id;
}

void Notifications::dismiss(uint id)
{
    const auto before = m_shown.size();
    m_shown.erase(std::remove_if(m_shown.begin(), m_shown.end(), [id](const QVariant &v) { return v.toMap().value(QStringLiteral("id")).toUInt() == id; }),
                  m_shown.end());
    if (m_shown.size() != before) {
        Q_EMIT shownChanged();
        Q_EMIT closed(id, 2);
    }
}

NotificationsAdaptor::NotificationsAdaptor(Notifications *parent)
    : QDBusAbstractAdaptor(parent)
    , m_notifications(parent)
{
    connect(parent, &Notifications::closed, this, &NotificationsAdaptor::NotificationClosed);
}

QString NotificationsAdaptor::GetServerInformation(QString &vendor, QString &version, QString &spec)
{
    vendor = QStringLiteral("DiskAnvil");
    version = QStringLiteral(DISKANVIL_VERSION);
    spec = QStringLiteral("1.2");
    return QStringLiteral("diskanvil-shell");
}

uint NotificationsAdaptor::Notify(const QString &app_name, uint replaces_id, const QString &app_icon, const QString &summary, const QString &body,
                                  const QStringList &, const QVariantMap &, int expire_timeout)
{
    return m_notifications->notify(app_name, replaces_id, app_icon, summary, body, expire_timeout);
}

void NotificationsAdaptor::CloseNotification(uint id)
{
    m_notifications->dismiss(id);
}
