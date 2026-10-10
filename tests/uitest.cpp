// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Drives the real window, answering its dialogs the way a user would:
// - Check for Errors finds damage, the disk list refreshes while "Repair them now?" is
//   open, and the repair still has to reach the right partition.
// - The right-click menus of the system disk offer nothing that writes to it.
// - Clone Drive, with the disk list refreshed while its dialog is open.
// - Back Up a partition, damage it, Restore it, and it's back exactly.
// sudo QT_QPA_PLATFORM=offscreen build/diskforge-uitest

#include "../src/addonoutput.h"
#include "../src/addonform.h"
#include "../src/addonmaker.h"
#include "../src/diskmap.h"
#include "../src/homewindow.h"
#include "../src/blockio.h"
#include "../src/imagesource.h"
#include "../src/lostfilesdialog.h"
#include "../src/inspectdialog.h"
#include "../src/isomode.h"
#include "../src/filecopy.h"
#include "../src/recoverdialog.h"
#include "../src/rescuestick.h"
#include "../src/rescueusbdialog.h"
#include "../src/stickcheckdialog.h"
#include "../src/noticebar.h"
#include "../src/typedialog.h"
#include "../src/windowsusbjob.h"
#include "../src/windowsusbdialog.h"
#include "slowdisk.h"
#include "../src/addons.h"
#include "../src/addonsdialog.h"
#include "../src/copydialogs.h"
#include "../src/format.h"
#include "../src/mainwindow.h"
#include "../src/systemtools.h"
#include "../src/theme.h"
#include "../src/tools.h"
#include "../src/udisks.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QStorageInfo>
#include <QBuffer>
#include <QPainter>
#include <QListView>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QDirIterator>
#include <QFileDialog>
#include <QKeyEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QFile>
#include <QStyle>
#include <QFileInfo>
#include <QDir>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QToolBar>
#include <QInputDialog>
#include <QMouseEvent>
#include <QListWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QShortcut>
#include <QMenuBar>
#include <QPushButton>
#include <QSpinBox>
#include <QRadioButton>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QProcess>
#include <QRandomGenerator>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <memory>

#include <fcntl.h>
#include <unistd.h>

namespace {

QTextStream out(stdout);
int failures = 0;

void report(bool ok, const QString &step, const QString &detail = {})
{
    out << (ok ? "PASS  " : "FAIL  ") << step;
    if (!detail.isEmpty())
        out << "  (" << detail << ")";
    out << Qt::endl;
    if (!ok)
        ++failures;
}

int sh(const QString &program, const QStringList &args, QString *output = nullptr)
{
    QProcess p;
    p.start(program, args);
    p.waitForFinished(60000);
    if (output)
        *output = QString::fromLocal8Bit(p.readAllStandardOutput() + p.readAllStandardError()).trimmed();
    return p.exitCode();
}

void waitUntil(const std::function<bool()> &done, int ms)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        QThread::msleep(20);
    }
}


QAction *findAction(MainWindow &window, const QString &startsWith)
{
    for (QAction *a : window.findChildren<QAction *>()) {
        // Pinned add-on actions carry their add-on id and label; they're never DiskForge's own.
        if (a->data().toStringList().size() == 2)
            continue;
        if (a->text().remove(QLatin1Char('&')).startsWith(startsWith))
            return a;
    }
    return nullptr;
}

QPushButton *findButton(QWidget *dialog, const QString &text)
{
    for (QPushButton *b : dialog->findChildren<QPushButton *>()) {
        if (b->text().remove(QLatin1Char('&')) == text)
            return b;
    }
    return nullptr;
}

// Every 100 ms, hands whatever modal dialog is open to `answer`. Message boxes nobody
// expects are recorded and closed.
struct Answerer {
    QTimer timer;
    QStringList boxes;
    std::function<bool(QWidget *)> answer;
    Answerer()
    {
        QObject::connect(&timer, &QTimer::timeout, [this] {
            QWidget *modal = QApplication::activeModalWidget();
            if (!modal || (answer && answer(modal)))
                return;
            if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                boxes << box->text();
                box->close();
            }
        });
        timer.start(100);
    }
};

QString partitionHash(const QString &device)
{
    QFile f(device);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&f);
    return QString::fromLatin1(hash.result().toHex());
}

QString layout(const QString &device)
{
    QString dump;
    sh(QStringLiteral("sfdisk"), {QStringLiteral("--dump"), device}, &dump);
    QStringList lines;
    for (const QString &line : dump.split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("/dev/")))
            lines << line.section(QLatin1Char(':'), 1);
    }
    return lines.join(QLatin1Char('\n'));
}

QString attach(const QString &image, const QString &script, qint64 megabytes)
{
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("%1M").arg(megabytes), image});
    if (!script.isEmpty()) {
        QProcess sfdisk;
        sfdisk.start(QStringLiteral("sfdisk"), {QStringLiteral("-q"), image});
        sfdisk.waitForStarted();
        sfdisk.write(script.toLatin1());
        sfdisk.closeWriteChannel();
        sfdisk.waitForFinished();
    }
    QString loop;
    sh(QStringLiteral("losetup"), {QStringLiteral("-fP"), QStringLiteral("--show"), image}, &loop);
    QThread::msleep(500);
    return loop;
}

// Nothing in the system disk's right-click menus may write to it.
void systemMenus(MainWindow &window, UDisks &udisks)
{
    static const QStringList writes = {QStringLiteral("Format"), QStringLiteral("Resize"), QStringLiteral("Delete Partition"),
                                       QStringLiteral("Change Label"), QStringLiteral("Mount at Startup"), QStringLiteral("Change Passphrase"),
                                       QStringLiteral("New Partition Table"), QStringLiteral("Wipe Disk"), QStringLiteral("Secure Erase"),
                                       QStringLiteral("Restore"), QStringLiteral("Clone Drive"), QStringLiteral("Rescue Copy"),
                                       QStringLiteral("Unmount"), QStringLiteral("Safely Remove")};
    QStringList devices;
    for (const Disk &d : udisks.disks()) {
        if (!d.isSystem)
            continue;
        devices << d.device;
        for (const Volume &v : d.volumes)
            devices << v.device;
    }
    report(!devices.isEmpty(), QStringLiteral("find the system disk"));
    QStringList offered;
    for (const QString &device : devices) {
        if (!window.selectDevice(device))
            continue;
        QMenu menu;
        window.buildContextMenu(&menu);
        for (QAction *a : menu.actions()) {
            const QString text = a->text().remove(QLatin1Char('&'));
            for (const QString &w : writes) {
                if (!a->isSeparator() && text.startsWith(w))
                    offered << QStringLiteral("%1: %2").arg(shortDevice(device), text);
            }
        }
    }
    report(offered.isEmpty(), QStringLiteral("the system disk's menus offer nothing that writes to it"), offered.join(QStringLiteral(", ")));
}

void cloneThroughWindow(MainWindow &window, UDisks &udisks, const QTemporaryDir &dir)
{
    const QString source = attach(dir.filePath(QStringLiteral("clone-source.img")), QStringLiteral("label: gpt\nsize=48MiB, type=L\nsize=24MiB, type=L\n"), 96);
    const QString target = attach(dir.filePath(QStringLiteral("clone-target.img")), QString(), 160);
    sh(QStringLiteral("mkfs.ext4"), {QStringLiteral("-q"), QStringLiteral("-L"), QStringLiteral("CLONEME"), source + QStringLiteral("p1")});
    sh(QStringLiteral("mkfs.vfat"), {QStringLiteral("-n"), QStringLiteral("CLONEFAT"), source + QStringLiteral("p2")});
    QString targetPath;
    waitUntil([&] {
        udisks.refresh();
        for (const Disk &d : udisks.disks()) {
            if (d.device == target)
                targetPath = d.blockPath;
        }
        return !targetPath.isEmpty() && window.selectDevice(source);
    }, 15000);
    report(window.selectDevice(source), QStringLiteral("select the drive to clone"), source);

    Answerer answerer;
    bool started = false, refreshed = false;
    answerer.answer = [&](QWidget *modal) {
        auto *dialog = qobject_cast<CloneDialog *>(modal);
        if (!dialog || started)
            return dialog != nullptr;
        auto *targets = dialog->findChild<QComboBox *>();
        const int index = targets ? targets->findData(targetPath) : -1;
        if (index < 0)
            return true; // not listed yet; try again next tick
        targets->setCurrentIndex(index);
        for (QLineEdit *e : dialog->findChildren<QLineEdit *>()) {
            if (e->placeholderText() == shortDevice(target))
                e->setText(shortDevice(target));
        }
        // Keep the layout identical so it can be compared (growing is tested in --clone).
        for (QCheckBox *box : dialog->findChildren<QCheckBox *>()) {
            if (box->text().startsWith(QLatin1String("Let the last partition")))
                box->setChecked(false);
        }
        // The list gets replaced while the dialog is open; the dialog has to cope.
        for (int i = 0; i < 3; ++i)
            udisks.refresh();
        refreshed = true;
        if (QPushButton *clone = findButton(dialog, QStringLiteral("Clone")); clone && clone->isEnabled()) {
            started = true;
            clone->click();
        }
        return true;
    };
    if (QAction *clone = findAction(window, QStringLiteral("Clone Drive")))
        clone->trigger(); // returns once the dialog has closed
    else
        report(false, QStringLiteral("Clone Drive is in the menus"));
    report(started && refreshed, QStringLiteral("start a clone after the disk list was replaced"));
    const bool copied = std::any_of(answerer.boxes.cbegin(), answerer.boxes.cend(), [](const QString &b) { return b.startsWith(QLatin1String("Copied")); });
    report(copied, QStringLiteral("the clone finished"), answerer.boxes.join(QStringLiteral(" | ")));
    sh(QStringLiteral("partprobe"), {target});
    const QString was = layout(source), now = layout(target);
    report(!was.isEmpty() && now == was && partitionHash(target + QStringLiteral("p1")) == partitionHash(source + QStringLiteral("p1")),
           QStringLiteral("the copy has the same partitions and data"), now == was ? QString() : was + QStringLiteral(" vs ") + now);
    sh(QStringLiteral("losetup"), {QStringLiteral("-d"), source});
    sh(QStringLiteral("losetup"), {QStringLiteral("-d"), target});
}

void backupAndRestore(MainWindow &window, UDisks &udisks, const QTemporaryDir &dir)
{
    const QString loop = attach(dir.filePath(QStringLiteral("backup-me.img")), QStringLiteral("label: gpt\nsize=64MiB, type=L\n"), 96);
    const QString part = loop + QStringLiteral("p1");
    sh(QStringLiteral("mkfs.ext4"), {QStringLiteral("-q"), QStringLiteral("-L"), QStringLiteral("KEEPME"), part});
    const QString before = partitionHash(part);
    waitUntil([&] {
        udisks.refresh();
        return window.selectDevice(part);
    }, 15000);
    const QString file = dir.filePath(QStringLiteral("backups/keepme.img.zst"));

    {
        Answerer answerer;
        bool started = false;
        answerer.answer = [&](QWidget *modal) {
            auto *dialog = qobject_cast<BackupDialog *>(modal);
            if (!dialog || started)
                return dialog != nullptr;
            if (auto *edit = dialog->findChild<QLineEdit *>())
                edit->setText(file);
            if (QPushButton *go = findButton(dialog, QStringLiteral("Back Up")); go && go->isEnabled()) {
                started = true;
                go->click();
            }
            return true;
        };
        if (QAction *backup = findAction(window, QStringLiteral("Back Up Partition")))
            backup->trigger();
        report(started && QFile::exists(file) && QFile::exists(file + QStringLiteral(".json")), QStringLiteral("back up a partition through the window"),
               answerer.boxes.join(QStringLiteral(" | ")));
    }

    // Damage it, then put the backup back.
    sh(QStringLiteral("mkfs.ext4"), {QStringLiteral("-q"), QStringLiteral("-F"), QStringLiteral("-L"), QStringLiteral("OOPS"), part});
    report(partitionHash(part) != before, QStringLiteral("the partition was changed"));
    waitUntil([&] {
        udisks.refresh();
        return window.selectDevice(part);
    }, 15000);
    {
        Answerer answerer;
        bool started = false;
        answerer.answer = [&](QWidget *modal) {
            auto *dialog = qobject_cast<RestoreDialog *>(modal);
            if (!dialog || started)
                return dialog != nullptr;
            for (QLineEdit *e : dialog->findChildren<QLineEdit *>()) {
                if (e->placeholderText() == shortDevice(part))
                    e->setText(shortDevice(part));
                else
                    e->setText(file);
            }
            for (int i = 0; i < 3; ++i)
                udisks.refresh();
            if (QPushButton *go = findButton(dialog, QStringLiteral("Restore")); go && go->isEnabled()) {
                started = true;
                go->click();
            }
            return true;
        };
        if (QAction *restore = findAction(window, QStringLiteral("Restore Partition Backup")))
            restore->trigger();
        report(started, QStringLiteral("restore it through the window"), answerer.boxes.join(QStringLiteral(" | ")));
    }
    report(partitionHash(part) == before, QStringLiteral("the partition is exactly as it was"));
    sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
}

// The add-on output window: real output and exit code, Stop ends everything the command
// started, and the command gets no open files besides its output.
void outputWindow()
{
    auto run = [](const QString &script) {
        auto *window = new AddonOutputWindow(QStringLiteral("test"), {QStringLiteral("sh"), QStringLiteral("-c"), script});
        window->setAttribute(Qt::WA_DeleteOnClose, false);
        window->show();
        return window;
    };
    {
        std::unique_ptr<AddonOutputWindow> w(run(QStringLiteral("printf 'one\\ntwo\\n'; ls /proc/$$/fd | tr '\\n' ' '; exit 3")));
        waitUntil([&] { return !w->isRunning(); }, 5000);
        QApplication::processEvents();
        QThread::msleep(150);
        QApplication::processEvents();
        const QString shown = w->text();
        report(shown.startsWith(QLatin1String("one\ntwo\n")) && w->findChild<QLabel *>()->text().contains(QLatin1String("3")),
               QStringLiteral("the output window shows what the command printed, and its exit code"), shown.simplified());
        report(shown.contains(QLatin1String("0 1 2")) && !shown.contains(QLatin1String("0 1 2 3")),
               QStringLiteral("the command only gets its own input and output"), shown.section(QLatin1Char('\n'), 2).simplified());
    }
    {
        // A background child too: Stop has to end the whole group.
        std::unique_ptr<AddonOutputWindow> w(run(QStringLiteral("sleep 60 & echo started; sleep 60")));
        waitUntil([&] {
            QThread::msleep(150);
            QApplication::processEvents();
            return w->text().contains(QLatin1String("started"));
        }, 5000);
        QProcess pids;
        QElapsedTimer timer;
        timer.start();
        w->stop();
        waitUntil([&] { return !w->isRunning(); }, 6000);
        QThread::msleep(300);
        pids.start(QStringLiteral("pgrep"), {QStringLiteral("-f"), QStringLiteral("^sleep 60$")});
        pids.waitForFinished();
        report(!w->isRunning() && timer.elapsed() < 5000 && pids.readAllStandardOutput().trimmed().isEmpty(),
               QStringLiteral("Stop ends the command and everything it started"), QString::number(timer.elapsed()) + QStringLiteral(" ms"));
    }
}

