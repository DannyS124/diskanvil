// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hostprocess.h"

#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

bool host::inFlatpak()
{
    static const bool yes = QFileInfo::exists(QStringLiteral("/.flatpak-info"));
    return yes;
}

bool host::programExists(const QString &name)
{
    if (!inFlatpak())
        return !QStandardPaths::findExecutable(name).isEmpty() || QFileInfo(name).isExecutable();
    const QString path = name.startsWith(QLatin1Char('/')) ? name : QStringLiteral("/usr/bin/") + name;
    return QProcess::execute(QStringLiteral("flatpak-spawn"), {QStringLiteral("--host"), QStringLiteral("test"), QStringLiteral("-x"), path}) == 0;
}

int host::run(QStringList command)
{
    if (inFlatpak())
        return QProcess::execute(QStringLiteral("flatpak-spawn"), QStringList{QStringLiteral("--host")} + command);
    const QString program = command.takeFirst();
    return QProcess::execute(program, command);
}
