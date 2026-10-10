// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "applog.h"
#include "format.h"
#include "homewindow.h"
#include "mainwindow.h"
#include "translations.h"
#include "udisks.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QMenu>
#include <QMessageBox>
#include <QScreen>
#include <QSysInfo>
#include <QTextStream>
#include <QTimer>

namespace {

// for comparing against lsblk
void dump(const QVector<Disk> &disks)
{
    QTextStream out(stdout);
    for (int i = 0; i < disks.size(); ++i) {
        const Disk &d = disks[i];
        out << "Disk " << i << "  " << d.device << "  " << d.model << "  " << formatSize(d.size)
            << "  " << tableName(d) << "  " << diskKind(d);
        if (d.isSystem)
            out << "  [system: " << d.systemReason << "]";
        if (d.health.state != Health::State::Unknown) {
            out << "  [health: " << d.health.summary;
            if (d.health.temperatureC > 0)
                out << ", " << qRound(d.health.temperatureC) << " °C";
            out << ", " << d.health.powerOnHours << " h]";
        }
        out << "\n";
        for (const Span &s : diskSpans(d)) {
            if (s.isFree()) {
                out << "    " << QStringLiteral("unallocated").leftJustified(14) << formatSize(s.size).rightJustified(11)
                    << "  at " << s.offset << "\n";
                continue;
            }
            const Volume &v = d.volumes[s.volume];
            out << "    " << shortDevice(v.device).leftJustified(14) << formatSize(v.size).rightJustified(11)
                << "  at " << v.offset << "  " << (v.fsType.isEmpty() ? QStringLiteral("-") : v.fsType)
                << "  " << volumeStatus(v);
            if (v.fsTotal)
                out << "  (" << formatSize(v.fsFree) << " free)";
            out << "\n";
        }
    }
}

} // namespace