// Add-ons in the Tools menu, the right-click menu, the toolbar and on shortcuts, on the
// system disk (which every PC has, so this runs as the user).
void addonMenus()
{
    const QByteArray manifest = R"({"id":"uitest","name":"UI Test","settings":[{"id":"dest","type":"folder","label":"Folder","default":"{home}/Backups"}],"actions":[
        {"label":"Look Around","applies_to":"any","look_only":true,"system_disks":true,"command":["true"]},
        {"label":"Only USB","applies_to":"any","when":["removable"],"command":["true"]},
        {"label":"Format Everything","applies_to":"any","command":["true"]}]})";
    QString error;
    report(Addons::install(manifest, &error), QStringLiteral("a test add-on installs"), error);

    UDisks udisks;
    udisks.setInteractive(false);
    MainWindow window(&udisks);
    window.show();
    QString systemPart;
    for (const Disk &d : udisks.disks()) {
        if (d.isSystem && !d.volumes.isEmpty())
            systemPart = d.volumes.last().device;
    }
    if (systemPart.isEmpty() || !window.selectDevice(systemPart)) {
        out << "SKIP  no system disk to try the add-on menus on" << Qt::endl;
        return;
    }
    auto text = [](const QAction *a) { return a->text().remove(QLatin1Char('&')).section(QLatin1Char('\t'), 0, 0); };

    QMenu *tools = nullptr;
    for (QAction *a : window.menuBar()->actions()) {
        if (a->text().remove(QLatin1Char('&')) == QLatin1String("Tools"))
            tools = a->menu();
    }
    Q_EMIT tools->aboutToShow();
    QMap<QString, QAction *> listed;
    for (QAction *a : tools->actions())
        listed.insert(text(a), a);
    report(listed.value(QStringLiteral("Look Around")) && listed.value(QStringLiteral("Look Around"))->isEnabled()
               && listed.value(QStringLiteral("Format Everything")) && !listed.value(QStringLiteral("Format Everything"))->isEnabled()
               && listed.value(QStringLiteral("Format Everything"))->toolTip().contains(QLatin1String("system disk")),
           QStringLiteral("Tools lists every add-on action; one that doesn't fit is greyed out and says why"),
           listed.value(QStringLiteral("Format Everything")) ? listed.value(QStringLiteral("Format Everything"))->toolTip() : QStringLiteral("missing"));

    QMenu context;
    window.buildContextMenu(&context);
    QStringList offered;
    for (QAction *a : context.actions())
        offered << text(a);
    report(offered.contains(QLatin1String("Look Around")) && !offered.contains(QLatin1String("Format Everything"))
               && !offered.contains(QLatin1String("Only USB")),
           QStringLiteral("the system disk's right-click menu only offers the look-only add-on"), offered.join(QStringLiteral(", ")));

    Addons addons;
    addons.load();
    auto keyOf = [&addons](const QString &label) {
        for (const Addon &a : addons.all()) {
            for (const AddonAction &act : a.actions) {
                if (act.label == label)
                    return Addons::actionKey(a, act);
            }
        }
        return QString();
    };
    Addons::setPinned(keyOf(QStringLiteral("Look Around")), true);
    Addons::setPinned(keyOf(QStringLiteral("Format Everything")), true);
    window.refreshAddons();
    auto *toolbar = window.findChild<QToolBar *>(QStringLiteral("mainToolbar"));
    QAction *lookPin = nullptr, *formatPin = nullptr;
    for (QAction *a : toolbar->actions()) {
        if (text(a) == QLatin1String("Look Around"))
            lookPin = a;
        if (text(a) == QLatin1String("Format Everything"))
            formatPin = a;
    }
    report(lookPin && lookPin->isEnabled() && formatPin && !formatPin->isEnabled() && formatPin->toolTip().contains(QLatin1String("system disk")),
           QStringLiteral("pinned add-on actions go on the toolbar and follow the selection"));

    Addons::setShortcut(keyOf(QStringLiteral("Format Everything")), QStringLiteral("Ctrl+Alt+J"));
    Addons::setShortcut(keyOf(QStringLiteral("Look Around")), QStringLiteral("Ctrl+Alt+K"));
    window.refreshAddons();
    auto shortcutFor = [&window](const QString &keys) -> QShortcut * {
        for (QShortcut *s : window.findChildren<QShortcut *>()) {
            if (s->key() == QKeySequence(keys))
                return s;
        }
        return nullptr;
    };
    QShortcut *formatKey = shortcutFor(QStringLiteral("Ctrl+Alt+J"));
    if (formatKey)
        Q_EMIT formatKey->activated();
    const QString message = window.statusBar()->currentMessage();
    report(formatKey && message.contains(QLatin1String("Format Everything")) && message.contains(QLatin1String("system disk")),
           QStringLiteral("a shortcut for an action that doesn't fit says why in the status bar"), message);
    QString asked;
    Answerer answerer;
    answerer.answer = [&asked](QWidget *modal) {
        asked = modal->windowTitle();
        modal->close();
        return true;
    };
    if (QShortcut *lookKey = shortcutFor(QStringLiteral("Ctrl+Alt+K")))
        Q_EMIT lookKey->activated();
    waitUntil([&] { return !asked.isEmpty(); }, 3000);
    report(asked == QLatin1String("Run Add-on?"), QStringLiteral("a shortcut runs the add-on action (it asks first)"), asked);

    // The Add-ons window: shortcuts that clash or have no modifier are refused, settings save.
    addons.load();
    // Spelled out: the offscreen platform the tests use has no key for QKeySequence::Quit.
    AddonsDialog dialog(&addons, {QKeySequence(QStringLiteral("Ctrl+Q"))});
    dialog.show();
    QStringList warnings;
    answerer.answer = [&warnings](QWidget *modal) {
        if (auto *form = qobject_cast<AddonFormDialog *>(modal)) {
            form->findChild<QLineEdit *>()->setText(QStringLiteral("/srv/test-backups"));
            form->accept();
            return true;
        }
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            warnings << box->text();
            box->close();
            return true;
        }
        return false;
    };
    const auto edits = dialog.findChildren<QKeySequenceEdit *>();
    if (!edits.isEmpty()) {
        edits.first()->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Q")));
        Q_EMIT edits.first()->editingFinished();
        edits.first()->setKeySequence(QKeySequence(Qt::Key_J));
        Q_EMIT edits.first()->editingFinished();
    }
    report(warnings.size() == 2 && warnings.value(0).contains(QLatin1String("DiskForge's own")) && warnings.value(1).contains(QLatin1String("Ctrl"))
               && Addons::shortcut(keyOf(QStringLiteral("Look Around"))) == QLatin1String("Ctrl+Alt+K"),
           QStringLiteral("the Add-ons window refuses a shortcut DiskForge uses, or one without Ctrl/Alt"), warnings.join(QStringLiteral(" / ")));
    for (QPushButton *button : dialog.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("Settings…"))
            button->click();
    }
    QApplication::processEvents();
    report(Addons::setting(addons.all().value(0), QStringLiteral("dest")) == QLatin1String("/srv/test-backups"),
           QStringLiteral("an add-on's settings save from the Add-ons window"), Addons::setting(addons.all().value(0), QStringLiteral("dest")));
}

// Themes: built-ins switch and switch back, broken colors get fixed, and a theme that
// turned up from outside isn't used.
void themes()
{
    Addons addons;
    addons.load();
    Theme &theme = Theme::instance();
    const QString desktopStyle = QApplication::style()->name();
    const QColor desktopWindow = QApplication::palette().color(QPalette::Window);
    theme.select(QStringLiteral("deadshadow"), addons);
    report(theme.currentId() == QLatin1String("deadshadow") && QApplication::palette().color(QPalette::Window) == QColor(0x0a, 0x0a, 0x0f)
               && QApplication::style()->name().compare(QLatin1String("fusion"), Qt::CaseInsensitive) == 0
               && theme.partitionColor(QStringLiteral("ext4")) == QColor(0x00, 0xe6, 0x76),
           QStringLiteral("a built-in theme sets the window colors and its own map colors"), QApplication::style()->name());
    theme.select(QStringLiteral("system"), addons);
    report(QApplication::palette().color(QPalette::Window) == desktopWindow && QApplication::style()->name() == desktopStyle,
           QStringLiteral("going back to System restores the desktop's look"), QApplication::style()->name());

    AddonTheme sneaky;
    sneaky.colors.insert(QStringLiteral("danger"), 0x00ff00);  // green "danger"
    sneaky.colors.insert(QStringLiteral("warning"), 0xfafa00); // yellow on a light window: unreadable
    sneaky.palette.insert(QStringLiteral("window"), 0x000000);
    sneaky.palette.insert(QStringLiteral("text"), 0x111111);   // black on black
    QStringList replaced;
    const AddonTheme fixed = Theme::checked(sneaky, &replaced);
    report(replaced.contains(QLatin1String("danger")) && replaced.contains(QLatin1String("palette")) && fixed.palette.isEmpty()
               && QColor(QRgb(fixed.colors.value(QStringLiteral("danger")))).hsvHue() <= 20,
           QStringLiteral("a theme can't make danger green or its text unreadable"), replaced.join(QLatin1Char(' ')));

    const QByteArray mine = R"({"id":"my-theme","name":"Mine","theme":{"colors":{"partition":"#123456"}}})";
    QString error;
    Addons::install(mine, &error);
    const QString planted = Addons::userDir() + QStringLiteral("/planted-theme/addon.json");
    QDir().mkpath(QFileInfo(planted).path());
    QFile f(planted);
    if (f.open(QIODevice::WriteOnly))
        f.write(R"({"id":"planted-theme","name":"Planted","theme":{"colors":{"partition":"#654321"}}})");
    f.close();
    addons.load();
    bool plantedUsable = true, mineListed = false;
    for (const Theme::Choice &c : Theme::choices(addons)) {
        if (c.id == QLatin1String("planted-theme"))
            plantedUsable = c.usable;
        mineListed = mineListed || (c.id == QLatin1String("my-theme") && c.usable);
    }
    theme.use(QStringLiteral("planted-theme"), addons);
    report(mineListed && !plantedUsable && theme.currentId() == QLatin1String("system"),
           QStringLiteral("a theme that turned up from outside is listed but not used"), theme.currentId());
    theme.select(QStringLiteral("my-theme"), addons);
    report(theme.color(Theme::Role::Partition) == QColor(0x12, 0x34, 0x56), QStringLiteral("an installed theme add-on is used"));
    theme.select(QStringLiteral("system"), addons);
}

// The Add-on Maker: what it saves loads and isn't flagged, replacing asks first, editing an
// add-on that turned up from outside keeps the flag, and Test hands over the action.
void maker()
{
    Addons addons;
    addons.load();
    auto clickButton = [](QWidget *window, const QString &text) {
        for (QPushButton *b : window->findChildren<QPushButton *>()) {
            if (b->text() == text) {
                b->click();
                return true;
            }
        }
        return false;
    };
    auto find = [&addons](const QString &id) {
        for (const Addon &a : addons.all()) {
            if (a.id == id)
                return a;
        }
        return Addon();
    };
    auto fillIn = [](AddonMaker &m, const QString &name, const QString &label, const QString &program) {
        m.findChild<QLineEdit *>(QStringLiteral("name"))->setText(name);
        m.findChild<QLineEdit *>(QStringLiteral("label"))->setText(label);
        auto *command = m.findChild<QTableWidget *>(QStringLiteral("command"));
        command->item(0, 0)->setText(program);
    };
    {
        AddonMaker m(&addons, nullptr, {});
        fillIn(m, QStringLiteral("Maker Test"), QStringLiteral("Say Hi"), QStringLiteral("echo"));
        clickButton(&m, QStringLiteral("Save"));
    }
    addons.load();
    const Addon made = find(QStringLiteral("maker-test"));
    report(made.error.isEmpty() && !made.outside && made.actions.value(0).label == QLatin1String("Say Hi")
               && made.actions.value(0).command == QStringList{QStringLiteral("echo")},
           QStringLiteral("the Add-on Maker saves an add-on that loads and isn't flagged"), made.error);

    QStringList asked;
    Answerer answerer;
    answerer.answer = [&asked](QWidget *modal) {
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            asked << box->text();
            if (QAbstractButton *no = box->button(QMessageBox::No))
                no->click();
            else
                box->close();
            return true;
        }
        return false;
    };
    {
        AddonMaker m(&addons, nullptr, {});
        fillIn(m, QStringLiteral("Maker Test"), QStringLiteral("Something Else"), QStringLiteral("true"));
        clickButton(&m, QStringLiteral("Save"));
    }
    addons.load();
    report(asked.value(0).contains(QLatin1String("already")) && find(QStringLiteral("maker-test")).actions.value(0).label == QLatin1String("Say Hi"),
           QStringLiteral("saving over an existing add-on asks first (and No keeps it)"), asked.join(QStringLiteral(" / ")));

    const QString planted = Addons::userDir() + QStringLiteral("/planted-maker/addon.json");
    QDir().mkpath(QFileInfo(planted).path());
    QFile f(planted);
    if (f.open(QIODevice::WriteOnly))
        f.write(R"({"id":"planted-maker","name":"Planted","actions":[{"label":"Hi","command":["echo","hi"]}]})");
    f.close();
    addons.load();
    {
        const Addon outside = find(QStringLiteral("planted-maker"));
        asked.clear();
        AddonMaker m(&addons, &outside, {});
        m.findChild<QLineEdit *>(QStringLiteral("label"))->setText(QStringLiteral("Hello"));
        clickButton(&m, QStringLiteral("Save"));
    }
    addons.load();
    report(find(QStringLiteral("planted-maker")).outside && find(QStringLiteral("planted-maker")).actions.value(0).label == QLatin1String("Hello"),
           QStringLiteral("editing an add-on that turned up from outside keeps the flag"), asked.join(QStringLiteral(" / ")));

    QString tested;
    {
        AddonMaker m(&addons, nullptr, [&tested](const Addon &, const AddonAction &action) { tested = action.label; });
        fillIn(m, QStringLiteral("Try Me"), QStringLiteral("Try This"), QStringLiteral("true"));
        clickButton(&m, QStringLiteral("Test"));
    }
    report(tested == QLatin1String("Try This"), QStringLiteral("Test hands the unsaved action over to be run"), tested);
}

