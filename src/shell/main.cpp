// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// diskanvil-shell: DiskAnvil Live's own screen, a Wayland compositor of its own (no desktop
// underneath). It draws the bar along the top, the dock and the windows' frames; DiskAnvil, its
// tools, a terminal and Firefox are its windows.

#include "selftest.h"
#include "network.h"
#include "notifications.h"
#include "shell.h"

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>

int main(int argc, char **argv)
{
    // Software drawing when there's no graphics driver to draw with (or when it's asked for).
    if (qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND") && (qgetenv("QT_QPA_PLATFORM").startsWith("linuxfb") || qgetenv("QT_QPA_PLATFORM") == "offscreen"))
        qputenv("QT_QUICK_BACKEND", "software");
    // The platform's own pointer is hidden: the shell draws the one the program under it asks for.
    qputenv("QT_QPA_FB_HIDECURSOR", "1");
    qputenv("QT_QPA_EGLFS_HIDECURSOR", "1");
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("diskanvil-shell"));
    // Drawing straight to the screen there's no desktop to say where icon themes are.
    QStringList themes = QIcon::themeSearchPaths();
    for (const QString &dir : QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("icons"), QStandardPaths::LocateDirectory))
        if (!themes.contains(dir))
            themes << dir;
    QIcon::setThemeSearchPaths(themes);
    QIcon::setThemeName(QStringLiteral("breeze-dark"));
    QIcon::setFallbackThemeName(QStringLiteral("breeze"));

    Shell shell;
    Network network;
    Notifications notifications;
    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("icon"), new IconProvider);
    engine.rootContext()->setContextProperty(QStringLiteral("shell"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("network"), &network);
    engine.rootContext()->setContextProperty(QStringLiteral("notifications"), &notifications);
    engine.loadFromModule("DiskAnvilShell", "Main");
    if (engine.rootObjects().isEmpty())
        return 1;
    // Where the mouse is, for the pointer the shell draws.
    for (QQuickWindow *w : engine.rootObjects().first()->findChildren<QQuickWindow *>())
        w->installEventFilter(new PointerWatch(&shell, w));

    // For checking it without a screen: --shot FILE MS saves what's on it, then quits;
    // --selftest uses it with real windows in it.
    const QStringList args = app.arguments();
    if (args.contains(QStringLiteral("--selftest"))) {
        int result = 1;
        QTimer::singleShot(0, &app, [&engine, &result] {
            result = runSelfTest(engine);
            QCoreApplication::quit();
        });
        app.exec();
        return result;
    }
    if (const int i = args.indexOf(QStringLiteral("--shot")); i > 0 && i + 2 < args.size()) {
        const QString file = args[i + 1];
        QTimer::singleShot(args[i + 2].toInt(), &app, [&engine, file] {
            for (QQuickWindow *w : engine.rootObjects().first()->findChildren<QQuickWindow *>())
                w->grabWindow().save(file);
            QCoreApplication::quit();
        });
    }
    return app.exec();
}
