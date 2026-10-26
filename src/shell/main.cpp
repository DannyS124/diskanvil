// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// diskanvil-shell: DiskAnvil Live's own screen, a Wayland compositor of its own (no desktop
// underneath). It draws the bar along the top, the dock and the windows' frames; DiskAnvil, its
// tools, a terminal and Firefox are its windows.

#include "selftest.h"
#include "input.h"
#include "network.h"
#include "shell.h"

#include <QGuiApplication>
#include <QFile>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>

#include <memory>

namespace {

// What the drag-and-drop test's target window was given, written where the test reads it.
class DropNote : public QObject
{
    Q_OBJECT
public:
    explicit DropNote(QString path)
        : m_path(std::move(path))
    {
    }
    Q_INVOKABLE void dropped(const QString &text)
    {
        QFile f(m_path);
        if (f.open(QIODevice::WriteOnly))
            f.write(text.toUtf8());
    }

private:
    QString m_path;
};

// --drag-test-window source|target FILE: a program window for the self-test's drag and drop,
// started inside the shell like any other. The source has a box to drag (text "anvil-drop-test");
// the target writes what's dropped on it to FILE.
int dragTestWindow(QGuiApplication &app, const QString &role, const QString &file)
{
    DropNote note(file);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("note"), &note);
    const QByteArray qml = role == QLatin1String("source") ? QByteArrayLiteral(R"(
        import QtQuick
        Window {
            title: "drag source"; width: 300; height: 240; visible: true; color: "#202838"
            Rectangle {
                id: box; x: 90; y: 60; width: 120; height: 120; color: "#1f6fff"
                Drag.active: area.drag.active; Drag.dragType: Drag.Automatic
                Drag.supportedActions: Qt.CopyAction; Drag.mimeData: { "text/plain": "anvil-drop-test" }
                MouseArea { id: area; anchors.fill: parent; drag.target: parent }
            }
        })")
                                                                   : QByteArrayLiteral(R"(
        import QtQuick
        Window {
            title: "drop target"; width: 300; height: 240; visible: true; color: "#203828"
            DropArea { anchors.fill: parent; onDropped: (drop) => { note.dropped(drop.text); drop.accept(Qt.CopyAction) } }
        })");
    engine.loadData(qml);
    return engine.rootObjects().isEmpty() ? 1 : app.exec();
}

} // namespace

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
    if (const int i = app.arguments().indexOf(QStringLiteral("--drag-test-window")); i > 0 && i + 2 < app.arguments().size())
        return dragTestWindow(app, app.arguments()[i + 1], app.arguments()[i + 2]);
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
    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("icon"), new IconProvider);
    engine.rootContext()->setContextProperty(QStringLiteral("shell"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("network"), &network);
    engine.loadFromModule("DiskAnvilShell", "Main");
    if (engine.rootObjects().isEmpty())
        return 1;
    // Where the mouse is, for the pointer the shell draws.
    for (QQuickWindow *w : engine.rootObjects().first()->findChildren<QQuickWindow *>())
        w->installEventFilter(new PointerWatch(&shell, w));
#ifdef DISKANVIL_SHELL_INPUT
    // Straight on the screen, the mouse, touchpad and keyboard are the shell's own to read (the
    // session turns Qt's off: QT_QPA_FB_DISABLE_INPUT).
    std::unique_ptr<Input> input;
    if (shell.drawsPointer() && qEnvironmentVariableIntValue("QT_QPA_FB_DISABLE_INPUT")) {
        if (QQuickWindow *w = engine.rootObjects().first()->findChild<QQuickWindow *>()) {
            input = std::make_unique<Input>(w);
            QObject::connect(&shell, &Shell::keyboardChanged, input.get(), [&shell, &input] { input->setLayout(shell.keyboardLayout(), shell.keyboardVariant()); });
            input->setLayout(shell.keyboardLayout(), shell.keyboardVariant());
        }
    }
#endif

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

#include "main.moc"