// The warning banner, with health faked through the test hook: a bar for a failing drive,
// Details opens the Health window, Dismiss hides it, and it comes back when it gets worse.
void banner()
{
    UDisks udisks;
    udisks.setInteractive(false);
    if (udisks.disks().isEmpty()) {
        report(true, QStringLiteral("the warning banner (skipped: no drives)"));
        return;
    }
    for (const Disk &d : udisks.disks()) {
        Health healthy;
        healthy.state = Health::State::Healthy;
        healthy.summary = QStringLiteral("Healthy");
        healthy.key = QStringLiteral("test-healthy");
        udisks.setHealthForTest(d.blockPath, healthy);
    }
    udisks.refresh();
    MainWindow window(&udisks);
    window.show();
    auto *notices = window.findChild<QWidget *>(QStringLiteral("notices"));
    auto bars = [notices] {
        QList<NoticeBar *> out;
        for (NoticeBar *b : notices->findChildren<NoticeBar *>()) {
            if (b->isVisibleTo(notices))
                out << b;
        }
        return out;
    };
    report(notices && !notices->isVisible(), QStringLiteral("no banner while every drive is healthy"));
    if (!notices)
        return;

    const Disk target = udisks.disks().constFirst();
    Health failing;
    failing.state = Health::State::Failing;
    failing.summary = QStringLiteral("Failing, back up now");
    failing.key = QStringLiteral("test-failing");
    failing.reasons = {{HealthReason::Level::Failing, QStringLiteral("drive-failing"), -1, QStringLiteral("The drive itself says it's failing.")}};
    udisks.setHealthForTest(target.blockPath, failing);
    udisks.refresh();
    QList<NoticeBar *> shown = bars();
    QStringList buttons;
    for (QPushButton *b : shown.isEmpty() ? QList<QPushButton *>() : shown[0]->findChildren<QPushButton *>())
        buttons << b->text();
    const QStringList expected = target.isSystem ? QStringList{QStringLiteral("Details…"), QStringLiteral("Dismiss")}
                                                 : QStringList{QStringLiteral("Rescue Copy…"), QStringLiteral("Details…"), QStringLiteral("Dismiss")};
    report(notices->isVisible() && shown.size() == 1 && shown[0]->text().contains(QLatin1String("is failing")) && buttons == expected,
           QStringLiteral("a failing drive gets a bar on top, with what to do"),
           (shown.isEmpty() ? QString() : shown[0]->text()) + QStringLiteral(" [") + buttons.join(QStringLiteral(", ")) + QLatin1Char(']'));

    QString opened;
    Answerer answerer;
    answerer.answer = [&opened](QWidget *modal) {
        if (modal->inherits("HealthDialog")) {
            opened = modal->windowTitle();
            modal->close();
            return true;
        }
        return false;
    };
    auto click = [&shown](const QString &text) {
        for (QPushButton *b : shown.value(0) ? shown[0]->findChildren<QPushButton *>() : QList<QPushButton *>()) {
            if (b->text() == text)
                b->click();
        }
    };
    click(QStringLiteral("Details…"));
    waitUntil([&opened] { return !opened.isEmpty(); }, 5000);
    report(!opened.isEmpty(), QStringLiteral("Details… opens the Health window"), opened);

    click(QStringLiteral("Dismiss"));
    waitUntil([notices] { return !notices->isVisible(); }, 3000);
    udisks.refresh();
    report(!notices->isVisible(), QStringLiteral("Dismiss hides it, and it stays hidden"));

    failing.reasons << HealthReason{HealthReason::Level::Warning, QStringLiteral("pending"), 3, QStringLiteral("3 unreadable sectors.")};
    udisks.setHealthForTest(target.blockPath, failing);
    udisks.refresh();
    report(notices->isVisible() && bars().size() == 1, QStringLiteral("it comes back when it gets worse"));
}

// Stop from the bar on top: a real wipe on a slow test disk, stopped with the bar's own
// button. The question is answered "Stop Wiping"; then the bar and the toolbar's Stop go,
// and the status bar says where it stopped (no error box).
void stopWipe()
{
    QTemporaryDir dir;
    SlowDisk slow;
    if (!slow.create(dir.path(), 128, 40)) {
        report(false, QStringLiteral("make a slow test disk"), slow.error);
        slow.remove();
        return;
    }
    UDisks udisks;
    udisks.setInteractive(false);
    MainWindow window(&udisks);
    window.show();
    QString blockPath;
    waitUntil([&] {
        udisks.refresh();
        for (const Disk &d : udisks.disks()) {
            if (d.device == slow.loop)
                blockPath = d.blockPath;
        }
        return !blockPath.isEmpty();
    }, 15000);
    if (blockPath.isEmpty()) {
        report(false, QStringLiteral("the slow test disk shows up"), slow.loop);
        slow.remove();
        return;
    }
    udisks.wipe(*udisks.diskByPath(blockPath));
    QAction *stop = findAction(window, QStringLiteral("Stop"));
    NoticeBar *bar = nullptr;
    QPushButton *stopWiping = nullptr;
    waitUntil([&] {
        for (NoticeBar *b : window.findChildren<NoticeBar *>(QStringLiteral("job"))) {
            for (QPushButton *button : b->findChildren<QPushButton *>()) {
                if (!b->isHidden() && button->text() == QLatin1String("Stop Wiping")) {
                    bar = b;
                    stopWiping = button;
                }
            }
        }
        return bar != nullptr;
    }, 15000);
    report(bar && stop && stop->isEnabled(), QStringLiteral("a running wipe gets a bar on top with Stop, and the toolbar's Stop is on"),
           bar ? bar->text() : QString());
    if (!stopWiping) {
        slow.remove();
        return;
    }
    QStringList asked;
    Answerer answerer;
    answerer.answer = [&asked](QWidget *modal) {
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            for (QAbstractButton *b : box->buttons()) {
                if (b->text() == QLatin1String("Stop Wiping")) {
                    asked << box->text();
                    b->click();
                    return true;
                }
            }
        }
        return false;
    };
    stopWiping->click();
    auto noBars = [&window] {
        for (NoticeBar *b : window.findChildren<NoticeBar *>(QStringLiteral("job"))) {
            if (!b->isHidden())
                return false;
        }
        return true;
    };
    waitUntil([&] { return noBars() && !stop->isEnabled(); }, 20000);
    const QString status = window.statusBar()->currentMessage();
    report(asked.size() == 1 && asked[0].startsWith(QLatin1String("Stop wiping")) && noBars() && !stop->isEnabled()
               && status.startsWith(QLatin1String("Stopped wiping")) && answerer.boxes.isEmpty(),
           QStringLiteral("Stop asks first, then the wipe stops: no error box, and the bar and Stop go away"),
           status + QStringLiteral(" | ") + answerer.boxes.join(QStringLiteral(" / ")));
    slow.remove();
}

// Type and Flags through the window: pick Linux /home, tick Don't mount automatically,
// press Change, and the partition has both.
void typeAndFlags()
{
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("types.img"));
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("64M"), image});
    QProcess sfdisk;
    sfdisk.start(QStringLiteral("sfdisk"), {QStringLiteral("-q"), image});
    sfdisk.waitForStarted();
    sfdisk.write("label: gpt\n,,L\n");
    sfdisk.closeWriteChannel();
    sfdisk.waitForFinished();
    QString loop;
    sh(QStringLiteral("losetup"), {QStringLiteral("-fP"), QStringLiteral("--show"), image}, &loop);
    loop = loop.trimmed();
    const QString part = loop + QStringLiteral("p1");

    UDisks udisks;
    udisks.setInteractive(false);
    MainWindow window(&udisks);
    window.show();
    waitUntil([&] {
        udisks.refresh();
        return window.selectDevice(part);
    }, 15000);
    QAction *action = findAction(window, QStringLiteral("Partition Type and Flags"));
    report(action && action->isEnabled(), QStringLiteral("Type and Flags is offered for a partition"));
    bool answered = false;
    Answerer answerer;
    answerer.answer = [&answered](QWidget *modal) {
        auto *dialog = qobject_cast<PartitionTypeDialog *>(modal);
        if (!dialog)
            return false;
        auto *types = dialog->findChild<QComboBox *>();
        types->setCurrentIndex(types->findData(QStringLiteral("933ac7e1-2eb4-4f13-b844-0e14e2aef915")));
        for (QCheckBox *check : dialog->findChildren<QCheckBox *>()) {
            if (check->text() == QLatin1String("Don't mount automatically"))
                check->setChecked(true);
        }
        if (QPushButton *change = findButton(dialog, QStringLiteral("Change"))) {
            answered = change->isEnabled();
            change->click();
        }
        return true;
    };
    if (action)
        action->trigger();
    const Volume *v = nullptr;
    waitUntil([&] {
        udisks.refresh();
        for (const Disk &d : udisks.disks()) {
            for (const Volume &x : d.volumes) {
                if (x.device == part)
                    v = &x;
            }
        }
        return v && v->partType == QLatin1String("933ac7e1-2eb4-4f13-b844-0e14e2aef915") && v->partFlags == (quint64(1) << 63);
    }, 20000);
    report(answered && v && v->partType == QLatin1String("933ac7e1-2eb4-4f13-b844-0e14e2aef915") && v->partFlags == (quint64(1) << 63),
           QStringLiteral("the dialog's type and flag end up on the partition"), v ? v->partType + QStringLiteral(" flags ") + QString::number(v->partFlags) : QString());
    sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
}

// The lock on an encrypted partition: clicking it asks for the passphrase and unlocks,
// clicking it again locks. A LUKS partition on a loop device.
void lockInMap()
{
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("luks.img"));
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("64M"), image});
    QProcess sfdisk;
    sfdisk.start(QStringLiteral("sfdisk"), {QStringLiteral("-q"), image});
    sfdisk.waitForStarted();
    sfdisk.write("label: gpt\n,,L\n");
    sfdisk.closeWriteChannel();
    sfdisk.waitForFinished();
    QString loop;
    sh(QStringLiteral("losetup"), {QStringLiteral("-fP"), QStringLiteral("--show"), image}, &loop);
    loop = loop.trimmed();
    const QString part = loop + QStringLiteral("p1");
    QThread::msleep(500);
    QProcess luks;
    luks.start(QStringLiteral("cryptsetup"), {QStringLiteral("luksFormat"), QStringLiteral("--batch-mode"), QStringLiteral("--type"), QStringLiteral("luks2"),
                                              QStringLiteral("--pbkdf"), QStringLiteral("pbkdf2"), QStringLiteral("--pbkdf-force-iterations"), QStringLiteral("1000"),
                                              QStringLiteral("--key-file=-"), part});
    luks.waitForStarted();
    luks.write("diskforge-test");
    luks.closeWriteChannel();
    luks.waitForFinished(60000);
    report(luks.exitCode() == 0, QStringLiteral("make a LUKS test partition"), QString::fromLocal8Bit(luks.readAllStandardError()).trimmed());

    UDisks udisks;
    udisks.setInteractive(false);
    MainWindow window(&udisks);
    window.resize(1200, 800);
    window.show();
    auto volume = [&]() -> const Volume * {
        for (const Disk &d : udisks.disks()) {
            for (const Volume &v : d.volumes) {
                if (v.device == part)
                    return &v;
            }
        }
        return nullptr;
    };
    waitUntil([&] {
        udisks.refresh();
        return volume() && volume()->encrypted;
    }, 15000);
    auto *map = window.findChild<DiskMap *>();
    const QString path = volume() ? volume()->objectPath : QString();
    auto clickLock = [map, &path] {
        const QPoint at = map->lockRect(path).center();
        QMouseEvent press(QEvent::MouseButtonPress, at, map->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(map, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, at, map->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(map, &release);
    };
    report(map && !path.isEmpty() && !map->lockRect(path).isNull(), QStringLiteral("an encrypted partition has a lock on it in the map"));
    if (!map || path.isEmpty() || map->lockRect(path).isNull()) {
        sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
        return;
    }
    QString asked;
    Answerer answerer;
    answerer.answer = [&asked](QWidget *modal) {
        if (auto *input = qobject_cast<QInputDialog *>(modal)) {
            asked = input->windowTitle();
            input->setTextValue(QStringLiteral("diskforge-test"));
            input->accept();
            return true;
        }
        return false;
    };
    clickLock();
    // UDisks shows the unlocked side a moment before it answers; wait for both.
    waitUntil([&] {
        udisks.refresh();
        return volume() && !volume()->cleartextPath.isEmpty() && !udisks.isBusy();
    }, 20000);
    report(asked.startsWith(QLatin1String("Unlock")) && volume() && !volume()->cleartextPath.isEmpty(),
           QStringLiteral("clicking the lock asks for the passphrase and unlocks it"), asked);
    clickLock();
    waitUntil([&] {
        udisks.refresh();
        return volume() && volume()->cleartextPath.isEmpty();
    }, 20000);
    report(volume() && volume()->cleartextPath.isEmpty() && answerer.boxes.isEmpty(), QStringLiteral("clicking it again locks it"),
           answerer.boxes.join(QStringLiteral(" / ")));
    sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
}

// Inspect Partition Table on the system disk: read-only, so it opens without unmounting
// anything, and the table it shows is intact.
void inspector()
{
    UDisks udisks;
    udisks.setInteractive(false);
    MainWindow window(&udisks);
    window.show();
    QString system;
    for (const Disk &d : udisks.disks()) {
        if (d.isSystem)
            system = d.device;
    }
    if (system.isEmpty() || !window.selectDevice(system)) {
        report(true, QStringLiteral("the inspector on the system disk (skipped: no system disk found)"));
        return;
    }
    QString mountsBefore, mountsAfter;
    sh(QStringLiteral("findmnt"), {QStringLiteral("-rno"), QStringLiteral("TARGET,SOURCE")}, &mountsBefore);
    QAction *inspect = findAction(window, QStringLiteral("Inspect Partition Table"));
    report(inspect && inspect->isEnabled(), QStringLiteral("Inspect Partition Table is offered for the system disk"));
    gpt::Report seen;
    bool shown = false;
    Answerer answerer;
    answerer.answer = [&](QWidget *modal) {
        auto *dialog = qobject_cast<TableInspectorDialog *>(modal);
        if (!dialog)
            return false;
        if (dialog->isReady()) {
            seen = dialog->report();
            shown = true;
            dialog->close();
        }
        return true;
    };
    if (inspect)
        inspect->trigger();
    sh(QStringLiteral("findmnt"), {QStringLiteral("-rno"), QStringLiteral("TARGET,SOURCE")}, &mountsAfter);
    report(shown && mountsAfter == mountsBefore, QStringLiteral("it opens read-only: nothing got unmounted"));
    report(shown && (seen.primary.present ? seen.primary.valid() && seen.backupMatches : seen.mbr.signature),
           QStringLiteral("and it reads the system disk's table as intact"), seen.problems.join(QStringLiteral(" | ")));
}

// Recover Partitions through the window: a loop device whose main GPT header was wiped is
// put back from the backup copy, typing the device name to confirm.
void recoverThroughWindow()
{
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("recover.img"));
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("64M"), image});
    QProcess sfdisk;
    sfdisk.start(QStringLiteral("sfdisk"), {QStringLiteral("-q"), image});
    sfdisk.waitForStarted();
    sfdisk.write("label: gpt\nsize=16MiB, type=linux, name=\"one\"\nsize=24MiB, type=linux, name=\"two\"\n");
    sfdisk.closeWriteChannel();
    sfdisk.waitForFinished();
    QString loop;
    sh(QStringLiteral("losetup"), {QStringLiteral("-fP"), QStringLiteral("--show"), image}, &loop);
    loop = loop.trimmed();
    {
        QFile d(loop);
        if (d.open(QIODevice::ReadWrite) && d.seek(512)) {
            d.write(QByteArray(512, '\0')); // the main header
            d.flush();
        }
    }
    QString verify;
    sh(QStringLiteral("sfdisk"), {QStringLiteral("--verify"), loop}, &verify);

    UDisks udisks;
    udisks.setInteractive(false);
    MainWindow window(&udisks);
    window.show();
    waitUntil([&] {
        udisks.refresh();
        return window.selectDevice(loop);
    }, 15000);
    QAction *recoverAction = findAction(window, QStringLiteral("Recover Partitions"));
    report(recoverAction && recoverAction->isEnabled(), QStringLiteral("Recover Partitions is offered for a drive"), verify.simplified().left(120));
    bool written = false, success = false;
    QString result, offered;
    Answerer answerer;
    answerer.answer = [&](QWidget *modal) {
        auto *dialog = qobject_cast<RecoverDialog *>(modal);
        if (!dialog)
            return false;
        if (!written && dialog->sourceCount() > 0) {
            written = true;
            offered = dialog->findChild<QListWidget *>(QStringLiteral("sources"))->item(0)->text();
            QObject::connect(dialog, &RecoverDialog::done, dialog, [&, dialog](bool ok, const QString &message) {
                success = ok;
                result = message;
                dialog->close();
            });
            for (QLineEdit *edit : dialog->findChildren<QLineEdit *>())
                edit->setText(QFileInfo(loop).fileName());
            if (QPushButton *write = findButton(dialog, QStringLiteral("Write Partition Table")))
                write->click();
        }
        return true;
    };
    if (recoverAction)
        recoverAction->trigger();
    // sfdisk quietly uses the backup when the main header is gone, so look at sector 1 itself.
    QByteArray header;
    {
        QFile d(loop);
        if (d.open(QIODevice::ReadOnly) && d.seek(512))
            header = d.read(8);
    }
    report(offered.contains(QLatin1String("backup copy")) && success && header == "EFI PART",
           QStringLiteral("the main header is put back from the backup copy, through the window"), offered + QStringLiteral(" | ") + result);
    sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
}

