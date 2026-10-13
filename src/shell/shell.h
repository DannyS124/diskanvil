// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QObject>
#include <QQuickImageProvider>
#include <QStringList>

// What the shell's QML can't do itself: start programs (as windows of the shell), power off,
// and say which drives there are.
class Shell : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString version READ version CONSTANT)
public:
    QString version() const;
    // Starts a program to show as a window here; its process number, or 0.
    Q_INVOKABLE qint64 launch(const QString &program, const QStringList &arguments = {});
    Q_INVOKABLE bool running(qint64 pid) const;
    // "poweroff", "reboot" or "firmware" (restart into the UEFI setup).
    Q_INVOKABLE void power(const QString &how);
    // Where programs find this compositor: set once it's listening.
    Q_INVOKABLE void setSocket(const QString &name) { m_socket = name; }
    Q_INVOKABLE bool firmwareSetupPossible() const;

private:
    QString m_socket;
};

// image://icon/<name>: an icon of the theme (Breeze), or one of DiskAnvil Live's own (in the
// shell itself), at the size asked for.
class IconProvider : public QQuickImageProvider
{
public:
    IconProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