int main(int argc, char *argv[])
{
    // These never open a window, so they shouldn't need a screen (over SSH, on a text console).
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if ((arg == "--dump" || arg == "--help" || arg == "-h" || arg == "--version" || arg == "-v")
            && qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
            qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    installTranslations();
    QApplication::setApplicationName(QStringLiteral("diskforge"));
    QApplication::setApplicationDisplayName(QStringLiteral("DiskForge"));
    QApplication::setApplicationVersion(QStringLiteral(APP_VERSION));
    QApplication::setDesktopFileName(QStringLiteral(APP_ID));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral(APP_ID), QIcon(QStringLiteral(":/data/" APP_ID ".svg"))));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("A Windows-style disk manager built on UDisks2."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption dumpOption(QStringLiteral("dump"), QStringLiteral("Print the disk layout and exit."));
    const QCommandLineOption screenshotOption(QStringLiteral("screenshot"),
                                              QStringLiteral("Render the window to <file> and exit."),
                                              QStringLiteral("file"));
    const QCommandLineOption selectOption(QStringLiteral("select"),
                                          QStringLiteral("Start with partition <device> selected."),
                                          QStringLiteral("device"));
    const QCommandLineOption menuOption(QStringLiteral("menu"),
                                        QStringLiteral("With --screenshot: render the right-click menu of the selection."));
    const QCommandLineOption logOption(QStringLiteral("log"),
                                       QStringLiteral("Write what DiskForge does to <file>. DISKFORGE_LOG=<file> does the same."),
                                       QStringLiteral("file"));
    const QCommandLineOption openOption(QStringLiteral("open"),
                                        QStringLiteral("Open just one tool: write-image, windows-usb, rescue-usb, check-stick or lost-files."),
                                        QStringLiteral("tool"));
    const QCommandLineOption homeOption(QStringLiteral("home"), QStringLiteral("Be DiskForge Live's home screen (its desktop)."));
    parser.addOptions({dumpOption, screenshotOption, selectOption, menuOption, logOption, openOption, homeOption});
    parser.addPositionalArgument(QStringLiteral("images"), QStringLiteral("Disk images (.iso, .img) to open."), QStringLiteral("[image...]"));
    parser.process(app);

    const QString logFile = parser.isSet(logOption) ? parser.value(logOption) : qEnvironmentVariable("DISKFORGE_LOG");
    if (!logFile.isEmpty() && applog::start(logFile)) {
        qCInfo(lcOps).noquote() << "DiskForge" << APP_VERSION << "started, Qt" << qVersion() << "on"
                                << QSysInfo::prettyProductName() << QSysInfo::kernelVersion();
    }

    UDisks udisks;
    if (!udisks.isAvailable()) {
#ifdef Q_OS_WIN
        const QString message = udisks.lastError();
#else
        const QString message = QStringLiteral("Can't reach UDisks2: %1\nIs udisks2 installed and running?")
                                    .arg(udisks.lastError());
#endif
        qCCritical(lcOps).noquote() << message;
        if (parser.isSet(dumpOption)) {
            QTextStream(stderr) << message << "\n";
            return 1;
        }
        QMessageBox::critical(nullptr, QStringLiteral("DiskForge"), message);
        return 1;
    }

    if (parser.isSet(dumpOption)) {
        dump(udisks.disks());
        return 0;
    }
    if (applog::enabled()) {
        qCInfo(lcOps).noquote() << QStringLiteral("UDisks %1, %2 drives").arg(udisks.daemonVersion()).arg(udisks.disks().size());
        for (const Disk &d : udisks.disks()) {
            qCInfo(lcOps).noquote() << " " << d.device << d.model << formatSize(d.size) << tableName(d)
                                    << (d.isSystem ? QStringLiteral("[system: %1]").arg(d.systemReason) : QString());
        }
        QObject::connect(&udisks, &UDisks::operationFinished, &app, [](bool ok, const QString &message) {
            qCInfo(lcOps).noquote() << (ok ? "result:" : "failed:") << message;
        });
    }

    if (parser.isSet(homeOption)) {
        HomeWindow home(&udisks, HomeWindow::Mode::Desktop);
        home.show();
        // A desktop window only gets the keyboard when asked; Tab then walks the tiles.
        QTimer::singleShot(500, &home, [&home] { home.activateWindow(); });
        const int status = app.exec();
        qCInfo(lcOps).noquote() << "Home screen closed";
        return status;
    }

    MainWindow window(&udisks);
    window.resize(1280, 800);
    // On a smaller screen (a small laptop, a VM, DiskForge Live) that would put the bottom of
    // the window under the taskbar, so it takes the room there is instead. Screenshots keep
    // the size they always have.
    const QScreen *screen = QGuiApplication::primaryScreen();
    const QRect room = screen ? screen->availableGeometry() : QRect();
    const bool small = room.isValid() && (room.width() < 1280 + 40 || room.height() < 800 + 60);
    if (parser.isSet(openOption)) {
        // Only the tool shows, see below.
    } else if (small && !parser.isSet(screenshotOption)) {
        window.showMaximized();
    } else {
        window.show();
    }
    for (const QString &image : parser.positionalArguments())
        udisks.openImage(image);
    if (parser.isSet(selectOption) && !window.selectDevice(parser.value(selectOption)))
        QTextStream(stderr) << "No partition " << parser.value(selectOption) << "\n";
    if (parser.isSet(openOption)) {
        // Just the tool, for launchers like the ones on DiskForge Live's desktop: no main
        // window behind it, and DiskForge closes with it.
        const QString tool = parser.value(openOption);
        QTimer::singleShot(0, &window, [&window, tool] {
            if (!window.openTool(tool)) {
                QTextStream(stderr) << "No tool called " << tool << " (write-image, windows-usb, rescue-usb, check-stick, lost-files)\n";
                QCoreApplication::exit(2);
                return;
            }
            QCoreApplication::quit();
        });
    }

    if (parser.isSet(screenshotOption)) {
        const QString file = parser.value(screenshotOption);
        const bool menu = parser.isSet(menuOption);
        QTimer::singleShot(500, &app, [&window, file, menu] {
            if (menu) {
                QMenu popup(&window);
                window.buildContextMenu(&popup);
                popup.popup(QPoint(0, 0));
                QApplication::processEvents();
                popup.grab().save(file);
            } else {
                window.grab().save(file);
            }
            QApplication::quit();
        });
    }
    const int status = app.exec();
    qCInfo(lcOps).noquote() << "DiskForge closed";
    return status;
}