// The scan, through the window: a loop device with ext4 and FAT where partitions used to
// be and no table at all. Scan, keep both, write, and they're partitions again.
void scanThroughWindow()
{
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("lost.img"));
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("256M"), image});
    sh(QStringLiteral("mke2fs"), {QStringLiteral("-q"), QStringLiteral("-t"), QStringLiteral("ext4"), QStringLiteral("-L"), QStringLiteral("root"),
                                  QStringLiteral("-E"), QStringLiteral("offset=1048576"), image, QStringLiteral("100M")});
    sh(QStringLiteral("mkfs.fat"), {QStringLiteral("-F"), QStringLiteral("16"), QStringLiteral("--offset"), QString::number(102 * 2048),
                                    QStringLiteral("-n"), QStringLiteral("DATA"), image, QStringLiteral("65536")});
    QString loop;
    // -P: the kernel only makes partitions on a loop device that scans for them.
    sh(QStringLiteral("losetup"), {QStringLiteral("-fP"), QStringLiteral("--show"), image}, &loop);
    loop = loop.trimmed();

    UDisks udisks;
    udisks.setInteractive(false);
    MainWindow window(&udisks);
    window.show();
    waitUntil([&] {
        udisks.refresh();
        return window.selectDevice(loop);
    }, 15000);
    QAction *recoverAction = findAction(window, QStringLiteral("Recover Partitions"));
    enum class Step { Start, Scanning, Writing, Done } step = Step::Start;
    QString result;
    bool success = false;
    Answerer answerer;
    answerer.answer = [&](QWidget *modal) {
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            if (!box->text().startsWith(QLatin1String("The scan reads")))
                return false;
            if (QPushButton *scan = findButton(box, QStringLiteral("Scan")))
                scan->click();
            step = Step::Scanning;
            return true;
        }
        auto *dialog = qobject_cast<RecoverDialog *>(modal);
        if (!dialog)
            return false;
        QPushButton *scan = findButton(dialog, QStringLiteral("Scan the Drive for File Systems…"));
        if (step == Step::Start && scan && scan->isEnabled()) {
            QObject::connect(dialog, &RecoverDialog::done, dialog, [&, dialog](bool ok, const QString &message) {
                success = ok;
                result = message;
                step = Step::Done;
                dialog->close();
            });
            // Queued: Scan asks first, and that question needs this timer to answer it.
            QTimer::singleShot(0, scan, &QPushButton::click);
            step = Step::Scanning;
        } else if (step == Step::Scanning) {
            auto *list = dialog->findChild<QListWidget *>(QStringLiteral("sources"));
            if (list && list->count() > 0 && list->item(list->count() - 1)->text().startsWith(QLatin1String("Found by scanning"))) {
                for (QLineEdit *edit : dialog->findChildren<QLineEdit *>())
                    edit->setText(QFileInfo(loop).fileName());
                if (QPushButton *write = findButton(dialog, QStringLiteral("Write Partition Table")); write && write->isEnabled()) {
                    step = Step::Writing;
                    write->click();
                }
            }
        }
        return true;
    };
    if (recoverAction)
        recoverAction->trigger();
    QString table;
    sh(QStringLiteral("sfdisk"), {QStringLiteral("--dump"), loop}, &table);
    QString types;
    sh(QStringLiteral("lsblk"), {QStringLiteral("-rno"), QStringLiteral("FSTYPE,LABEL"), loop}, &types);
    report(success && table.contains(QLatin1String("start=        2048, size=      204800")) && types.contains(QLatin1String("ext4 root"))
               && types.contains(QLatin1String("vfat DATA")),
           QStringLiteral("a scan through the window finds both file systems and makes them partitions again"),
           result + QStringLiteral(" | ") + types.simplified());
    sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
}

// A degraded RAID array gets a warning bar: RAID 1 on two loop devices, one failed.
void raidBanner()
{
    QTemporaryDir dir;
    QStringList loops;
    for (const char *name : {"a.img", "b.img"}) {
        const QString image = dir.filePath(QLatin1String(name));
        sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("64M"), image});
        QString loop;
        sh(QStringLiteral("losetup"), {QStringLiteral("-f"), QStringLiteral("--show"), image}, &loop);
        loops << loop.trimmed();
    }
    const int created = sh(QStringLiteral("mdadm"), {QStringLiteral("--create"), QStringLiteral("/dev/md/dfbanner"), QStringLiteral("--level=1"),
                                                     QStringLiteral("--raid-devices=2"), QStringLiteral("--metadata=1.2"), QStringLiteral("--assume-clean"),
                                                     QStringLiteral("--run"), QStringLiteral("--quiet"), loops.value(0), loops.value(1)});
    if (created == 0)
        sh(QStringLiteral("mdadm"), {QStringLiteral("--manage"), QStringLiteral("/dev/md/dfbanner"), QStringLiteral("--fail"), loops.value(1)});
    {
        UDisks udisks;
        udisks.setInteractive(false);
        MainWindow window(&udisks);
        window.show();
        QString text;
        waitUntil([&] {
            udisks.refresh();
            for (NoticeBar *b : window.findChildren<NoticeBar *>(QStringLiteral("health"))) {
                if (!b->isHidden() && b->text().contains(QLatin1String("dfbanner")))
                    text = b->text();
            }
            return !text.isEmpty();
        }, 20000);
        report(created == 0 && text.contains(QLatin1String("missing 1 of its 2 drives")), QStringLiteral("a degraded RAID array gets a warning bar"), text);
    }
    sh(QStringLiteral("mdadm"), {QStringLiteral("--stop"), QStringLiteral("/dev/md/dfbanner")});
    for (const QString &loop : std::as_const(loops)) {
        sh(QStringLiteral("mdadm"), {QStringLiteral("--zero-superblock"), loop});
        sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
    }
}

// Job bars, with jobs faked through the test hook: a firmware erase says it can't be
// stopped and has no Stop; a wipe has Stop and turns on the toolbar's Stop; when the jobs
// end, the bars go.
void jobBars()
{
    UDisks udisks;
    udisks.setInteractive(false);
    if (udisks.disks().isEmpty()) {
        report(true, QStringLiteral("job bars (skipped: no drives)"));
        return;
    }
    const Disk d = udisks.disks().constFirst();
    Job erase;
    erase.path = QStringLiteral("/test/jobs/erase");
    erase.operation = QStringLiteral("ata-secure-erase");
    erase.objects = {d.drivePath};
    erase.progress = 0.1;
    erase.progressValid = true;
    udisks.setJobsForTest({erase});
    udisks.refresh();
    MainWindow window(&udisks);
    window.show();
    QAction *stop = findAction(window, QStringLiteral("Stop"));
    auto jobBars = [&window] {
        QList<NoticeBar *> out;
        for (NoticeBar *b : window.findChildren<NoticeBar *>(QStringLiteral("job"))) {
            if (!b->isHidden())
                out << b;
        }
        return out;
    };
    QList<NoticeBar *> bars = jobBars();
    report(bars.size() == 1 && bars[0]->text().contains(QLatin1String("can't be interrupted")) && bars[0]->findChildren<QPushButton *>().isEmpty()
               && stop && !stop->isEnabled(),
           QStringLiteral("a firmware erase gets a bar that says it can't be stopped, with no Stop"), bars.value(0) ? bars[0]->text() : QString());

    Job wipe;
    wipe.path = QStringLiteral("/test/jobs/wipe");
    wipe.operation = QStringLiteral("format-erase");
    wipe.cancelable = true;
    wipe.objects = {d.blockPath};
    wipe.progress = 0.34;
    wipe.progressValid = true;
    wipe.rate = 49000000;
    udisks.setJobsForTest({wipe});
    udisks.refresh();
    bars = jobBars();
    const QList<QPushButton *> buttons = bars.value(0) ? bars[0]->findChildren<QPushButton *>() : QList<QPushButton *>();
    report(bars.size() == 1 && bars[0]->text().startsWith(QLatin1String("Wiping")) && bars[0]->text().contains(QLatin1String("34%"))
               && buttons.size() == 1 && buttons[0]->text() == QLatin1String("Stop Wiping") && stop && stop->isEnabled(),
           QStringLiteral("a wipe gets a bar with Stop Wiping, and the toolbar's Stop is on"), bars.value(0) ? bars[0]->text() : QString());

    udisks.setJobsForTest({});
    udisks.refresh();
    QCoreApplication::processEvents();
    report(jobBars().isEmpty() && stop && !stop->isEnabled(), QStringLiteral("when the jobs end, the bars and Stop go"));
}

// Unmounts everything on a test loop device and lets go of it. The desktop's file indexer can
// hold a freshly mounted stick for a moment, so unmounting is tried a few times. Reports a
// failure if the loop device stays behind.
void cleanUpLoop(UDisks &udisks, const QString &loopPath)
{
    const QVariantMap quiet{{QStringLiteral("auth.no_user_interaction"), true}};
    waitUntil([&] {
        udisks.refresh();
        bool mounted = false;
        if (const Disk *d = udisks.diskByPath(loopPath)) {
            for (const Volume &v : d->volumes) {
                if (v.mountPoints.isEmpty())
                    continue;
                mounted = true;
                QDBusMessage unmount = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.UDisks2"), v.objectPath,
                                                                      QStringLiteral("org.freedesktop.UDisks2.Filesystem"), QStringLiteral("Unmount"));
                unmount << quiet;
                QDBusConnection::systemBus().call(unmount, QDBus::Block, 30000);
            }
        }
        if (mounted)
            QThread::msleep(500);
        return !mounted;
    }, 20000);
    QDBusMessage remove = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.UDisks2"), loopPath,
                                                         QStringLiteral("org.freedesktop.UDisks2.Loop"), QStringLiteral("Delete"));
    remove << quiet;
    const QDBusMessage removed = QDBusConnection::systemBus().call(remove, QDBus::Block, 30000);
    waitUntil([&] {
        udisks.refresh();
        return udisks.diskByPath(loopPath) == nullptr;
    }, 10000);
    report(udisks.diskByPath(loopPath) == nullptr, QStringLiteral("the test stick is cleaned up afterwards"), removed.errorMessage());
}

// A loop device of the user's own for `image` (UDisks sets those up without a password).
QString userLoop(const QString &image)
{
    QFile backing(image);
    if (!backing.open(QIODevice::ReadWrite))
        return {};
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.UDisks2"), QStringLiteral("/org/freedesktop/UDisks2/Manager"),
                                                       QStringLiteral("org.freedesktop.UDisks2.Manager"), QStringLiteral("LoopSetup"));
    call << QVariant::fromValue(QDBusUnixFileDescriptor(backing.handle())) << QVariantMap{{QStringLiteral("auth.no_user_interaction"), true}};
    const QDBusMessage reply = QDBusConnection::systemBus().call(call, QDBus::Block, 30000);
    return reply.arguments().value(0).value<QDBusObjectPath>().path();
}

// Mounts the stick's partition labelled `label` and gives back where.
QString mountStick(UDisks &udisks, const QString &loopPath, const QString &label)
{
    QString root;
    bool asked = false;
    waitUntil([&] {
        udisks.refresh();
        const Disk *d = udisks.diskByPath(loopPath);
        for (const Volume &v : d ? d->volumes : QVector<Volume>()) {
            if (v.label != label)
                continue;
            if (!v.mountPoints.isEmpty())
                root = v.mountPoints.first();
            else if (!asked) {
                udisks.mount(v);
                asked = true;
            }
        }
        return !root.isEmpty();
    }, 15000);
    return root;
}

// How many files under `root` match its sha256sum.txt; `listed` gets how many it lists.
int filesMatchingSums(const QString &root, int *listed)
{
    QFile sumsFile(root + QStringLiteral("/sha256sum.txt"));
    const QHash<QString, QByteArray> sums = sumsFile.open(QIODevice::ReadOnly) ? rescue::parseSums(sumsFile.readAll()) : QHash<QString, QByteArray>();
    int good = 0;
    for (auto it = sums.cbegin(); it != sums.cend(); ++it) {
        QFile f(root + QLatin1Char('/') + it.key());
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (f.open(QIODevice::ReadOnly) && hash.addData(&f) && hash.result().toHex() == it.value())
            ++good;
    }
    *listed = int(sums.size());
    return good;
}

// Make a DiskForge Live USB writes the boot code for BIOS PCs into the stick's image file instead
// of going through UDisks (see RescueUsbDialog::openStickForTest).
void writeBiosBootTo(const QString &image)
{
    RescueUsbDialog::openStickForTest = [image](const QString &) {
        return ::open(QFile::encodeName(image).constData(), O_RDWR | O_CLOEXEC);
    };
}

// Whether what's in front of the stick's partition is the boot code from its own files: GRUB's
// code in the first 440 bytes (the partition table behind it stays), its core right after.
bool biosBootMatches(const QString &image, const QString &root, QString *detail)
{
    auto readAll = [](const QString &path, qint64 len = -1) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return QByteArray();
        return len < 0 ? f.readAll() : f.read(len);
    };
    const QByteArray boot = readAll(root + QStringLiteral("/boot/grub/i386-pc/stick-boot.img"));
    const QByteArray core = readAll(root + QStringLiteral("/boot/grub/i386-pc/stick-core.img"));
    const QByteArray start = readAll(image, 512 + core.size());
    *detail = QStringLiteral("boot %1 bytes, core %2 bytes").arg(boot.size()).arg(core.size());
    return boot.size() == 512 && !core.isEmpty() && start.size() == 512 + core.size() && start.left(440) == boot.left(440)
        && uchar(start[510]) == 0x55 && uchar(start[511]) == 0xAA && start.mid(512) == core;
}

