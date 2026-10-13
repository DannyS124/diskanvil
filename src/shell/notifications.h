// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDBusAbstractAdaptor>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

// Programs' notifications (Firefox's "download finished" and the like): the shell is the
// freedesktop notification service, and shows them in the corner for a few seconds.
class Notifications : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList shown READ shown NOTIFY shownChanged)
    Q_PROPERTY(bool registered READ registered CONSTANT)
public:
    explicit Notifications(QObject *parent = nullptr);
    bool registered() const { return m_registered; }
    // Each: id, app, summary, body, icon.
    QVariantList shown() const { return m_shown; }
    Q_INVOKABLE void dismiss(uint id);

    uint notify(const QString &app, uint replaces, const QString &icon, const QString &summary, const QString &body, int timeout);

Q_SIGNALS:
    void shownChanged();
    void closed(uint id, uint reason);

private:
    QVariantList m_shown;
    uint m_next = 1;
    bool m_registered = false;
};

class NotificationsAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    explicit NotificationsAdaptor(Notifications *parent);

public Q_SLOTS:
    QStringList GetCapabilities() { return {QStringLiteral("body")}; }
    QString GetServerInformation(QString &vendor, QString &version, QString &spec);
    uint Notify(const QString &app_name, uint replaces_id, const QString &app_icon, const QString &summary, const QString &body,
                const QStringList &actions, const QVariantMap &hints, int expire_timeout);
    void CloseNotification(uint id);

Q_SIGNALS:
    void NotificationClosed(uint id, uint reason);
    void ActionInvoked(uint id, const QString &action_key);

private:
    Notifications *m_notifications;
};
