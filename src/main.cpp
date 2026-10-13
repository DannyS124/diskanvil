// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "applog.h"
#include "format.h"
#include "homewindow.h"
#include "mainwindow.h"
#include "oldname.h"
#include "preview.h"
#include "translations.h"
#include "udisks.h"
#include "viewer.h"
#include "holders.h"
#include "opal.h"
#include "writeprotect.h"

#include <QApplication>
#include <cstdio>
#include <QGuiApplication>
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
    // Who's Using It? looking at other users' programs, as root.
    if (argc >= 3 && QByteArray(argv[1]) == holders::kOption) {
        QStringList devices;
        for (int i = 3; i < argc; ++i)
            devices << QString::fromLocal8Bit(argv[i]);
        std::printf("%s\n", holders::toLines(holders::find({QString::fromLocal8Bit(argv[2])}, devices)).toLocal8Bit().constData());
        return 0;
    }
    // Write-Protected Drive asking a drive whether it's write-protected, as root on DiskAnvil Live.
    if (argc == 3 && QByteArray(argv[1]) == writeprotect::kAskOption) {
        std::printf("%d\n", writeprotect::askDevice(QString::fromLocal8Bit(argv[2])));
        return 0;
    }
    // Self-Encrypting Drive asking a drive about its encryption, or resetting it with its PSID, as
    // root (DiskAnvil Live's sudo, or pkexec).
    if (argc == 4 && QByteArray(argv[1]) == opal::kOption)
        return opal::runOption(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    // Find Lost Files' preview helper (preview.h): no window, no screen, just answers.
    if (argc == 2 && QByteArray(argv[1]) == "--preview-helper") {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QGuiApplication helper(argc, argv);
        return preview::runHelper();
    }
    // These never open a window, so they shouldn't need a screen (over SSH, on a text console).
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if ((arg == "--dump" || arg == "--help" || arg == "-h" || arg == "--version" || arg == "-v")
            && qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
            qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    // DiskAnvil's own icons, next to the program (Windows has no icon theme to borrow from);
    // Theme picks the dark or the light ones.
    QIcon::setThemeSearchPaths(QIcon::themeSearchPaths() << QCoreApplication::applicationDirPath() + QStringLiteral("/icons"));
    QIcon::setThemeName(QStringLiteral("DiskAnvil-Light"));
#endif
    installTranslations();
    QApplication::setApplicationName(QStringLiteral("diskanvil"));
    QApplication::setApplicationDisplayName(QStringLiteral("DiskAnvil"));
    QApplication::setApplicationVersion(QStringLiteral(APP_VERSION));
    QApplication::setDesktopFileName(QStringLiteral(APP_ID));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral(APP_ID), QIcon(QStringLiteral(":/data/" APP_ID ".svg"))));
    // Find Lost Files' viewer (viewer.h): a program of its own, so a damaged file can't take
    // DiskAnvil down. Find Lost Files talks to it on its standard input and output.
    if (argc == 2 && QByteArray(argv[1]) == "--viewer")
        return viewer::run();
    // What DiskAnvil kept while it was called DiskForge.
    oldname::moveOver();

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
                                       QStringLiteral("Write what DiskAnvil does to <file>. DISKANVIL_LOG=<file> does the same."),
                                       QStringLiteral("file"));
    const QString tools = QStringLiteral("what-happened, write-image, windows-usb, rescue-usb, live-on-pc, check-stick, write-protected, why-crash, windows-start, still-ok, burn-in, write-speed, self-encrypting, lost-files, virus-scan, save-files, remote-help, net-boot, product-key, drivers, activity, look-closer, files, boot-menu, linux-boot, make-room, certificates or open-sets");
    const QCommandLineOption openOption(QStringLiteral("open"), QStringLiteral("Open just one tool: ") + tools + QLatin1Char('.'),
                                        QStringLiteral("tool"));
    const QCommandLineOption homeOption(QStringLiteral("home"), QStringLiteral("Be DiskAnvil Live's home screen (its desktop)."));
    parser.addOptions({dumpOption, screenshotOption, selectOption, menuOption, logOption, openOption, homeOption});
    parser.addPositionalArgument(QStringLiteral("images"), QStringLiteral("Disk images (.iso, .img) to open."), QStringLiteral("[image...]"));
    parser.process(app);

    const QString logFile = parser.isSet(logOption) ? parser.value(logOption) : qEnvironmentVariable("DISKANVIL_LOG");
    if (!logFile.isEmpty() && applog::start(logFile)) {
        qCInfo(lcOps).noquote() << "DiskAnvil" << APP_VERSION << "started, Qt" << qVersion() << "on"
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
        QMessageBox::critical(nullptr, QStringLiteral("DiskAnvil"), message);
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
    // On a smaller screen (a small laptop, a VM, DiskAnvil Live) that would put the bottom of
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
        // Just the tool, for launchers like the ones on DiskAnvil Live's desktop: no main
        // window behind it, and DiskAnvil closes with it.
        const QString tool = parser.value(openOption);
        QTimer::singleShot(0, &window, [&window, tool, tools] {
            if (!window.openTool(tool)) {
                QTextStream(stderr) << "No tool called " << tool << " (" << tools << ")\n";
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
    qCInfo(lcOps).noquote() << "DiskAnvil closed";
    return status;
}