// Inside DiskForge Live, Make a DiskForge Live USB copies the stick it runs from. Here the stick
// made a moment ago (mounted at `sourceRoot`, build ID "test") stands in for it, with a fake
// rescue conf: the dialog finds it by its build ID, won't copy it onto itself, and copies it
// onto a second stick with every file checked.
void copyRunningStick(UDisks &udisks, const QString &sourceLoop, const QString &sourceRoot, const QTemporaryDir &dir)
{
    const QString realConf = rescue::rescueConfPath;
    rescue::rescueConfPath = dir.filePath(QStringLiteral("diskforge-live.conf"));
    QFile conf(rescue::rescueConfPath);
    if (!conf.open(QIODevice::WriteOnly) || conf.write("VERSION=0.0.1\nBUILD_ID=test\n") < 0) {
        report(false, QStringLiteral("write a test rescue conf"), conf.errorString());
        rescue::rescueConfPath = realConf;
        return;
    }
    conf.close();
    report(RescueUsbDialog::findImage() == sourceRoot, QStringLiteral("in the rescue, it finds the stick it runs from by its build ID"),
           RescueUsbDialog::findImage());

    const QString image = dir.filePath(QStringLiteral("second.img"));
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("600M"), image});
    const QString loopPath = userLoop(image);
    const Disk *second = nullptr;
    waitUntil([&] {
        udisks.refresh();
        second = udisks.diskByPath(loopPath);
        return second != nullptr;
    }, 15000);
    report(second != nullptr, QStringLiteral("a second loop device for the copy"));
    if (!second) {
        rescue::rescueConfPath = realConf;
        return;
    }
    const QString device = second->device;

    QStringList boxes;
    Answerer answerer;
    answerer.answer = [&](QWidget *modal) {
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            boxes << box->text();
            box->accept();
            return true;
        }
        return false;
    };
    writeBiosBootTo(image);
    {
        RescueUsbDialog dialog(&udisks, sourceLoop);
        dialog.show();
        QCoreApplication::processEvents();
        auto *imageEdit = dialog.findChild<QLineEdit *>(QStringLiteral("image"));
        auto *info = dialog.findChild<QLabel *>(QStringLiteral("imageInfo"));
        auto *targets = dialog.findChild<QComboBox *>(QStringLiteral("targets"));
        auto *warning = dialog.findChild<QLabel *>(QStringLiteral("warning"));
        auto *confirm = dialog.findChild<QLineEdit *>(QStringLiteral("confirm"));
        QPushButton *make = findButton(&dialog, QStringLiteral("Make the DiskForge Live USB"));
        report(imageEdit->text() == sourceRoot && info->text().startsWith(QLatin1String("A copy of the rescue stick")),
               QStringLiteral("the dialog starts with the running stick"), info->text());
        report(targets->currentData().toString() == sourceLoop && warning->text().contains(QLatin1String("being copied"))
                   && make && !make->isEnabled() && !confirm->isVisibleTo(&dialog),
               QStringLiteral("it won't copy the stick onto itself"), warning->text());
        targets->setCurrentIndex(targets->findData(loopPath));
        confirm->setText(shortDevice(device));
        report(make && make->isEnabled(), QStringLiteral("onto the second stick, typing its name unlocks the button"), warning->text());
        if (make)
            make->click();
        waitUntil([&] { return !dialog.isVisible() || boxes.size() > 0; }, 120000);
        waitUntil([&] { return !dialog.isVisible(); }, 5000);
        report(boxes.size() == 1 && boxes[0].startsWith(QLatin1String("The DiskForge Live USB is ready")), QStringLiteral("the copy is ready"),
               boxes.join(QStringLiteral(" | ")));
    }
    RescueUsbDialog::openStickForTest = nullptr;
    const QString root = mountStick(udisks, loopPath, QStringLiteral("DISKFORGE"));
    int listed = 0;
    const int good = filesMatchingSums(root, &listed);
    report(listed > 0 && good == listed && rescue::stickInfo(root).id == QLatin1String("test"),
           QStringLiteral("the copy has every file, matching sha256sum.txt"), QStringLiteral("%1 of %2").arg(good).arg(listed));
    QString biosDetail;
    report(boxes.value(0).contains(QLatin1String("and old BIOS PCs")) && biosBootMatches(image, root, &biosDetail),
           QStringLiteral("the copy starts old BIOS PCs too"), biosDetail);
    rescue::rescueConfPath = realConf;
    cleanUpLoop(udisks, loopPath);
}

// Make a DiskForge Live USB on a loop device of the user's own (UDisks lets you set those up and
// change them without a password), through the dialog like a user would.
// DISKFORGE_TEST_RESCUE_ISO uses a real rescue image instead of a small made-up one, and with
// it DISKFORGE_TEST_STICK keeps the stick's image file afterwards, to boot it in a VM.
void rescueUsb()
{
    QTemporaryDir dir;
    QString iso = qEnvironmentVariable("DISKFORGE_TEST_RESCUE_ISO");
    if (iso.isEmpty()) {
        const QString tree = dir.filePath(QStringLiteral("tree"));
        QMap<QString, QByteArray> files;
        files[QStringLiteral(".disk/diskforge-live")] = "DiskForge Live\nversion=0.0.1\nbuilt=2026-10-09\nid=test\n";
        files[QStringLiteral("EFI/BOOT/BOOTX64.EFI")] = QByteArray(300000, 'x');
        files[QStringLiteral("boot/grub/i386-pc/stick-boot.img")] = QByteArray(510, 'b') + QByteArray("\x55\xaa", 2);
        files[QStringLiteral("boot/grub/i386-pc/stick-core.img")] = QByteArray(40000, 'c');
        files[QStringLiteral("live/filesystem.squashfs")] = QByteArray(9 * 1024 * 1024, 's');
        QByteArray sums;
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            QDir().mkpath(QFileInfo(tree + QLatin1Char('/') + it.key()).path());
            QFile f(tree + QLatin1Char('/') + it.key());
            if (f.open(QIODevice::WriteOnly))
                f.write(it.value());
            sums += QCryptographicHash::hash(it.value(), QCryptographicHash::Sha256).toHex() + "  ./" + it.key().toUtf8() + "\n";
        }
        QFile sumsFile(tree + QStringLiteral("/sha256sum.txt"));
        if (sumsFile.open(QIODevice::WriteOnly))
            sumsFile.write(sums);
        sumsFile.close();
        iso = dir.filePath(QStringLiteral("rescue.iso"));
        if (sh(QStringLiteral("xorriso"), {QStringLiteral("-as"), QStringLiteral("mkisofs"), QStringLiteral("-quiet"), QStringLiteral("-J"),
                                           QStringLiteral("-joliet-long"), QStringLiteral("-R"), QStringLiteral("-o"), iso, tree}) != 0) {
            out << "SKIP  Make a DiskForge Live USB: xorriso isn't installed" << Qt::endl;
            return;
        }
    }
    // Only with a real image: the other scenarios would make their own sticks over it.
    QString stickImage = qEnvironmentVariable("DISKFORGE_TEST_RESCUE_ISO").isEmpty() ? QString() : qEnvironmentVariable("DISKFORGE_TEST_STICK");
    if (stickImage.isEmpty())
        stickImage = dir.filePath(QStringLiteral("stick.img"));
    const bool big = QFileInfo(iso).size() > 100 * 1024 * 1024;
    QFile::remove(stickImage);
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), big ? QStringLiteral("3G") : QStringLiteral("600M"), stickImage});

    const QString loopPath = userLoop(stickImage);
    report(!loopPath.isEmpty(), QStringLiteral("Make a DiskForge Live USB: a loop device stands in for the stick"));
    if (loopPath.isEmpty())
        return;

    UDisks udisks;
    udisks.setInteractive(false);
    const Disk *stick = nullptr;
    waitUntil([&] {
        udisks.refresh();
        stick = udisks.diskByPath(loopPath);
        return stick != nullptr;
    }, 15000);
    const QString device = stick ? stick->device : QString();

    RescueUsbDialog::allowLoopDevicesForTest = true;
    QStringList steps;
    const auto stepConn = QObject::connect(&udisks, &UDisks::operationFinished, [&](bool ok, const QString &m) {
        steps << (ok ? QString() : QStringLiteral("FAILED ")) + m;
    });
    writeBiosBootTo(stickImage);
    QString readyText;
    {
        QStringList boxes;
        Answerer answerer;
        answerer.answer = [&](QWidget *modal) {
            if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                boxes << box->text();
                box->accept();
                return true;
            }
            return false;
        };
        RescueUsbDialog dialog(&udisks, loopPath);
        dialog.show();
        auto *image = dialog.findChild<QLineEdit *>(QStringLiteral("image"));
        auto *confirm = dialog.findChild<QLineEdit *>(QStringLiteral("confirm"));
        auto *info = dialog.findChild<QLabel *>(QStringLiteral("imageInfo"));
        QPushButton *make = findButton(&dialog, QStringLiteral("Make the DiskForge Live USB"));
        image->setText(iso);
        report(info->text().contains(QLatin1String("DiskForge Live")), QStringLiteral("the dialog reads the image"), info->text());
        report(make && !make->isEnabled(), QStringLiteral("nothing happens before the device name is typed"));
        confirm->setText(shortDevice(device));
        report(make && make->isEnabled(), QStringLiteral("typing it unlocks the button"));
        if (make)
            make->click();
        waitUntil([&] { return !dialog.isVisible() || boxes.size() > 0; }, big ? 900000 : 120000);
        waitUntil([&] { return !dialog.isVisible(); }, 5000);
        report(boxes.size() == 1 && boxes[0].startsWith(QLatin1String("The DiskForge Live USB is ready")), QStringLiteral("it reports the stick ready"),
               (boxes + steps).join(QStringLiteral(" | ")));
        readyText = boxes.value(0);
    }
    RescueUsbDialog::openStickForTest = nullptr;
    QObject::disconnect(stepConn);

    // What it made: MBR, one FAT32 partition, bootable, with every file checked.
    const Volume *part = nullptr;
    waitUntil([&] {
        udisks.refresh();
        stick = udisks.diskByPath(loopPath);
        part = nullptr;
        for (const Volume &v : stick ? stick->volumes : QVector<Volume>()) {
            if (v.label == QLatin1String("DISKFORGE"))
                part = &v;
        }
        return part != nullptr;
    }, 15000);
    report(stick && stick->tableType == QLatin1String("dos") && stick->volumes.size() == 1, QStringLiteral("the stick has an MBR with one partition"));
    report(part && part->fsType == QLatin1String("vfat") && part->partType == QLatin1String("0x0c") && (part->partFlags & 0x80),
           QStringLiteral("FAT32, type 0x0c, marked bootable"),
           part ? QStringLiteral("%1 %2 %3").arg(part->fsType, part->partType).arg(part->partFlags, 0, 16) : QString());
    report(part && part->mountPoints.isEmpty(), QStringLiteral("and it's unmounted at the end"));
    const bool madeOne = part != nullptr;
    if (madeOne) {
        const QString root = mountStick(udisks, loopPath, QStringLiteral("DISKFORGE"));
        int listed = 0;
        const int good = filesMatchingSums(root, &listed);
        report(listed > 0 && good == listed, QStringLiteral("every file on it matches sha256sum.txt"),
               QStringLiteral("%1 of %2").arg(good).arg(listed));
        QString biosDetail;
        report(readyText.contains(QLatin1String("and old BIOS PCs")) && biosBootMatches(stickImage, root, &biosDetail),
               QStringLiteral("GRUB's boot code for old BIOS PCs is in front of the partition"), biosDetail);

        // Opened again, the dialog knows the stick.
        RescueUsbDialog again(&udisks, loopPath);
        again.show();
        QCoreApplication::processEvents();
        auto *stickInfo = again.findChild<QLabel *>(QStringLiteral("stickInfo"));
        QPushButton *openLogs = findButton(&again, QStringLiteral("Open Logs"));
        report(stickInfo && stickInfo->text().contains(QLatin1String("has DiskForge Live")) && openLogs && openLogs->isVisibleTo(&again),
               QStringLiteral("opened again, it says the stick has DiskForge Live and offers its logs"), stickInfo ? stickInfo->text() : QString());
        again.close();
        // The made-up image's build ID is "test"; a real one has its own.
        if (rescue::stickInfo(root).id == QLatin1String("test"))
            copyRunningStick(udisks, loopPath, root, dir);
    }
    RescueUsbDialog::allowLoopDevicesForTest = false;

    cleanUpLoop(udisks, loopPath);
}

// Check a USB Stick on a loop device, through the dialog. As root: the quick check says a
// genuine "stick" is fine, then Format It leaves one partition on it. As the user: writing to
// a drive directly needs the admin password, so without a prompt it's refused, and says so.
void stickCheck()
{
    const bool asRoot = geteuid() == 0;
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("stick.img"));
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("128M"), image});
    QFile backing(image);
    QString loopPath;
    if (backing.open(QIODevice::ReadWrite)) {
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.UDisks2"), QStringLiteral("/org/freedesktop/UDisks2/Manager"),
                                                           QStringLiteral("org.freedesktop.UDisks2.Manager"), QStringLiteral("LoopSetup"));
        call << QVariant::fromValue(QDBusUnixFileDescriptor(backing.handle())) << QVariantMap{{QStringLiteral("auth.no_user_interaction"), true}};
        loopPath = QDBusConnection::systemBus().call(call, QDBus::Block, 30000).arguments().value(0).value<QDBusObjectPath>().path();
        backing.close();
    }
    report(!loopPath.isEmpty(), QStringLiteral("Check a USB Stick: a loop device stands in for the stick"));
    if (loopPath.isEmpty())
        return;
    UDisks udisks;
    udisks.setInteractive(false);
    const Disk *stick = nullptr;
    waitUntil([&] {
        udisks.refresh();
        stick = udisks.diskByPath(loopPath);
        return stick != nullptr;
    }, 15000);
    const QString device = stick ? stick->device : QString();

    StickCheckDialog::allowLoopDevicesForTest = true;
    QStringList boxes;
    Answerer answerer;
    answerer.answer = [&](QWidget *modal) {
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            boxes << box->text();
            if (QAbstractButton *yes = box->button(QMessageBox::Yes))
                yes->click();
            else
                box->accept();
            return true;
        }
        return false;
    };
    {
        StickCheckDialog dialog(&udisks, loopPath);
        dialog.show();
        auto *confirm = dialog.findChild<QLineEdit *>(QStringLiteral("confirm"));
        auto *result = dialog.findChild<QLabel *>(QStringLiteral("result"));
        QPushButton *start = findButton(&dialog, QStringLiteral("Start the Check"));
        report(start && !start->isEnabled(), QStringLiteral("nothing starts before the device name is typed"));
        confirm->setText(shortDevice(device));
        if (start)
            start->click();
        waitUntil([&] { return !result->text().isEmpty(); }, 120000);
        if (!asRoot) {
            report(result->text().contains(QLatin1String("Couldn't open the stick")),
                   QStringLiteral("as the user without a password prompt, it can't open the stick and says so"), result->text().left(120));
        } else {
            report(result->text().contains(QLatin1String("No problems found")), QStringLiteral("the quick check says a genuine stick is fine"),
                   result->text().left(200));
        }
        QPushButton *format = findButton(&dialog, QStringLiteral("Format It"));
        // As the user nothing was checked, so nothing is offered. Format It is tried anyway: it
        // goes through UDisks' own steps, which don't need a password on your own loop device.
        if (asRoot)
            report(format && format->isVisible(), QStringLiteral("and offers to format it"));
        else
            report(format && !format->isVisible(), QStringLiteral("and offers nothing after that"));
        if (format)
            format->click();
        waitUntil([&] { return std::any_of(boxes.cbegin(), boxes.cend(), [](const QString &b) { return b.startsWith(QLatin1String("Done")); }); }, 60000);
        report(std::any_of(boxes.cbegin(), boxes.cend(), [](const QString &b) { return b.startsWith(QLatin1String("Done")); }),
               QStringLiteral("Format It leaves one partition on it"), boxes.join(QStringLiteral(" | ")).left(300));
    }
    StickCheckDialog::allowLoopDevicesForTest = false;
    udisks.refresh();
    stick = udisks.diskByPath(loopPath);
    report(stick && stick->tableType == QLatin1String("dos") && stick->volumes.size() == 1 && stick->volumes[0].fsType == QLatin1String("vfat"),
           QStringLiteral("an MBR with one FAT32 partition"));

    cleanUpLoop(udisks, loopPath);
}

