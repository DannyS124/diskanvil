// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shell.h"

#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QPainter>
#include <QProcess>

#include <signal.h>

QString Shell::version() const
{
    return QStringLiteral(DISKANVIL_VERSION);
}

qint64 Shell::launch(const QString &program, const QStringList &arguments)
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    // A window here, whatever the shell itself is drawn with.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("WAYLAND_DISPLAY"), m_socket);
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("wayland"));
    env.insert(QStringLiteral("MOZ_ENABLE_WAYLAND"), QStringLiteral("1"));
    env.insert(QStringLiteral("GDK_BACKEND"), QStringLiteral("wayland"));
    env.insert(QStringLiteral("XDG_SESSION_TYPE"), QStringLiteral("wayland"));
    env.remove(QStringLiteral("QT_QUICK_BACKEND"));
    env.remove(QStringLiteral("DISPLAY"));
    process.setProcessEnvironment(env);
    qint64 pid = 0;
    return process.startDetached(&pid) ? pid : 0;
}

bool Shell::running(qint64 pid) const
{
    return pid > 0 && ::kill(pid_t(pid), 0) == 0;
}

bool Shell::firmwareSetupPossible() const
{
    return QFileInfo::exists(QStringLiteral("/sys/firmware/efi"));
}

void Shell::power(const QString &how)
{
    // logind lets the person at the PC do these without a password.
    if (how == QLatin1String("poweroff"))
        QProcess::startDetached(QStringLiteral("systemctl"), {QStringLiteral("poweroff")});
    else if (how == QLatin1String("reboot"))
        QProcess::startDetached(QStringLiteral("systemctl"), {QStringLiteral("reboot")});
    else if (how == QLatin1String("firmware"))
        QProcess::startDetached(QStringLiteral("systemctl"), {QStringLiteral("reboot"), QStringLiteral("--firmware-setup")});
}

QImage IconProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    const QSize want = requestedSize.isValid() && !requestedSize.isEmpty() ? requestedSize : QSize(48, 48);
    const QString own = QStringLiteral(":/shell/icons/%1.svg").arg(id);
    const QIcon icon = QFile::exists(own) ? QIcon(own) : QIcon::fromTheme(id, QIcon::fromTheme(QStringLiteral("application-x-executable")));
    QImage image(want, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    icon.paint(&painter, QRect(QPoint(0, 0), want));
    painter.end();
    if (size)
        *size = want;
    return image;
}
