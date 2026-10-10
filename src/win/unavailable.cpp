// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// The Linux services DiskForge talks to (systemd, snapper, fwupd) and libblkid don't exist on
// Windows. These stand in for them so the windows that mention them still build; the menu
// entries that would use them are hidden on Windows.

#include "firmware.h"
#include "snapper.h"
#include "systemd.h"

#include <QCoreApplication>
#include <QTimer>

namespace {

QString notOnWindows()
{
    return QCoreApplication::translate("Windows", "This needs Linux. It works in DiskForge Live.");
}

} // namespace

// systemd

Systemd::Systemd(QObject *parent)
    : QObject(parent)
{
}

void Systemd::startUnit(const QString &, const Done &done)
{
    QTimer::singleShot(0, this, [done] { done(false, notOnWindows()); });
}

void Systemd::stopUnit(const QString &, const Done &done)
{
    QTimer::singleShot(0, this, [done] { done(false, notOnWindows()); });
}

void Systemd::setTimerEnabled(const QString &, bool, const Done &done)
{
    QTimer::singleShot(0, this, [done] { done(false, notOnWindows()); });
}

bool Systemd::unitExists(const QString &) const
{
    return false;
}

QString Systemd::unitFileState(const QString &) const
{
    return {};
}

quint64 Systemd::timerLastTrigger(const QString &) const
{
    return 0;
}

QString Systemd::serviceResult(const QString &) const
{
    return {};
}

quint64 Systemd::serviceLastRun(const QString &) const
{
    return 0;
}

Systemd::ServiceState Systemd::serviceState(const QString &) const
{
    return {};
}

void Systemd::ref(const QString &) const
{
}

QString Systemd::escapePath(const QString &path)
{
    return path;
}

void Systemd::callThen(const QString &, const QVariantList &, const Done &, const std::function<void(const QDBusMessage &)> &)
{
}

QVariant Systemd::unitProperty(const QString &, const QString &, const QString &) const
{
    return {};
}

// snapper

QVector<Subvolume> snapper::mountedSubvolumes(const QByteArray &)
{
    return {};
}

bool snapper::available()
{
    return false;
}

QVector<SnapperConfig> snapper::configs(QString *error)
{
    if (error)
        *error = notOnWindows();
    return {};
}

QVector<Snapshot> snapper::snapshots(const QString &, QString *error)
{
    if (error)
        *error = notOnWindows();
    return {};
}

// fwupd

QVector<firmware::Device> firmware::devices(const QList<QVariantMap> &)
{
    return {};
}

int firmware::match(const QVector<Device> &, const QString &, const QString &, const QString &)
{
    return -1;
}

firmware::Result firmware::outcome(const Device &, const QList<QVariantMap> &, const QString &, const QString &,
                                   const QList<QVariantMap> &)
{
    Result r;
    r.state = Result::State::NotInstalled;
    return r;
}

QString firmware::describe(const Result &)
{
    return notOnWindows();
}

bool firmware::installed()
{
    return false;
}

bool firmware::running()
{
    return false;
}

firmware::Check::Check(QObject *parent)
    : QObject(parent)
{
}

void firmware::Check::start(const QString &, const QString &, const QString &)
{
    QTimer::singleShot(0, this, [this] {
        Result r;
        r.state = Result::State::NotInstalled;
        emit finished(r);
    });
}

void firmware::Check::askUpgrades(const Device &)
{
}