// Write Image to USB's "copy the files" way, with persistence, on a loop device of the user's
// own (no raw access needed, so no password): a Debian-live-like ISO with a long label.
// DISKFORGE_TEST_COPY_ISO uses a real ISO instead (one with live-boot persistence, like
// DiskForge Live), and DISKFORGE_TEST_STICK keeps the stick's image to boot it in a VM.
void isoCopy()
{
    QTemporaryDir dir;
    const QString realIso = qEnvironmentVariable("DISKFORGE_TEST_COPY_ISO");
    const QString tree = dir.filePath(QStringLiteral("tree"));
    auto put = [&](const QString &path, const QByteArray &data) {
        QDir().mkpath(QFileInfo(tree + QLatin1Char('/') + path).path());
        QFile f(tree + QLatin1Char('/') + path);
        if (f.open(QIODevice::WriteOnly))
            f.write(data);
    };
    put(QStringLiteral("EFI/BOOT/BOOTX64.EFI"), QByteArray(8192, 'e'));
    put(QStringLiteral("live/vmlinuz"), QByteArray(300000, 'k'));
    put(QStringLiteral("live/filesystem.squashfs"), QByteArray(2 * 1024 * 1024, 's'));
    put(QStringLiteral("boot/grub/grub.cfg"), "search --set=root --label My\\x20Live\\x201.0\nmenuentry \"Live\" {\n"
                                              "    linux /live/vmlinuz boot=live quiet\n    initrd /live/initrd.img\n}\n");
    QString iso = realIso;
    if (iso.isEmpty()) {
        iso = dir.filePath(QStringLiteral("live.iso"));
        if (sh(QStringLiteral("xorriso"), {QStringLiteral("-as"), QStringLiteral("mkisofs"), QStringLiteral("-quiet"), QStringLiteral("-J"),
                                           QStringLiteral("-joliet-long"), QStringLiteral("-R"), QStringLiteral("-V"), QStringLiteral("My Live 1.0"),
                                           QStringLiteral("-o"), iso, tree}) != 0) {
            out << "SKIP  copy mode: xorriso isn't installed" << Qt::endl;
            return;
        }
    }
    const QString label = isomode::fatLabel(filecopy::openIso(iso)->label());
    QString image = realIso.isEmpty() ? QString() : qEnvironmentVariable("DISKFORGE_TEST_STICK");
    if (image.isEmpty())
        image = dir.filePath(QStringLiteral("stick.img"));
    QFile::remove(image);
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), realIso.isEmpty() ? QStringLiteral("3G") : QStringLiteral("4G"), image});
    QFile backing(image);
    QString loopPath;
    if (backing.open(QIODevice::ReadWrite)) {
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.UDisks2"), QStringLiteral("/org/freedesktop/UDisks2/Manager"),
                                                           QStringLiteral("org.freedesktop.UDisks2.Manager"), QStringLiteral("LoopSetup"));
        call << QVariant::fromValue(QDBusUnixFileDescriptor(backing.handle())) << QVariantMap{{QStringLiteral("auth.no_user_interaction"), true}};
        loopPath = QDBusConnection::systemBus().call(call, QDBus::Block, 30000).arguments().value(0).value<QDBusObjectPath>().path();
        backing.close();
    }
    report(!loopPath.isEmpty(), QStringLiteral("copy mode: a loop device stands in for the stick"));
    if (loopPath.isEmpty())
        return;
    UDisks udisks;
    udisks.setInteractive(false);
    const Disk *stick = nullptr;
    waitUntil([&] {
        udisks.refresh();
        stick = udisks.diskByPath(loopPath);
        return stick != nullptr;
    }, 15000);
    const QString device = stick ? stick->device : QString();

    WriteImageDialog::allowLoopDevicesForTest = true;
    QStringList boxes;
    Answerer answerer;
    answerer.answer = [&](QWidget *modal) {
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            boxes << box->text();
            box->accept();
            return true;
        }
        return false;
    };
    {
        WriteImageDialog dialog(&udisks, loopPath);
        dialog.show();
        dialog.findChild<QLineEdit *>(QStringLiteral("image"))->setText(iso);
        auto *copyMode = dialog.findChild<QRadioButton *>(QStringLiteral("copyMode"));
        auto *persist = dialog.findChild<QCheckBox *>(QStringLiteral("persist"));
        report(copyMode && copyMode->isVisibleTo(&dialog) && copyMode->isEnabled(), QStringLiteral("an ISO gets the choice to copy its files"));
        copyMode->setChecked(true);
        report(persist && persist->isEnabled(), QStringLiteral("and, for a Debian-style live system, to keep changes"));
        persist->setChecked(true);
        dialog.findChild<QSpinBox *>(QStringLiteral("persistSize"))->setValue(1);
        dialog.findChild<QLineEdit *>(QStringLiteral("confirm"))->setText(shortDevice(device));
        QPushButton *write = findButton(&dialog, QStringLiteral("Write"));
        report(write && write->isEnabled(), QStringLiteral("typing the device name unlocks Write"));
        if (write)
            write->click();
        waitUntil([&] { return !dialog.isVisible() || !boxes.isEmpty(); }, 900000);
        waitUntil([&] { return !dialog.isVisible(); }, 5000);
        report(boxes.size() == 1 && boxes[0].startsWith(QLatin1String("The USB stick is ready")), QStringLiteral("it says the stick is ready"),
               boxes.join(QStringLiteral(" | ")).left(300));
    }
    WriteImageDialog::allowLoopDevicesForTest = false;

    const Volume *fat = nullptr, *ext = nullptr;
    waitUntil([&] {
        udisks.refresh();
        stick = udisks.diskByPath(loopPath);
        fat = ext = nullptr;
        for (const Volume &v : stick ? stick->volumes : QVector<Volume>()) {
            if (v.fsType == QLatin1String("vfat"))
                fat = &v;
            else if (v.fsType == QLatin1String("ext4"))
                ext = &v;
        }
        return fat && ext;
    }, 15000);
    const bool layout = fat && fat->label == label && (fat->partFlags & 0x80) && ext && ext->label == QLatin1String("persistence");
    const QString fatLabel = fat ? fat->label : QStringLiteral("no FAT");
    fat = ext = nullptr; // the next refresh replaces them
    report(layout, QStringLiteral("FAT32 with the ISO's label made fit, bootable, then ext4 called persistence"), fatLabel);
    // By object path: a refresh replaces the disk list, so Volume pointers don't last.
    auto mounted = [&](const QString &fsType) -> QString {
        udisks.refresh();
        QString path;
        for (const Volume &x : udisks.diskByPath(loopPath) ? udisks.diskByPath(loopPath)->volumes : QVector<Volume>()) {
            if (x.fsType == fsType) {
                path = x.objectPath;
                udisks.mount(x);
                break;
            }
        }
        QString root;
        waitUntil([&] {
            udisks.refresh();
            for (const Volume &x : udisks.diskByPath(loopPath) ? udisks.diskByPath(loopPath)->volumes : QVector<Volume>()) {
                if (x.objectPath == path && !x.mountPoints.isEmpty())
                    root = x.mountPoints.first();
            }
            return !root.isEmpty();
        }, 15000);
        return root;
    };
    const QString fatRoot = mounted(QStringLiteral("vfat"));
    QFile cfg(fatRoot + QStringLiteral("/boot/grub/grub.cfg"));
    const QByteArray menu = cfg.open(QIODevice::ReadOnly) ? cfg.readAll() : QByteArray();
    report((!realIso.isEmpty() || menu.contains("--label MY_LIVE_1_0")) && menu.contains("boot=live persistence"),
           QStringLiteral("the boot menu points at the stick's label and turns persistence on"), QString::fromUtf8(menu).left(160));
    cfg.close();
    const QString extRoot = mounted(QStringLiteral("ext4"));
    QFile conf(extRoot + QStringLiteral("/persistence.conf"));
    report(conf.open(QIODevice::ReadOnly) && conf.readAll() == "/ union\n", QStringLiteral("persistence.conf says to keep everything"));
    conf.close();

    cleanUpLoop(udisks, loopPath);
}

// Make a Windows USB with a small fake Windows ISO (real WIMs made by wimlib), on a loop device
// of the user's own: install.wim split into 1 MiB parts and checked, the answer file written,
// and the ISO's own loop device gone at the end. DISKFORGE_TEST_WINDOWS_ISO uses a real
// Windows ISO instead (split at the real size), and DISKFORGE_TEST_STICK keeps the stick's
// image, to start Windows Setup from it in a VM.
void windowsUsb()
{
    const QString realIso = qEnvironmentVariable("DISKFORGE_TEST_WINDOWS_ISO");
    if (QStandardPaths::findExecutable(QStringLiteral("wimlib-imagex")).isEmpty() || QStandardPaths::findExecutable(QStringLiteral("xorriso")).isEmpty()) {
        out << "SKIP  Make a Windows USB: wimlib or xorriso isn't installed" << Qt::endl;
        return;
    }
    QTemporaryDir dir;
    const QString tree = dir.filePath(QStringLiteral("tree"));
    auto put = [&](const QString &path, const QByteArray &data) {
        QDir().mkpath(QFileInfo(tree + QLatin1Char('/') + path).path());
        QFile f(tree + QLatin1Char('/') + path);
        if (f.open(QIODevice::WriteOnly))
            f.write(data);
    };
    put(QStringLiteral("bootmgr.efi"), QByteArray(4096, 'b'));
    put(QStringLiteral("efi/boot/bootx64.efi"), QByteArray(4096, 'e'));
    put(QStringLiteral("setup.exe"), QByteArray(4096, 's'));
    // Content that doesn't pack, so install.wim really is a few MiB.
    QByteArray noise(3 * 1024 * 1024, Qt::Uninitialized);
    QRandomGenerator rng(99);
    for (qsizetype i = 0; i < noise.size(); i += 4)
        *reinterpret_cast<quint32 *>(noise.data() + i) = rng.generate();
    QDir().mkpath(dir.filePath(QStringLiteral("content")));
    QFile n(dir.filePath(QStringLiteral("content/noise.bin")));
    if (n.open(QIODevice::WriteOnly))
        n.write(noise);
    n.close();
    QDir().mkpath(tree + QStringLiteral("/sources"));
    sh(QStringLiteral("wimlib-imagex"), {QStringLiteral("capture"), dir.filePath(QStringLiteral("content")), tree + QStringLiteral("/sources/install.wim"),
                                         QStringLiteral("Windows 11 Pro"), QStringLiteral("--compress=none")});
    QString iso = realIso;
    if (iso.isEmpty()) {
        iso = dir.filePath(QStringLiteral("Win11_Test.iso"));
        sh(QStringLiteral("xorriso"), {QStringLiteral("-as"), QStringLiteral("mkisofs"), QStringLiteral("-quiet"), QStringLiteral("-J"), QStringLiteral("-joliet-long"),
                                       QStringLiteral("-R"), QStringLiteral("-V"), QStringLiteral("CCCOMA_X64FRE_EN-US_DV9"), QStringLiteral("-o"), iso, tree});
    }
    QString image = realIso.isEmpty() ? QString() : qEnvironmentVariable("DISKFORGE_TEST_STICK");
    if (image.isEmpty())
        image = dir.filePath(QStringLiteral("stick.img"));
    QFile::remove(image);
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), realIso.isEmpty() ? QStringLiteral("600M") : QStringLiteral("16G"), image});
    QFile backing(image);
    QString loopPath;
    if (backing.open(QIODevice::ReadWrite)) {
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.UDisks2"), QStringLiteral("/org/freedesktop/UDisks2/Manager"),
                                                           QStringLiteral("org.freedesktop.UDisks2.Manager"), QStringLiteral("LoopSetup"));
        call << QVariant::fromValue(QDBusUnixFileDescriptor(backing.handle())) << QVariantMap{{QStringLiteral("auth.no_user_interaction"), true}};
        loopPath = QDBusConnection::systemBus().call(call, QDBus::Block, 30000).arguments().value(0).value<QDBusObjectPath>().path();
        backing.close();
    }
    report(!loopPath.isEmpty(), QStringLiteral("Make a Windows USB: a loop device stands in for the stick"));
    if (loopPath.isEmpty())
        return;
    UDisks udisks;
    udisks.setInteractive(false);
    const Disk *stick = nullptr;
    waitUntil([&] {
        udisks.refresh();
        stick = udisks.diskByPath(loopPath);
        return stick != nullptr;
    }, 15000);
    const QString device = stick ? stick->device : QString();

    WindowsUsbDialog::allowLoopDevicesForTest = true;
    const quint64 splitFrom = windowsusb::splitFrom;
    if (realIso.isEmpty()) {
        windowsusb::splitFrom = 1024 * 1024;
        WindowsUsbJob::splitMiB = 1;
    }
    QStringList boxes;
    Answerer answerer;
    answerer.answer = [&](QWidget *modal) {
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            boxes << box->text();
            box->accept();
            return true;
        }
        return false;
    };
    {
        WindowsUsbDialog dialog(&udisks, loopPath, nullptr, iso);
        dialog.show();
        auto *info = dialog.findChild<QLabel *>(QStringLiteral("isoInfo"));
        waitUntil([&] { return info->text().contains(QLatin1String("Windows 11")) || info->text().contains(QLatin1String("isn't")); }, 30000);
        report(info->text().contains(QLatin1String("Windows 11")) && info->text().contains(QLatin1String("split")),
               QStringLiteral("the ISO is opened and read: the edition, and that install.wim gets split"), info->text().left(160));
        dialog.findChild<QCheckBox *>(QStringLiteral("localUser"))->setChecked(true);
        dialog.findChild<QLineEdit *>(QStringLiteral("userName"))->setText(QStringLiteral("Tester"));
        // The stick list is filled in once the ISO is open.
        waitUntil([&] {
            auto *combo = dialog.findChild<QComboBox *>();
            return combo && combo->findData(loopPath) >= 0;
        }, 10000);
        if (auto *combo = dialog.findChild<QComboBox *>())
            combo->setCurrentIndex(combo->findData(loopPath));
        dialog.findChild<QLineEdit *>(QStringLiteral("confirm"))->setText(shortDevice(device));
        QPushButton *make = findButton(&dialog, QStringLiteral("Make the Windows USB"));
        report(make && make->isEnabled(), QStringLiteral("typing the device name unlocks the button"));
        if (make)
            make->click();
        waitUntil([&] { return !dialog.isVisible() || !boxes.isEmpty(); }, 3600000);
        waitUntil([&] { return !dialog.isVisible(); }, 15000);
        report(boxes.size() == 1 && boxes[0].startsWith(QLatin1String("The Windows USB is ready")), QStringLiteral("it says the stick is ready"),
               boxes.join(QStringLiteral(" | ")).left(300));
    }
    windowsusb::splitFrom = splitFrom;
    WindowsUsbJob::splitMiB = windowsusb::kSplitMiB;
    WindowsUsbDialog::allowLoopDevicesForTest = false;

    udisks.refresh();
    bool isoStillOpen = false;
    for (const Disk &d : udisks.disks())
        isoStillOpen = isoStillOpen || d.backingFile == iso;
    report(!isoStillOpen, QStringLiteral("the ISO is closed again"));

    // A dialog that goes without being closed (DiskForge quitting with it open) closes it too.
    auto isoOpen = [&] {
        udisks.refresh();
        for (const Disk &d : udisks.disks()) {
            if (d.backingFile == iso)
                return true;
        }
        return false;
    };
    {
        auto *dialog = new WindowsUsbDialog(&udisks, QString(), nullptr, iso);
        dialog->show();
        auto *info = dialog->findChild<QLabel *>(QStringLiteral("isoInfo"));
        waitUntil([&] { return info->text().contains(QLatin1String("Windows 11")); }, 30000);
        report(isoOpen(), QStringLiteral("opened again, the ISO is open"));
        delete dialog;
    }
    waitUntil([&] { return !isoOpen(); }, 15000);
    report(!isoOpen(), QStringLiteral("deleting the dialog without closing it closes the ISO too"));

    stick = udisks.diskByPath(loopPath);
    const bool oneFat = stick && stick->volumes.size() == 1 && stick->volumes.first().fsType == QLatin1String("vfat");
    QString root;
    if (oneFat) {
        udisks.mount(stick->volumes.first());
        waitUntil([&] {
            udisks.refresh();
            const Disk *d = udisks.diskByPath(loopPath);
            root = d && !d->volumes.isEmpty() ? d->volumes.first().mountPoints.value(0) : QString();
            return !root.isEmpty();
        }, 15000);
    }
    const QStringList parts = QDir(root + QStringLiteral("/sources")).entryList({QStringLiteral("install*.swm")}, QDir::Files);
    report(oneFat && QFileInfo::exists(root + QStringLiteral("/bootmgr.efi"))
               && !QFileInfo::exists(root + QStringLiteral("/sources/install.wim")) && parts.size() >= 2, // a file inside never spans parts
           QStringLiteral("FAT32 with the ISO's files, install.wim as split parts instead"), parts.join(QLatin1Char(' ')));
    const int verified = sh(QStringLiteral("wimlib-imagex"), {QStringLiteral("verify"), root + QStringLiteral("/sources/install.swm"),
                                                              QStringLiteral("--ref=%1/sources/install*.swm").arg(root)});
    report(verified == 0, QStringLiteral("and the parts check out"));
    QFile answers(root + QStringLiteral("/autounattend.xml"));
    const QByteArray xml = answers.open(QIODevice::ReadOnly) ? answers.readAll() : QByteArray();
    report(xml.contains("BypassTPMCheck") && xml.contains("<Name>Tester</Name>"), QStringLiteral("autounattend.xml has the options"));
    answers.close();
    cleanUpLoop(udisks, loopPath);
}

