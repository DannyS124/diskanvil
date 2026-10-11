// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Is there newer firmware for a drive? Asked of fwupd over the system bus. DiskForge only
// asks: it never downloads or installs firmware, and fwupd answers from the list it
// downloaded itself. fwupd is only asked when it's already running, or when the user
// presses Check, since asking starts it otherwise (a root daemon that probes hardware).

#include <QDateTime>
#include <QObject>
#include <QVariantMap>
#include <QVector>

namespace firmware {

struct Device {
    QString id; // fwupd's DeviceId
    QString name;
    QString vendor;
    QString version;
    QString serial;
    QString plugin;
    quint64 flags = 0;
    bool updatable() const { return flags & (quint64(1) << 1); }
};

struct Result {
    enum class State {
        NotInstalled, // no fwupd
        NotRunning,   // not asked yet
        Unknown,      // fwupd doesn't know this drive
        NotUpdatable, // it knows it, but can't update it
        NoList,       // it hasn't downloaded its firmware list
        UpToDate,     // nothing newer in its list
        Available,
        Error
    };
    State state = State::NotRunning;
    QString version;    // installed, as fwupd sees it
    QString newVersion; // when one is available
    QDateTime listDate; // when fwupd last downloaded its list
    QString error;
};

QVector<Device> devices(const QList<QVariantMap> &reply); // from GetDevices
// The drive's entry: by serial, or by model and firmware version when there's no serial
// (only when exactly one entry fits). -1 when there's none.
int match(const QVector<Device> &devices, const QString &serial, const QString &model, const QString &revision);
// From GetUpgrades' reply (or the error it gave) and GetRemotes'.
Result outcome(const Device &device, const QList<QVariantMap> &upgrades, const QString &errorName, const QString &errorMessage,
               const QList<QVariantMap> &remotes);
QString describe(const Result &result); // plain words

bool installed(); // running, or can be started
bool running();

// Asks fwupd about one drive, without blocking. Emits finished() once.
class Check : public QObject
{
    Q_OBJECT
public:
    explicit Check(QObject *parent = nullptr);
    void start(const QString &serial, const QString &model, const QString &revision);

signals:
    void finished(const firmware::Result &result);

private:
    void askUpgrades(const Device &device);
    Device m_device;
};

} // namespace firmware
