// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "writeblock.h"

#include "rescuestick.h"

#include <QFile>
#include <QFileInfo>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

namespace writeblock {

namespace {

QString state()
{
    const QString test = qEnvironmentVariable("DISKFORGE_WRITEBLOCK_STATE");
    return test.isEmpty() ? QStringLiteral("/run/diskforge-live") : test;
}

QString helper()
{
    return helperForTest.isEmpty() ? QStringLiteral("/usr/local/bin/diskforge-live-writeblock") : helperForTest;
}

} // namespace

bool available()
{
#ifdef Q_OS_WIN
    return false;
#endif
    return !helperForTest.isEmpty() || (rescue::runningInRescue() && QFileInfo(helper()).isExecutable());
}

bool isOn()
{
    return QFile::exists(state() + QStringLiteral("/writeblock"));
}

QStringList allowed()
{
    QFile f(state() + QStringLiteral("/writeblock-allowed"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromLocal8Bit(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

QStringList command(const QStringList &args)
{
    QStringList c;
#ifndef Q_OS_WIN
    if (::geteuid() != 0)
#endif
        c << QStringLiteral("sudo") << QStringLiteral("-n");
    c << QStringLiteral("env");
    for (const char *name : {"DISKFORGE_WRITEBLOCK_STATE", "DISKFORGE_WRITEBLOCK_ONLY"}) {
        if (qEnvironmentVariableIsSet(name))
            c << QString::fromLatin1(name) + QLatin1Char('=') + qEnvironmentVariable(name);
    }
    return c << helper() << args;
}

} // namespace writeblock