// The home screen (DiskForge Live's desktop, and Tools → Quick Fixes on a PC): which tiles are there,
// what each one starts, the ask before restarting into the firmware, and reading Secure Boot.
void homeScreen()
{
    QTemporaryDir dir;
    UDisks udisks;
    QList<QStringList> started;
    const auto realLaunch = HomeWindow::launch;
    HomeWindow::launch = [&started](const QString &program, const QStringList &args) {
        started << (QStringList{program} + args);
        return true;
    };
    const QString apps = dir.filePath(QStringLiteral("applications"));
    QDir().mkpath(apps);
    auto desktopFile = [&apps](const char *id, const QByteArray &exec) {
        QFile f(apps + QLatin1Char('/') + QLatin1String(id) + QStringLiteral(".desktop"));
        if (f.open(QIODevice::WriteOnly))
            f.write("[Desktop Entry]\nName=x\n[Other]\nExec=wrong\n[Desktop Entry]\nExec=" + exec + "\n");
    };
    desktopFile("diskforge-live-testdisk", "qterminal -e sudo testdisk %f");
    desktopFile("diskforge-live-firmware", "systemctl reboot --firmware-setup");
    desktopFile("qterminal", "qterminal");
    HomeWindow::applicationDirs = {apps};
    auto ids = [](const HomeWindow &home) {
        QStringList out;
        for (const HomeWindow::Tile &t : home.tiles())
            out << t.id;
        return out;
    };

    {
        HomeWindow fixes(&udisks, HomeWindow::Mode::Window);
        const QStringList tiles = ids(fixes);
        report(!tiles.contains(QLatin1String("drives")) && !tiles.contains(QLatin1String("terminal")) && !tiles.contains(QLatin1String("web"))
                   && tiles.contains(QLatin1String("writeimage")) && tiles.contains(QLatin1String("recover")),
               QStringLiteral("Quick Fixes: the tools, without the stick's programs or what isn't installed"), tiles.join(QLatin1Char(' ')));
        auto *copy = fixes.findChild<QAbstractButton *>(QStringLiteral("tile-copystick"));
        report(copy && copy->text() == QLatin1String("Make a DiskForge Live USB"), QStringLiteral("on a PC the stick tile makes a DiskForge Live USB"),
               copy ? copy->text() : QString());
        started.clear();
        if (auto *write = fixes.findChild<QAbstractButton *>(QStringLiteral("tile-writeimage")))
            write->click();
        report(started.size() == 1 && started[0].mid(1) == QStringList{QStringLiteral("--open"), QStringLiteral("write-image")}
                   && started[0][0] == QCoreApplication::applicationFilePath(),
               QStringLiteral("a tool tile starts DiskForge with that tool"), started.value(0).join(QLatin1Char(' ')));
    }
    {
        HomeWindow home(&udisks, HomeWindow::Mode::Desktop);
        const QStringList tiles = ids(home);
        report(tiles.contains(QLatin1String("drives")) && tiles.contains(QLatin1String("terminal")) && tiles.contains(QLatin1String("firmware"))
                   && !tiles.contains(QLatin1String("web")),
               QStringLiteral("the home screen: DiskForge, the tools and the programs that are there"), tiles.join(QLatin1Char(' ')));
        started.clear();
        if (auto *testdisk = home.findChild<QAbstractButton *>(QStringLiteral("tile-testdisk")))
            testdisk->click();
        report(started.size() == 1 && started[0] == QStringList{QStringLiteral("qterminal"), QStringLiteral("-e"), QStringLiteral("sudo"), QStringLiteral("testdisk")},
               QStringLiteral("a program tile runs the Exec line of its launcher, without %f"), started.value(0).join(QLatin1Char(' ')));
        started.clear();
        if (auto *recover = home.findChild<QAbstractButton *>(QStringLiteral("tile-recover")))
            recover->click();
        report(started.size() == 1 && started[0].mid(1) == QStringList{QStringLiteral("--open"), QStringLiteral("lost-files")},
               QStringLiteral("Get Files Back opens Find Lost Files"), started.value(0).join(QLatin1Char(' ')));

        // Restarting into the firmware asks first: No starts nothing, Yes restarts.
        for (const bool yes : {false, true}) {
            started.clear();
            Answerer answerer;
            answerer.answer = [yes](QWidget *modal) {
                if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                    box->button(yes ? QMessageBox::Yes : QMessageBox::No)->click();
                    return true;
                }
                return false;
            };
            if (auto *firmware = home.findChild<QAbstractButton *>(QStringLiteral("tile-firmware")))
                firmware->click();
            report(yes ? started.size() == 1 && started[0].value(1) == QLatin1String("reboot") : started.isEmpty(),
                   yes ? QStringLiteral("Firmware Settings restarts after Yes") : QStringLiteral("and does nothing after No"),
                   started.value(0).join(QLatin1Char(' ')));
        }
    }

    // Secure Boot from the firmware variable: 4 bytes of attributes, then the value.
    const QString efi = dir.filePath(QStringLiteral("efi"));
    QDir().mkpath(efi + QStringLiteral("/efivars"));
    auto secureBootVar = [&efi](const QByteArray &data) {
        QFile f(efi + QStringLiteral("/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c"));
        if (f.open(QIODevice::WriteOnly))
            f.write(data);
    };
    const bool noVar = HomeWindow::secureBoot(efi) == HomeWindow::SecureBoot::Unknown;
    // Firmware without Secure Boot: other variables, but not that one.
    QFile other(efi + QStringLiteral("/efivars/BootCurrent-8be4df61-93ca-11d2-aa0d-00e098032b8c"));
    if (other.open(QIODevice::WriteOnly))
        other.write(QByteArray("\x06\x00\x00\x00\x01\x00", 6));
    other.close();
    const bool noSecureBoot = HomeWindow::secureBoot(efi) == HomeWindow::SecureBoot::Off;
    secureBootVar(QByteArray("\x06\x00\x00\x00\x01", 5));
    const bool on = HomeWindow::secureBoot(efi) == HomeWindow::SecureBoot::On;
    secureBootVar(QByteArray("\x06\x00\x00\x00\x00", 5));
    const bool off = HomeWindow::secureBoot(efi) == HomeWindow::SecureBoot::Off;
    const bool bios = HomeWindow::secureBoot(dir.filePath(QStringLiteral("no-efi"))) == HomeWindow::SecureBoot::Bios;
    report(noVar && noSecureBoot && on && off && bios,
           QStringLiteral("Secure Boot is read right: on, off, firmware without it, BIOS, can't tell"));

    HomeWindow::applicationDirs.clear();
    HomeWindow::launch = realLaunch;
}

// Find Lost Files through its window: a disk image with pictures hidden in random bytes, looked
// through, a picture previewed, everything ticked and saved, and the saved files compared. Then
// saving onto the drive being looked through is refused.
// A FAT32 image with deleted files (tests/data/lostfs): they show with their names, under their
// folders, and the deleted folder's files are found by picking it in the tree.
void lostFilesFromFileSystem()
{
    QTemporaryDir dir;
    QString error;
    const auto image = ImageSource::open(QStringLiteral(SOURCE_DIR "/tests/data/lostfs/fat32.img.zst"), &error);
    const QString path = dir.filePath(QStringLiteral("stick.img"));
    QFile out(path);
    if (!image || !out.open(QIODevice::WriteOnly)) {
        report(false, QStringLiteral("unpack the FAT32 test image"), error);
        return;
    }
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    for (qint64 n; (n = image->read(buffer.data(), buffer.size())) > 0;)
        out.write(buffer.constData(), n);
    out.close();

    UDisks udisks;
    LostFilesDialog dialog(&udisks, QString());
    dialog.show();
    dialog.scanImage(path);
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("status"));
    waitUntil([&] { return status->text().startsWith(QLatin1String("Done")); }, 60000);
    auto *view = dialog.findChild<QListView *>(QStringLiteral("files"));
    report(status->text().contains(QLatin1String("found 6 deleted files")) && status->text().contains(QLatin1String("2 that weren't deleted"))
               && view->model()->rowCount() == 6,
           QStringLiteral("Find Lost Files: a FAT32 stick's deleted files are listed by name"), status->text());

    auto *folders = dialog.findChild<QTreeWidget *>(QStringLiteral("folders"));
    QTreeWidgetItem *oldStuff = nullptr;
    for (QTreeWidgetItemIterator it(folders); *it; ++it) {
        if ((*it)->data(0, Qt::UserRole).toString() == QLatin1String("TESTFAT32/Old stuff"))
            oldStuff = *it;
    }
    report(oldStuff && oldStuff->text(0) == QLatin1String("Old stuff (2)"), QStringLiteral("the deleted folder is in the folder tree"),
           oldStuff ? oldStuff->text(0) : QStringLiteral("missing"));
    if (!oldStuff)
        return;
    folders->setCurrentItem(oldStuff);
    QStringList names;
    for (int r = 0; r < view->model()->rowCount(); ++r)
        names << view->model()->index(r, 0).data().toString();
    names.sort();
    report(names == QStringList{QStringLiteral("letter.pdf"), QStringLiteral("scan.png")}, QStringLiteral("picking it shows what was in it"),
           names.join(QStringLiteral(", ")));

    view->setCurrentIndex(view->model()->index(names.indexOf(QStringLiteral("scan.png")) == 0 ? 0 : 1, 0));
    for (int r = 0; r < view->model()->rowCount(); ++r) {
        if (view->model()->index(r, 0).data().toString() == QLatin1String("scan.png"))
            view->setCurrentIndex(view->model()->index(r, 0));
    }
    auto *details = dialog.findChild<QLabel *>(QStringLiteral("details"));
    report(details->text().contains(QLatin1String("Was in TESTFAT32/Old stuff")) && details->text().contains(QLatin1String("should open fine")),
           QStringLiteral("its details say where it was and that it's whole"), details->text().left(160));

    // Unticking "Only deleted files" shows the two that are still there too.
    auto *deletedOnly = dialog.findChild<QCheckBox *>(QStringLiteral("deletedOnly"));
    folders->setCurrentItem(folders->topLevelItem(0));
    deletedOnly->setChecked(false);
    report(view->model()->rowCount() == 8, QStringLiteral("and the files that weren't deleted show when asked"),
           QString::number(view->model()->rowCount()));
}

