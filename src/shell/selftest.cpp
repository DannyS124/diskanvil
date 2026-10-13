// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// diskanvil-shell --selftest: the shell used the way a person uses it, with real DiskAnvil
// windows in it: opened from the dock, moved, maximised, minimised and got back, sized, Alt+Tab,
// closed, and the power menu and the grid of everything. PASS or FAIL for each.

#include "selftest.h"

#include <QDBusInterface>
#include <QDBusReply>
#include <QElapsedTimer>
#include <QQmlContext>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QTextStream>

#include <functional>

namespace {

QTextStream out(stdout);
int failed = 0;

void report(bool ok, const QString &what, const QString &detail = {})
{
    if (!ok)
        ++failed;
    out << (ok ? "PASS  " : "FAIL  ") << what << (detail.isEmpty() ? QString() : QStringLiteral("  (%1)").arg(detail)) << Qt::endl;
}

bool waitUntil(const std::function<bool()> &done, int ms)
{
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < ms)
        QTest::qWait(50);
    return done();
}

} // namespace

int runSelfTest(QQmlApplicationEngine &engine)
{
    QObject *comp = engine.rootObjects().first();
    QQuickWindow *screen = comp->findChild<QQuickWindow *>();
    if (!screen) {
        report(false, "the shell has a screen");
        return 1;
    }
    screen->resize(1600, 1000);
    QTest::qWait(300);
    const auto windows = [comp] {
        QList<QQuickItem *> list;
        for (const QVariant &v : comp->property("windows").toList())
            list << qobject_cast<QQuickItem *>(v.value<QObject *>());
        return list;
    };
    const auto active = [comp] { return qobject_cast<QQuickItem *>(comp->property("active").value<QObject *>()); };
    const auto dockItem = [comp](int i) { return comp->property("dockItems").toList().value(i); };
    const auto launch = [comp](const QVariant &item) { QMetaObject::invokeMethod(comp, "launch", Q_ARG(QVariant, item)); };
    const auto rect = [](QQuickItem *w) { return QRectF(w->x(), w->y(), w->width(), w->height()); };
    const auto click = [screen](QPointF at) { QTest::mouseClick(screen, Qt::LeftButton, {}, at.toPoint()); QTest::qWait(150); };
    const auto doubleClick = [screen](QPointF at) { QTest::mouseDClick(screen, Qt::LeftButton, {}, at.toPoint()); QTest::qWait(150); };
    const auto drag = [screen](QPointF from, QPointF to) {
        QTest::mousePress(screen, Qt::LeftButton, {}, from.toPoint());
        for (int i = 1; i <= 8; ++i)
            QTest::mouseMove(screen, (from + (to - from) * i / 8).toPoint(), 20);
        QTest::mouseRelease(screen, Qt::LeftButton, {}, to.toPoint());
        QTest::qWait(200);
    };
    const QQuickItem *dock = nullptr;
    for (QQuickItem *i : screen->findChildren<QQuickItem *>()) {
        if (i->objectName() == QLatin1String("everything"))
            dock = i;
    }
    if (qEnvironmentVariableIsSet("SHELL_TEST_NAMES")) {
        QStringList names;
        for (QObject *o : screen->findChildren<QObject *>())
            if (!o->objectName().isEmpty())
                names << o->objectName();
        out << "named: " << names.join(QLatin1Char(' ')) << Qt::endl;
    }

    // Opened from the dock: placed between the bar and the dock, in front.
    launch(dockItem(0));
    const bool opened = waitUntil([&] { return windows().size() == 1 && windows().first()->isVisible() && windows().first()->width() > 100; }, 30000);
    report(opened, "a tool opened from the dock becomes a window", QStringLiteral("%1 window(s)").arg(windows().size()));
    if (!opened)
        return 1;
    QQuickItem *main = windows().first();
    QTest::qWait(1500); // its size settles
    const QRectF placed = rect(main);
    report(placed.top() >= 36 && dock && placed.bottom() <= dock->mapToScene(QPointF(0, 0)).y() + 1 && active() == main,
           "it's placed between the bar and the dock, and has the keys",
           QStringLiteral("%1,%2 %3x%4, the one in front %5").arg(placed.x()).arg(placed.y()).arg(placed.width()).arg(placed.height())
               .arg(active() == main ? QStringLiteral("is it") : active() ? QStringLiteral("is another") : QStringLiteral("is none")));
    // Moved by its title.
    const bool maximisedFirst = main->property("maximized").toBool();
    if (maximisedFirst) {
        doubleClick(placed.topLeft() + QPointF(placed.width() / 2, 15));
        waitUntil([&] { return !main->property("maximized").toBool(); }, 3000);
        QTest::qWait(800);
    }
    QRectF before = rect(main);
    drag(before.topLeft() + QPointF(before.width() / 2, 15), before.topLeft() + QPointF(before.width() / 2 - 120, 15 + 60));
    // (A drag starts once the mouse has gone a few pixels.)
    report(qAbs(main->x() - (before.x() - 120)) < 20 && qAbs(main->y() - (before.y() + 60)) < 20, "dragged by its title, it moves",
           QStringLiteral("from %1,%2 to %3,%4").arg(before.x()).arg(before.y()).arg(main->x()).arg(main->y()));
    // Maximised with a double-click, and back.
    before = rect(main);
    doubleClick(before.topLeft() + QPointF(before.width() / 2, 15));
    const bool maxed = waitUntil([&] { return main->x() == 0 && main->y() == 36 && qAbs(main->width() - screen->width()) < 2; }, 5000);
    report(maxed, "a double-click on its title fills the room", QStringLiteral("%1,%2 %3x%4").arg(main->x()).arg(main->y()).arg(main->width()).arg(main->height()));
    doubleClick(QPointF(screen->width() / 2.0, 36 + 15));
    const bool back = waitUntil([&] { return qAbs(main->x() - before.x()) < 3 && qAbs(main->width() - before.width()) < 3; }, 5000);
    report(back, "and another puts it back where it was",
           QStringLiteral("now %1,%2 %3 wide, was %4,%5 %6 wide").arg(main->x()).arg(main->y()).arg(main->width()).arg(before.x()).arg(before.y()).arg(before.width()));
    // Sized from its right edge.
    QTest::qWait(500);
    before = rect(main);
    drag(QPointF(before.right() - 1, before.center().y()), QPointF(before.right() - 1 - 150, before.center().y()));
    const bool sized = waitUntil([&] { return qAbs(main->width() - (before.width() - 150)) < 12; }, 5000);
    report(sized, "dragged by its edge, it's sized", QStringLiteral("%1 wide, was %2").arg(main->width()).arg(before.width()));
    // Minimised, and back from the dock.
    before = rect(main);
    click(QPointF(before.right() - 6 - 30 * 3 + 15, before.top() + 17));
    report(!main->isVisible() && active() != main, "the minimise button puts it out of sight");
    launch(dockItem(0));
    QTest::qWait(300);
    report(main->isVisible() && active() == main && windows().size() == 1, "its dock button brings it back, without starting another");
    // A second tool, then Alt+Tab between them.
    launch(dockItem(1));
    const bool two = waitUntil([&] { return windows().size() == 2 && windows().last()->isVisible(); }, 30000);
    QTest::qWait(1500);
    QQuickItem *second = two ? windows().last() : nullptr;
    report(two && active() == second, "a second tool opens in front, with the keys");
    QTest::keyClick(screen, Qt::Key_Tab, Qt::AltModifier);
    QTest::qWait(200);
    report(active() == main && main->z() > (second ? second->z() : 0), "Alt+Tab goes to the other window");
    // Closed with its button: the program closes.
    if (second) {
        comp->setProperty("active", QVariant::fromValue<QObject *>(second));
        QMetaObject::invokeMethod(comp, "raise", Q_ARG(QVariant, QVariant::fromValue<QObject *>(second)));
        QTest::qWait(200);
        const QRectF s = rect(second);
        click(QPointF(s.right() - 6 - 15, s.top() + 17));
        report(waitUntil([&] { return windows().size() == 1; }, 10000), "the close button closes the program");
    }
    // The power menu and the grid of everything.
    QQuickItem *power = nullptr, *powerButton = nullptr, *grid = nullptr;
    for (QQuickItem *i : screen->findChildren<QQuickItem *>()) {
        if (i->objectName() == QLatin1String("powerMenu"))
            power = i;
        else if (i->objectName() == QLatin1String("powerButton"))
            powerButton = i;
        else if (i->objectName() == QLatin1String("grid"))
            grid = i;
    }
    if (powerButton)
        click(powerButton->mapToScene(QPointF(powerButton->width() / 2, powerButton->height() / 2)));
    report(power && power->isVisible(), "the power button opens its menu");
    if (powerButton)
        click(powerButton->mapToScene(QPointF(powerButton->width() / 2, powerButton->height() / 2)));
    if (dock)
        click(dock->mapToScene(QPointF(dock->width() / 2, dock->height() / 2)));
    const bool gridShown = grid && grid->isVisible();
    QTest::keyClick(screen, Qt::Key_Escape);
    report(gridShown && !grid->isVisible(), "the first dock button shows everything, and Esc closes it");

    // Whatever's still open closes with the test.
    for (QQuickItem *w : windows())
        QMetaObject::invokeMethod(w, "close");
    waitUntil([&] { return windows().isEmpty(); }, 10000);
    out << (failed ? QStringLiteral("%1 step(s) failed").arg(failed) : QStringLiteral("All steps passed")) << Qt::endl;
    return failed ? 1 : 0;
}
