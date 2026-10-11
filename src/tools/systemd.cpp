// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "systemd.h"

#include "dbusnames.h"

#include <QDBusArgument>
#include <QDir>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>

namespace {

const QString kSystemd = QStringLiteral("org.freedesktop.systemd1");
const QString kSystemdPath = QStringLiteral("/org/freedesktop/systemd1");
const QString kSystemdManager = QStringLiteral("org.freedesktop.systemd1.Manager");

} // namespace

Systemd::Systemd(QObject *parent)
    : QObject(parent)
{
    QDBusConnection bus = QDBusConnection::systemBus();
    bus.connect(kSystemd, kSystemdPath, kSystemdManager, QStringLiteral("JobRemoved"), this,
                SLOT(onJobRemoved(uint, QDBusObjectPath, QString, QString)));
    // JobRemoved is only sent to clients that subscribed.
    bus.call(QDBusMessage::createMethodCall(kSystemd, kSystemdPath, kSystemdManager, QStringLiteral("Subscribe")),
             QDBus::Block, 3000);
}

void Systemd::callThen(const QString &method, const QVariantList &args, const Done &failed,
                       const std::function<void(const QDBusMessage &)> &next)
{
    QDBusMessage message = QDBusMessage::createMethodCall(kSystemd, kSystemdPath, kSystemdManager, method);
    message.setArguments(args);
    message.setInteractiveAuthorizationAllowed(m_interactive);
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, kNoTimeout), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [failed, next](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (w->isError())
            failed(false, w->error().message());
        else
            next(w->reply());
    });
}

void Systemd::startUnit(const QString &unit, const Done &done)
{
    callThen(QStringLiteral("StartUnit"), {unit, QStringLiteral("replace")}, done, [this, done](const QDBusMessage &reply) {
        const QString job = reply.arguments().value(0).value<QDBusObjectPath>().path();
        if (m_finished.contains(job)) {
            const QString result = m_finished.take(job);
            done(result == QLatin1String("done"), result);
        } else {
            m_waiting.insert(job, done);
        }
    });
}

void Systemd::stopUnit(const QString &unit, const Done &done)
{
    callThen(QStringLiteral("StopUnit"), {unit, QStringLiteral("replace")}, done, [this, done](const QDBusMessage &reply) {
        const QString job = reply.arguments().value(0).value<QDBusObjectPath>().path();
        if (m_finished.contains(job)) {
            const QString result = m_finished.take(job);
            done(result == QLatin1String("done"), result);
        } else {
            m_waiting.insert(job, done);
        }
    });
}

void Systemd::onJobRemoved(uint, const QDBusObjectPath &job, const QString &, const QString &result)
{
    const auto it = m_waiting.find(job.path());
    if (it == m_waiting.end()) {
        m_finished.insert(job.path(), result);
        if (m_finished.size() > 64)
            m_finished.clear(); // jobs other programs started; nobody will ask for these
        return;
    }
    const Done done = it.value();
    m_waiting.erase(it);
    done(result == QLatin1String("done"), result);
}

void Systemd::setTimerEnabled(const QString &timer, bool enabled, const Done &done)
{
    const QStringList files = {timer};
    if (enabled) {
        callThen(QStringLiteral("EnableUnitFiles"), {files, false, false}, done, [this, timer, done](const QDBusMessage &) {
            callThen(QStringLiteral("Reload"), {}, done, [this, timer, done](const QDBusMessage &) { startUnit(timer, done); });
        });
    } else {
        callThen(QStringLiteral("StopUnit"), {timer, QStringLiteral("replace")}, done, [this, files, done](const QDBusMessage &) {
            callThen(QStringLiteral("DisableUnitFiles"), {files, false}, done, [this, done](const QDBusMessage &) {
                callThen(QStringLiteral("Reload"), {}, done, [done](const QDBusMessage &) { done(true, {}); });
            });
        });
    }
}

bool Systemd::unitExists(const QString &unit) const
{
    return !unitFileState(unit).isEmpty();
}

QString Systemd::unitFileState(const QString &unit) const
{
    QDBusMessage call = QDBusMessage::createMethodCall(kSystemd, kSystemdPath, kSystemdManager, QStringLiteral("GetUnitFileState"));
    call << unit;
    const QDBusMessage reply = QDBusConnection::systemBus().call(call, QDBus::Block, 5000);
    return reply.type() == QDBusMessage::ReplyMessage ? reply.arguments().value(0).toString() : QString();
}

