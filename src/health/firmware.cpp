// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "firmware.h"

#include "format.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QLocale>

namespace {

const QString kService = QStringLiteral("org.freedesktop.fwupd");
const QString kInterface = QStringLiteral("org.freedesktop.fwupd");

QList<QVariantMap> maps(const QDBusMessage &reply)
{
    QList<QVariantMap> out;
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return out;
    const QVariant first = reply.arguments().constFirst();
    if (!first.canConvert<QDBusArgument>())
        return out;
    first.value<QDBusArgument>() >> out;
    return out;
}

QDBusMessage call(const QString &method, const QVariantList &args = {})
{
    QDBusMessage m = QDBusMessage::createMethodCall(kService, QStringLiteral("/"), kInterface, method);
    m.setArguments(args);
    return m;
}

QString text(const QVariantMap &m, const char *key)
{
    return cleanName(m.value(QLatin1String(key)).toString());
}

// "Samsung SSD 990 PRO 1TB" (UDisks) and "SSD 990 PRO 1TB" (fwupd, with the maker apart).
bool sameModel(const QString &udisks, const QString &fwupd)
{
    const QString a = udisks.simplified(), b = fwupd.simplified();
    return !b.isEmpty() && (a.compare(b, Qt::CaseInsensitive) == 0 || a.endsWith(QLatin1Char(' ') + b, Qt::CaseInsensitive));
}

} // namespace

QVector<firmware::Device> firmware::devices(const QList<QVariantMap> &reply)
{
    QVector<Device> out;
    for (const QVariantMap &m : reply) {
        Device d;
        d.id = m.value(QStringLiteral("DeviceId")).toString();
        d.name = text(m, "Name");
        d.vendor = text(m, "Vendor");
        d.version = text(m, "Version");
        d.serial = text(m, "Serial");
        d.plugin = m.value(QStringLiteral("Plugin")).toString();
        d.flags = m.value(QStringLiteral("Flags")).toULongLong();
        if (!d.id.isEmpty())
            out.push_back(d);
    }
    return out;
}

int firmware::match(const QVector<Device> &devices, const QString &serial, const QString &model, const QString &revision)
{
    if (!serial.isEmpty()) {
        for (int i = 0; i < devices.size(); ++i) {
            if (devices[i].serial == serial)
                return i;
        }
    }
    int found = -1;
    for (int i = 0; i < devices.size(); ++i) {
        const Device &d = devices[i];
        if (!revision.isEmpty() && d.version == revision && sameModel(model, d.name)
            && (serial.isEmpty() || d.serial.isEmpty())) {
            if (found >= 0)
                return -1; // two alike: can't tell which is which
            found = i;
        }
    }
    return found;
}

firmware::Result firmware::outcome(const Device &device, const QList<QVariantMap> &upgrades, const QString &errorName,
                                   const QString &errorMessage, const QList<QVariantMap> &remotes)
{
    Result r;
    r.version = device.version;
    // When fwupd last downloaded a firmware list (download remotes only; "never" is all ones).
    qint64 newest = 0;
    for (const QVariantMap &remote : remotes) {
        const quint64 when = remote.value(QStringLiteral("ModificationTime")).toULongLong();
        if (remote.value(QStringLiteral("Enabled")).toBool() && remote.value(QStringLiteral("Type")).toUInt() == 1 && when > 0
            && when < quint64(4102444800)) // before 2100: anything later isn't a real date
            newest = qMax(newest, qint64(when));
    }
    if (newest > 0)
        r.listDate = QDateTime::fromSecsSinceEpoch(newest);

    if (!device.updatable()) {
        r.state = Result::State::NotUpdatable;
    } else if (!upgrades.isEmpty()) {
        r.state = Result::State::Available;
        r.newVersion = text(upgrades.constFirst(), "Version"); // fwupd lists the newest first
        if (r.newVersion.isEmpty())
            r.newVersion = QObject::tr("a newer version");
    } else if (errorName == QLatin1String("org.freedesktop.fwupd.NothingToDo")) {
        r.state = r.listDate.isValid() ? Result::State::UpToDate : Result::State::NoList;
    } else if (errorName == QLatin1String("org.freedesktop.fwupd.NotSupported") && !r.listDate.isValid()) {
        r.state = Result::State::NoList; // "no components in silo"
    } else if (errorName.isEmpty()) {
        r.state = r.listDate.isValid() ? Result::State::UpToDate : Result::State::NoList;
    } else {
        r.state = Result::State::Error;
        r.error = cleanName(errorMessage.left(200));
    }
    return r;
}

