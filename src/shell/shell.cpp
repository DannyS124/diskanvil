// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shell.h"

#include <QFile>
#include <QGuiApplication>
#include <QFileInfo>
#include <QIcon>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>

#include <signal.h>

Shell::Shell(QObject *parent)
    : QObject(parent)
{
    m_layout = qEnvironmentVariable("XKB_DEFAULT_LAYOUT");
    m_variant = qEnvironmentVariable("XKB_DEFAULT_VARIANT");
    connect(&m_keyboardFile, &QFileSystemWatcher::fileChanged, this, &Shell::readKeyboard);
    readKeyboard();
}

void Shell::readKeyboard()
{
    const QString path = QStringLiteral("/etc/default/keyboard");
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        QString layout, variant;
        for (const QByteArray &line : file.readAll().split('\n')) {
            const QString l = QString::fromUtf8(line).trimmed();
            const auto value = [&l] { return l.section(QLatin1Char('='), 1).remove(QLatin1Char('"')).trimmed(); };
            if (l.startsWith(QLatin1String("XKBLAYOUT=")))
                layout = value();
            else if (l.startsWith(QLatin1String("XKBVARIANT=")))
                variant = value();
        }
        if (!layout.isEmpty() && (layout != m_layout || variant != m_variant)) {
            m_layout = layout;
            m_variant = variant;
            Q_EMIT keyboardChanged();
        }
    }
    // An editor (sed -i) puts a new file in its place: watched again.
    if (QFile::exists(path) && !m_keyboardFile.files().contains(path))
        m_keyboardFile.addPath(path);
}

QString Shell::openAtStart() const
{
    // Only a tool's id: lower-case words joined by dashes. DiskAnvil itself says if there's no such tool.
    const QString id = qEnvironmentVariable("DISKANVIL_LIVE_OPEN").trimmed();
    static const QRegularExpression tool(QStringLiteral("^[a-z]+(-[a-z]+)*$"));
    return tool.match(id).hasMatch() ? id : QString();
}

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

bool Shell::drawsPointer() const
{
    const QString platform = QGuiApplication::platformName();
    return platform == QLatin1String("linuxfb") || platform == QLatin1String("eglfs") || qEnvironmentVariableIsSet("DISKANVIL_SHELL_POINTER");
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

bool PointerWatch::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::HoverMove:
        m_shell->setPointer(static_cast<QSinglePointEvent *>(event)->scenePosition());
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
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
