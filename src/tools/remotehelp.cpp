// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "remotehelp.h"

#include "rescuestick.h"

#include <QFileInfo>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>

namespace remotehelp {

namespace {

QString helper()
{
    return helperForTest.isEmpty() ? QStringLiteral("/usr/local/bin/diskanvil-live-remote") : helperForTest;
}

} // namespace

QString makePassword(int length)
{
    static const QString letters = QStringLiteral("abcdefghjkmnpqrstuvwxyz23456789");
    QString password;
    for (int i = 0; i < length; ++i)
        password += letters.at(int(QRandomGenerator::system()->bounded(quint32(letters.size()))));
    return password;
}

QStringList addresses()
{
    QStringList v4, v6;
    for (const QNetworkInterface &nic : QNetworkInterface::allInterfaces()) {
        const auto flags = nic.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) || (flags & QNetworkInterface::IsLoopBack))
            continue;
        // Not the ones containers and VMs on this PC make for themselves.
        if (nic.type() == QNetworkInterface::Virtual || nic.name().startsWith(QLatin1String("docker")) || nic.name().startsWith(QLatin1String("virbr"))
            || nic.name().startsWith(QLatin1String("podman")))
            continue;
        for (const QNetworkAddressEntry &entry : nic.addressEntries()) {
            const QHostAddress a = entry.ip();
            if (a.protocol() == QAbstractSocket::IPv4Protocol)
                v4 << a.toString();
            else if (a.protocol() == QAbstractSocket::IPv6Protocol && a.isGlobal() && !a.isUniqueLocalUnicast())
                v6 << a.toString();
        }
    }
    return v4 + v6;
}

QString x11vnc()
{
    return x11vncForTest.isEmpty() ? QStandardPaths::findExecutable(QStringLiteral("x11vnc")) : x11vncForTest;
}

bool available()
{
    if (!x11vncForTest.isEmpty())
        return true;
    return rescue::runningInRescue() && !x11vnc().isEmpty() && QFileInfo(helper()).isExecutable();
}

QStringList vncArguments(const QString &passwordFile)
{
    const QString display = qEnvironmentVariable("DISPLAY", QStringLiteral(":0"));
    return {QStringLiteral("-display"), display,          QStringLiteral("-passwdfile"), QStringLiteral("rm:") + passwordFile,
            QStringLiteral("-forever"), QStringLiteral("-shared"), QStringLiteral("-noxdamage"), QStringLiteral("-nolookup"),
            QStringLiteral("-norc")};
}

QString connectedFrom(const QString &line)
{
    static const QRegularExpression got(QStringLiteral("Got connection from client ([0-9A-Fa-f:.]+)"));
    return got.match(line).captured(1);
}

int clientsLeft(const QString &line)
{
    static const QRegularExpression count(QStringLiteral("client_count: (\\d+)"));
    const QRegularExpressionMatch m = count.match(line);
    return m.hasMatch() ? m.captured(1).toInt() : -1;
}

QStringList sshCommand(const QString &what)
{
    if (!helperForTest.isEmpty())
        return {helperForTest, what};
    return {QStringLiteral("sudo"), QStringLiteral("-n"), helper(), what};
}

} // namespace remotehelp