QString firmware::describe(const Result &r)
{
    switch (r.state) {
    case Result::State::NotInstalled:
        return QObject::tr("Install fwupd to look for firmware updates.");
    case Result::State::NotRunning:
        return {};
    case Result::State::Unknown:
        return QObject::tr("fwupd doesn't know this drive, so no firmware updates through it.");
    case Result::State::NotUpdatable:
        return QObject::tr("fwupd can't update this drive's firmware.");
    case Result::State::NoList:
        return QObject::tr("fwupd hasn't downloaded its list of firmware yet. Run fwupdmgr refresh, or turn on firmware "
                           "updates in your software center, then check again.");
    case Result::State::UpToDate:
        return QObject::tr("No newer firmware known (fwupd's list is from %1).")
            .arg(QLocale().toString(r.listDate.date(), QLocale::ShortFormat));
    case Result::State::Available:
        return QObject::tr("Firmware %1 is available. DiskForge doesn't install firmware: use your software center's "
                           "firmware updates, or fwupdmgr update. Back up first.").arg(r.newVersion);
    case Result::State::Error:
        return QObject::tr("fwupd: %1").arg(r.error);
    }
    return {};
}

bool firmware::running()
{
    QDBusConnectionInterface *bus = QDBusConnection::systemBus().interface();
    return bus && bus->isServiceRegistered(kService);
}

bool firmware::installed()
{
    QDBusConnectionInterface *bus = QDBusConnection::systemBus().interface();
    if (!bus)
        return false;
    if (bus->isServiceRegistered(kService))
        return true;
    const QDBusReply<QStringList> names = bus->activatableServiceNames();
    return names.isValid() && names.value().contains(kService);
}

firmware::Check::Check(QObject *parent)
    : QObject(parent)
{
}

void firmware::Check::start(const QString &serial, const QString &model, const QString &revision)
{
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call(QStringLiteral("GetDevices")), 60000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, serial, model, revision] {
        watcher->deleteLater();
        const QDBusMessage reply = watcher->reply();
        if (reply.type() != QDBusMessage::ReplyMessage) {
            Result r;
            r.state = Result::State::Error;
            r.error = cleanName(reply.errorMessage().left(200));
            emit finished(r);
            return;
        }
        const QVector<Device> list = devices(maps(reply));
        const int i = match(list, serial, model, revision);
        if (i < 0) {
            Result r;
            r.state = Result::State::Unknown;
            emit finished(r);
            return;
        }
        askUpgrades(list[i]);
    });
}

void firmware::Check::askUpgrades(const Device &device)
{
    m_device = device;
    if (!device.updatable()) {
        emit finished(outcome(device, {}, {}, {}, {}));
        return;
    }
    auto *upgrades = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call(QStringLiteral("GetUpgrades"), {device.id}), 60000), this);
    connect(upgrades, &QDBusPendingCallWatcher::finished, this, [this, upgrades] {
        upgrades->deleteLater();
        const QDBusMessage upgradeReply = upgrades->reply();
        auto *remotes = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call(QStringLiteral("GetRemotes")), 60000), this);
        connect(remotes, &QDBusPendingCallWatcher::finished, this, [this, remotes, upgradeReply] {
            remotes->deleteLater();
            const bool failed = upgradeReply.type() != QDBusMessage::ReplyMessage;
            emit finished(outcome(m_device, failed ? QList<QVariantMap>() : maps(upgradeReply), failed ? upgradeReply.errorName() : QString(),
                                  failed ? upgradeReply.errorMessage() : QString(), maps(remotes->reply())));
        });
    });
}