void lostFilesThroughWindow()
{
    QTemporaryDir dir;
    QVector<QPair<quint64, QByteArray>> placed;
    QByteArray data(16 * 1024 * 1024, Qt::Uninitialized);
    QRandomGenerator rng(2026);
    for (qsizetype i = 0; i + 4 <= data.size(); i += 4)
        *reinterpret_cast<quint32 *>(data.data() + i) = rng.generate();
    quint64 at = 64 * 1024;
    for (const char *format : {"PNG", "JPG", "PNG"}) {
        QImage image(240, 160, QImage::Format_RGB32);
        image.fill(QColor(0x00, 0xe5, 0xff));
        QPainter p(&image);
        p.setBrush(QColor(0xff, 0x33, 0x55));
        p.drawEllipse(QRect(20, 20, 100 + int(placed.size()) * 20, 100));
        p.end();
        QByteArray file;
        QBuffer buffer(&file);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, format);
        data.replace(qsizetype(at), file.size(), file);
        placed << qMakePair(at, file);
        at = (at + quint64(file.size()) + 100 * 1024) / 512 * 512;
    }
    const QString imagePath = dir.filePath(QStringLiteral("lost.img"));
    QFile imageFile(imagePath);
    if (!imageFile.open(QIODevice::WriteOnly) || imageFile.write(data) != data.size()) {
        report(false, QStringLiteral("write the test image"));
        return;
    }
    imageFile.close();

    UDisks udisks;
    LostFilesDialog dialog(&udisks, QString());
    dialog.show();
    dialog.scanImage(imagePath);
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("status"));
    waitUntil([&] { return status->text().startsWith(QLatin1String("Done")); }, 60000);
    auto *view = dialog.findChild<QListView *>(QStringLiteral("files"));
    report(status->text().contains(QLatin1String("found 3 files")) && view && view->model()->rowCount() == 3,
           QStringLiteral("Find Lost Files: the three pictures in the image are found"), status->text());

    view->setCurrentIndex(view->model()->index(0, 0));
    auto *preview = dialog.findChild<QLabel *>(QStringLiteral("preview"));
    waitUntil([&] { return !preview->pixmap().isNull(); }, 10000);
    report(!preview->pixmap().isNull() && preview->pixmap().width() == 240, QStringLiteral("the picture shows in the preview"),
           QStringLiteral("%1 wide").arg(preview->pixmap().width()));
    auto *details = dialog.findChild<QLabel *>(QStringLiteral("details"));
    report(details->text().contains(QLatin1String("should open fine")), QStringLiteral("and the details say it's whole"));

    QPushButton *tickAll = findButton(&dialog, QStringLiteral("Tick All Shown"));
    if (tickAll)
        tickAll->click();
    auto *ticked = dialog.findChild<QLabel *>(QStringLiteral("ticked"));
    report(ticked->text().startsWith(QLatin1String("3 files ticked")), QStringLiteral("ticking them all"), ticked->text());
    const QString saveTo = dir.filePath(QStringLiteral("saved"));
    QDir().mkpath(saveTo);
    // /tmp is often in memory: then it warns, and "No" saves there anyway.
    QStringList warnings;
    {
        Answerer answerer;
        answerer.answer = [&warnings](QWidget *modal) {
            if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                warnings << box->text();
                box->button(QMessageBox::No) ? box->button(QMessageBox::No)->click() : box->accept();
                return true;
            }
            return false;
        };
        dialog.saveTo(saveTo);
    }
    const QByteArray fs = QStorageInfo(saveTo).fileSystemType();
    report(fs != "tmpfs" || (warnings.size() == 1 && warnings[0].contains(QLatin1String("computer's memory"))),
           QStringLiteral("a folder in memory gets a warning first"), warnings.join(QStringLiteral(" | ")).left(100));
    auto *saveStatus = dialog.findChild<QLabel *>(QStringLiteral("saveStatus"));
    waitUntil([&] { return saveStatus->text().startsWith(QLatin1String("Saved")); }, 30000);
    int same = 0;
    QDirIterator it(saveTo, QDir::Files, QDirIterator::Subdirectories);
    QSet<QByteArray> wanted;
    for (const auto &p : std::as_const(placed))
        wanted.insert(QCryptographicHash::hash(p.second, QCryptographicHash::Sha256));
    while (it.hasNext()) {
        QFile f(it.next());
        if (f.fileName().endsWith(QLatin1String(".txt")) || !f.open(QIODevice::ReadOnly))
            continue;
        same += wanted.contains(QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256));
    }
    report(saveStatus->text().startsWith(QLatin1String("Saved 3 files")) && same == 3, QStringLiteral("all three saved, byte for byte"),
           saveStatus->text());

    // The same, as if the files were on the drive a folder in the home folder is on (/tmp can be
    // in memory): saving there is refused.
    QTemporaryDir onDisk(QDir::homePath() + QStringLiteral("/.cache/diskforge-uitest-XXXXXX"));
    QString rootDisk;
    for (const Disk &d : udisks.disks()) {
        if (blockio::pathIsOnDisk(onDisk.path(), d.device))
            rootDisk = d.device;
    }
    if (rootDisk.isEmpty()) {
        out << "SKIP  refusing the source drive: the temporary folder's drive isn't listed" << Qt::endl;
        return;
    }
    QStringList boxes;
    Answerer answerer;
    answerer.answer = [&](QWidget *modal) {
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            boxes << box->text();
            box->accept();
            return true;
        }
        return false;
    };
    dialog.scanSource(lost::Source::fromMemory(data), QStringLiteral("a drive"), rootDisk);
    waitUntil([&] { return status->text().startsWith(QLatin1String("Done")); }, 60000);
    if (tickAll)
        tickAll->click();
    const QString again = onDisk.filePath(QStringLiteral("again"));
    QDir().mkpath(again);
    dialog.saveTo(again);
    QCoreApplication::processEvents();
    report(boxes.size() == 1 && boxes[0].contains(QLatin1String("on the drive you're getting the files back from"))
               && QDir(again).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty(),
           QStringLiteral("saving onto the drive the files are on is refused"), boxes.join(QStringLiteral(" | ")).left(120));
    dialog.close();
}

// Enter in a USB dialog goes to Close or Cancel, never to the button that erases the stick, and
// not to Browse either, even after Browse had the focus (it used to open the file picker).
void enterKey()
{
    UDisks udisks;
    udisks.setInteractive(false);
    QStringList pickers;
    Answerer answerer;
    answerer.answer = [&](QWidget *modal) {
        if (auto *picker = qobject_cast<QFileDialog *>(modal)) {
            pickers << picker->windowTitle();
            picker->reject();
            return true;
        }
        return false;
    };
    auto check = [&](QDialog &dialog, const QString &name) {
        pickers.clear();
        dialog.show();
        QCoreApplication::processEvents();
        QPushButton *browse = findButton(&dialog, QStringLiteral("Browse…"));
        auto *confirm = dialog.findChild<QLineEdit *>(QStringLiteral("confirm"));
        if (!browse || !confirm) {
            report(false, QStringLiteral("%1: Browse and the name field are there").arg(name));
            return;
        }
        browse->setFocus();
        QCoreApplication::processEvents();
        confirm->setVisible(true);
        confirm->setFocus();
        QCoreApplication::processEvents();
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QCoreApplication::sendEvent(confirm, &press);
        waitUntil([&] { return !dialog.isVisible(); }, 5000);
        report(pickers.isEmpty() && !dialog.isVisible(), QStringLiteral("%1: Enter closes it, it doesn't open the file picker").arg(name),
               pickers.join(QStringLiteral(", ")));
    };
    {
        RescueUsbDialog dialog(&udisks, QString());
        check(dialog, QStringLiteral("Make a DiskForge Live USB"));
    }
    {
        WindowsUsbDialog dialog(&udisks, QString(), nullptr, QStringLiteral("/nonexistent/Win11.iso"));
        check(dialog, QStringLiteral("Make a Windows USB"));
    }
    {
        WriteImageDialog dialog(&udisks, QString());
        check(dialog, QStringLiteral("Write Image to USB"));
    }
}

// In DiskForge Live the few things that only change the running system are off and say why;
// the USB tools and everything else stay on.
void rescueMode()
{
    const QString realConf = rescue::rescueConfPath;
    QTemporaryDir dir;
    rescue::rescueConfPath = dir.filePath(QStringLiteral("diskforge-live.conf"));
    UDisks udisks;
    const QStringList offInRescue = {QStringLiteral("Mount at Startup"), QStringLiteral("Disk Cleanup"), QStringLiteral("Btrfs Snapshots")};
    const QStringList alwaysOn = {QStringLiteral("Write Image to USB"), QStringLiteral("Make a Windows USB"),
                                  QStringLiteral("Make a DiskForge Live USB"), QStringLiteral("Check a USB Stick"),
                                  QStringLiteral("Open Disk Image"), QStringLiteral("Optimize")};

    auto check = [&](bool inRescue) {
        const QString where = inRescue ? QStringLiteral("in the rescue") : QStringLiteral("on a normal PC");
        MainWindow window(&udisks);
        window.show();
        if (!udisks.disks().isEmpty())
            window.selectDevice(udisks.disks().constFirst().device);
        QCoreApplication::processEvents();
        for (const QString &name : offInRescue) {
            QAction *a = findAction(window, name);
            const bool saysWhy = a && a->toolTip() == rescue::notInRescueReason();
            if (inRescue)
                report(a && !a->isEnabled() && saysWhy, QStringLiteral("%1 is off %2 and says why").arg(name, where), a ? a->toolTip() : QStringLiteral("no such action"));
            else
                report(a && !saysWhy, QStringLiteral("%1 doesn't mention the rescue %2").arg(name, where));
        }
        if (!inRescue) {
            QAction *cleanup = findAction(window, QStringLiteral("Disk Cleanup"));
            report(cleanup && cleanup->isEnabled(), QStringLiteral("Disk Cleanup is on %1").arg(where));
        }
        for (const QString &name : alwaysOn) {
            QAction *a = findAction(window, name);
            report(a && a->isEnabled(), QStringLiteral("%1 is on %2").arg(name, where));
        }
        OptimizeDialog optimize(&udisks);
        QCheckBox *weekly = nullptr;
        for (QCheckBox *c : optimize.findChildren<QCheckBox *>()) {
            if (c->text() == QLatin1String("Optimize every week"))
                weekly = c;
        }
        if (inRescue)
            report(weekly && !weekly->isEnabled() && weekly->toolTip() == rescue::notInRescueReason(),
                   QStringLiteral("the weekly TRIM is off %1 and says why").arg(where));
        else
            report(weekly && weekly->toolTip() != rescue::notInRescueReason(), QStringLiteral("the weekly TRIM doesn't mention the rescue %1").arg(where));
    };

    report(!rescue::runningInRescue(), QStringLiteral("no rescue conf, not the rescue"));
    check(false);
    QFile conf(rescue::rescueConfPath);
    if (!conf.open(QIODevice::WriteOnly) || conf.write("VERSION=test\nBUILD_ID=uitest\n") < 0) {
        report(false, QStringLiteral("write a test rescue conf"), conf.errorString());
        rescue::rescueConfPath = realConf;
        return;
    }
    conf.close();
    report(rescue::runningInRescue(), QStringLiteral("with the rescue conf, the rescue"));
    check(true);
    rescue::rescueConfPath = realConf;
}

int userScenarios()
{
    QTemporaryDir home;
    qputenv("XDG_DATA_HOME", QFile::encodeName(home.filePath(QStringLiteral("data"))));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(home.filePath(QStringLiteral("config"))));
    outputWindow();
    addonMenus();
    themes();
    maker();
    banner();
    jobBars();
    rescueUsb();
    stickCheck();
    isoCopy();
    windowsUsb();
    rescueMode();
    enterKey();
    homeScreen();
    lostFilesThroughWindow();
    lostFilesFromFileSystem();
    out << (failures ? QStringLiteral("%1 step(s) failed").arg(failures) : QStringLiteral("All steps passed")) << Qt::endl;
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // As the user: the add-on, menu and theme parts, which need no test disks.
    if (app.arguments().contains(QStringLiteral("--user")))
        return userScenarios();
    if (geteuid() != 0) {
        out << "Run as root: it creates a loop device (or use --user for the parts that don't)." << Qt::endl;
        return 2;
    }

    // A disk image with one ext4 partition whose root folder has a wrong link count.
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("damaged.img"));
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("128M"), image});
    QProcess sfdisk;
    sfdisk.start(QStringLiteral("sfdisk"), {QStringLiteral("-q"), image});
    sfdisk.waitForStarted();
    sfdisk.write("label: gpt\n,,L\n");
    sfdisk.closeWriteChannel();
    sfdisk.waitForFinished();
    QString loop;
    sh(QStringLiteral("losetup"), {QStringLiteral("-fP"), QStringLiteral("--show"), image}, &loop);
    const QString part = loop + QStringLiteral("p1");
    QThread::msleep(500);
    sh(QStringLiteral("mkfs.ext4"), {QStringLiteral("-q"), QStringLiteral("-L"), QStringLiteral("DAMAGED"), part});
    sh(QStringLiteral("debugfs"), {QStringLiteral("-w"), QStringLiteral("-R"), QStringLiteral("set_inode_field <2> links_count 7"), part});
    report(sh(QStringLiteral("e2fsck"), {QStringLiteral("-fn"), part}) == 4, QStringLiteral("test partition has real file system errors"), part);

    UDisks udisks;
    udisks.setInteractive(false);
    MainWindow window(&udisks);
    window.show();
    waitUntil([&] {
        udisks.refresh();
        return window.selectDevice(part);
    }, 15000);
    report(window.selectDevice(part), QStringLiteral("select it in the window"));

    QStringList messages;
    bool askedToRepair = false;
    QObject::connect(&udisks, &UDisks::operationFinished, [&](bool, const QString &m) { messages << m; });

    // Answer the dialogs the way a user would, refreshing the disk list first.
    QTimer clicker;
    QObject::connect(&clicker, &QTimer::timeout, [&] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box)
            return;
        if (box->text().contains(QLatin1String("Repair them now"))) {
            askedToRepair = true;
            for (int i = 0; i < 3; ++i)
                udisks.refresh(); // replaces the disk list while the question is open
            box->button(QMessageBox::Yes)->click();
        } else {
            messages << QStringLiteral("dialog: ") + box->text();
            box->close();
        }
    });
    clicker.start(100);

    QAction *check = nullptr;
    for (QAction *a : window.findChildren<QAction *>()) {
        if (a->text().remove(QLatin1Char('&')).startsWith(QLatin1String("Check for Errors")))
            check = a;
    }
    report(check && check->isEnabled(), QStringLiteral("Check for Errors is available"));
    if (check)
        check->trigger();

    waitUntil([&] {
        return std::any_of(messages.cbegin(), messages.cend(), [](const QString &m) {
            return m.contains(QLatin1String("epair")) || m.contains(QLatin1String("not found")) || m.contains(QLatin1String("isn't there"));
        });
    }, 60000);
    clicker.stop();

    report(askedToRepair, QStringLiteral("Check found the errors and asked to repair"));
    const bool repaired = std::any_of(messages.cbegin(), messages.cend(), [](const QString &m) { return m.startsWith(QLatin1String("Repaired")); });
    report(repaired, QStringLiteral("repair reached the partition after the refresh"), messages.join(QStringLiteral(" | ")));
    report(sh(QStringLiteral("e2fsck"), {QStringLiteral("-fn"), part}) == 0, QStringLiteral("the file system is clean now"));

    sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});

    systemMenus(window, udisks);
    cloneThroughWindow(window, udisks, dir);
    backupAndRestore(window, udisks, dir);
    stopWipe();
    typeAndFlags();
    lockInMap();
    inspector();
    recoverThroughWindow();
    scanThroughWindow();
    raidBanner();
    stickCheck();

    out << (failures ? QStringLiteral("%1 step(s) failed").arg(failures) : QStringLiteral("All steps passed")) << Qt::endl;
    return failures ? 1 : 0;
}