QVariant Systemd::unitProperty(const QString &unit, const QString &interface, const QString &name) const
{
    QDBusMessage load = QDBusMessage::createMethodCall(kSystemd, kSystemdPath, kSystemdManager, QStringLiteral("LoadUnit"));
    load << unit;
    const QDBusMessage loaded = QDBusConnection::systemBus().call(load, QDBus::Block, 5000);
    if (loaded.type() != QDBusMessage::ReplyMessage)
        return {};
    const QString path = loaded.arguments().value(0).value<QDBusObjectPath>().path();
    QDBusMessage get = QDBusMessage::createMethodCall(kSystemd, path, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    get << interface << name;
    const QDBusMessage reply = QDBusConnection::systemBus().call(get, QDBus::Block, 5000);
    return reply.type() == QDBusMessage::ReplyMessage ? reply.arguments().value(0).value<QDBusVariant>().variant() : QVariant();
}

quint64 Systemd::timerLastTrigger(const QString &timer) const
{
    return unitProperty(timer, QStringLiteral("org.freedesktop.systemd1.Timer"), QStringLiteral("LastTriggerUSec")).toULongLong();
}

quint64 Systemd::serviceLastRun(const QString &service) const
{
    return unitProperty(service, QStringLiteral("org.freedesktop.systemd1.Service"), QStringLiteral("ExecMainExitTimestamp")).toULongLong();
}

Systemd::ServiceState Systemd::serviceState(const QString &service) const
{
    ServiceState s;
    QDBusMessage load = QDBusMessage::createMethodCall(kSystemd, kSystemdPath, kSystemdManager, QStringLiteral("LoadUnit"));
    load << service;
    const QDBusMessage loaded = QDBusConnection::systemBus().call(load, QDBus::Block, 5000);
    if (loaded.type() != QDBusMessage::ReplyMessage)
        return s;
    const QString path = loaded.arguments().value(0).value<QDBusObjectPath>().path();
    auto all = [&path](const QString &interface) {
        QDBusMessage get = QDBusMessage::createMethodCall(kSystemd, path, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("GetAll"));
        get << interface;
        const QDBusMessage reply = QDBusConnection::systemBus().call(get, QDBus::Block, 5000);
        QVariantMap out;
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty())
            reply.arguments().constFirst().value<QDBusArgument>() >> out;
        return out;
    };
    const QVariantMap unit = all(QStringLiteral("org.freedesktop.systemd1.Unit"));
    const QVariantMap svc = all(QStringLiteral("org.freedesktop.systemd1.Service"));
    s.exists = unit.value(QStringLiteral("LoadState")).toString() == QLatin1String("loaded");
    s.active = unit.value(QStringLiteral("ActiveState")).toString();
    s.conditionMet = unit.value(QStringLiteral("ConditionResult"), true).toBool();
    s.exitStatus = svc.value(QStringLiteral("ExecMainStatus")).toInt();
    s.result = svc.value(QStringLiteral("Result")).toString();
    s.started = svc.value(QStringLiteral("ExecMainStartTimestamp")).toULongLong();
    s.exited = svc.value(QStringLiteral("ExecMainExitTimestamp")).toULongLong();
    return s;
}

void Systemd::ref(const QString &unit) const
{
    QDBusMessage load = QDBusMessage::createMethodCall(kSystemd, kSystemdPath, kSystemdManager, QStringLiteral("LoadUnit"));
    load << unit;
    const QDBusMessage loaded = QDBusConnection::systemBus().call(load, QDBus::Block, 5000);
    if (loaded.type() != QDBusMessage::ReplyMessage)
        return;
    const QString path = loaded.arguments().value(0).value<QDBusObjectPath>().path();
    QDBusConnection::systemBus().call(QDBusMessage::createMethodCall(kSystemd, path, QStringLiteral("org.freedesktop.systemd1.Unit"), QStringLiteral("Ref")),
                                      QDBus::Block, 5000);
}

QString Systemd::escapePath(const QString &path)
{
    // As systemd's unit_name_path_escape(): simplified, without the slashes at either end.
    QString simple = QDir::cleanPath(QLatin1Char('/') + path);
    while (simple.startsWith(QLatin1Char('/')))
        simple.remove(0, 1);
    if (simple.isEmpty())
        return QStringLiteral("-");
    const QByteArray bytes = simple.toUtf8();
    QString out;
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        const char c = bytes[i];
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ':' || c == '_' || c == '.';
        if (c == '/')
            out += QLatin1Char('-');
        else if (!plain || (i == 0 && c == '.'))
            out += QStringLiteral("\\x%1").arg(uchar(c), 2, 16, QLatin1Char('0'));
        else
            out += QLatin1Char(c);
    }
    return out;
}

QString Systemd::serviceResult(const QString &service) const
{
    return unitProperty(service, QStringLiteral("org.freedesktop.systemd1.Service"), QStringLiteral("Result")).toString();
}
