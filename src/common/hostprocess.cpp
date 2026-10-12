// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hostprocess.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

bool host::inFlatpak()
{
    static const bool yes = QFileInfo::exists(QStringLiteral("/.flatpak-info"));
    return yes;
}

#ifdef Q_OS_WIN
QString host::windowsProgram(const QString &name)
{
    QString found = QStandardPaths::findExecutable(name, {QCoreApplication::applicationDirPath()});
    if (found.isEmpty())
        found = QStandardPaths::findExecutable(name);
    if (found.isEmpty()) {
        const QString programFiles = qEnvironmentVariable("ProgramFiles", QStringLiteral("C:/Program Files"));
        found = QStandardPaths::findExecutable(name, {programFiles + QStringLiteral("/smartmontools/bin")});
    }
    return found.isEmpty() ? name : QDir::toNativeSeparators(found);
}
#endif

bool host::programExists(const QString &name)
{
#ifdef Q_OS_WIN
    return windowsProgram(name) != name || QFileInfo(name).isExecutable();
#endif
    if (!inFlatpak())
        return !QStandardPaths::findExecutable(name).isEmpty() || QFileInfo(name).isExecutable();
    const QString path = name.startsWith(QLatin1Char('/')) ? name : QStringLiteral("/usr/bin/") + name;
    return QProcess::execute(QStringLiteral("flatpak-spawn"), {QStringLiteral("--host"), QStringLiteral("test"), QStringLiteral("-x"), path}) == 0;
}

QStringList host::command(QStringList command)
{
    if (inFlatpak())
        return QStringList{QStringLiteral("flatpak-spawn"), QStringLiteral("--host")} + command;
    return command;
}

int host::run(QStringList command)
{
    if (inFlatpak())
        return QProcess::execute(QStringLiteral("flatpak-spawn"), QStringList{QStringLiteral("--host")} + command);
    QString program = command.takeFirst();
#ifdef Q_OS_WIN
    program = windowsProgram(program);
#endif
    return QProcess::execute(program, command);
}
