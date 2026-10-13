// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QPointF>
#include <QQuickImageProvider>
#include <QStringList>

// What the shell's QML can't do itself: start programs (as windows of the shell), power off,
// and say which drives there are.
class Shell : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QPointF pointer READ pointer NOTIFY pointerChanged)
    Q_PROPERTY(bool drawsPointer READ drawsPointer CONSTANT)
    Q_PROPERTY(bool fullScreen READ fullScreen CONSTANT)
    Q_PROPERTY(QString keyboardLayout READ keyboardLayout NOTIFY keyboardChanged)
    Q_PROPERTY(QString keyboardVariant READ keyboardVariant NOTIFY keyboardChanged)
    // A DiskAnvil tool to open once the screen is up (the stick's settings.txt, "open = ..."): its id.
    Q_PROPERTY(QString openAtStart READ openAtStart CONSTANT)
public:
    // Drawing straight to the screen there's nothing else to draw the mouse pointer: the shell
    // does, where the mouse is.
    bool drawsPointer() const;
    bool fullScreen() const { return drawsPointer() || qEnvironmentVariableIsSet("DISKANVIL_SHELL_FULLSCREEN"); }
    QPointF pointer() const { return m_pointer; }
    void setPointer(QPointF at)
    {
        if (at != m_pointer) {
            m_pointer = at;
            Q_EMIT pointerChanged();
        }
    }
    // The keyboard layout programs get: /etc/default/keyboard's (diskanvil-live-keyboard changes
    // it, and it's followed at once), else what DiskAnvil Live was started with (XKB_DEFAULT_LAYOUT).
    explicit Shell(QObject *parent = nullptr);
    QString keyboardLayout() const { return m_layout; }
    QString openAtStart() const;
    QString keyboardVariant() const { return m_variant; }
    QString version() const;
    // Starts a program to show as a window here; its process number, or 0.
    Q_INVOKABLE qint64 launch(const QString &program, const QStringList &arguments = {});
    Q_INVOKABLE bool running(qint64 pid) const;
    // "poweroff", "reboot" or "firmware" (restart into the UEFI setup).
    Q_INVOKABLE void power(const QString &how);
    // Where programs find this compositor: set once it's listening.
    Q_INVOKABLE void setSocket(const QString &name) { m_socket = name; }
    Q_INVOKABLE bool firmwareSetupPossible() const;

Q_SIGNALS:
    void pointerChanged();
    void keyboardChanged();

private:
    QString m_socket;
    QPointF m_pointer;
    QString m_layout, m_variant;
    QFileSystemWatcher m_keyboardFile;
    void readKeyboard();
};

// Hears where the mouse is in the shell's window, whoever it's going to.
class PointerWatch : public QObject
{
public:
    PointerWatch(Shell *shell, QObject *parent) : QObject(parent), m_shell(shell) {}
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    Shell *m_shell;
};

// image://icon/<name>: an icon of the theme (Breeze), or one of DiskAnvil Live's own (in the
// shell itself), at the size asked for.
class IconProvider : public QQuickImageProvider
{
public:
    IconProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
