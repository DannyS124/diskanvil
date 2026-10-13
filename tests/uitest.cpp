// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Drives the real window, answering its dialogs the way a user would:
// - Check for Errors finds damage, the disk list refreshes while "Repair them now?" is
//   open, and the repair still has to reach the right partition.
// - The right-click menus of the system disk offer nothing that writes to it.
// - Clone Drive, with the disk list refreshed while its dialog is open.
// - Back Up a partition, damage it, Restore it, and it's back exactly.
// sudo QT_QPA_PLATFORM=offscreen build/diskanvil-uitest

#include "../src/drives/writeblock.h"
#include "../src/app/oldname.h"
#include "../src/viruses/wof.h"
#include "../src/tools/remotehelp.h"
#include "../src/tools/remotehelpdialog.h"
#include "../src/usb/liveonpc.h"
#include "../src/usb/liveonpcdialog.h"
#include "../src/tools/netboot.h"
#include "../src/tools/netbootdialog.h"
#include "../src/addons/addonoutput.h"
#include "../src/addons/addonform.h"
#include "../src/addons/addonmaker.h"
#include "../src/app/diskmap.h"
#include "../src/app/homewindow.h"
#include "../src/drives/blockio.h"
#include "../src/usb/imagesource.h"
#include "../src/lostfiles/enhance.h"
#include "../src/lostfiles/viewer.h"
#include "../src/lostfiles/hexview.h"
#include "../src/lostfiles/structure.h"
#include "../src/lostfiles/btrfsdialog.h"
#include "../src/app/whathappened.h"
#include "../src/lostfiles/lostfilesdialog.h"
#include "../src/lostfiles/timelinedialog.h"
#include "../src/lostfiles/placesdialog.h"
#include "../src/viruses/activitydialog.h"
#include "../src/lostfiles/filetypes.h"
#include "../src/common/jobui.h"
#include "../src/lostfiles/preview.h"
#include "../src/lostfiles/previewloader.h"
#include "../src/drives/inspectdialog.h"
#include "../src/usb/isomode.h"
#include "../src/usb/filecopy.h"
#include "../src/drives/recoverdialog.h"
#include "../src/viruses/reghive.h"
#include "../src/usb/rescuestick.h"
#include "../src/usb/rescueusbdialog.h"
#include "../src/usb/stickcheckdialog.h"
#include "../src/app/noticebar.h"
#include "../src/drives/typedialog.h"
#include "../src/viruses/virusdialog.h"
#include "../src/viruses/windowspicker.h"
#include "../src/viruses/productkeydialog.h"
#include "../src/viruses/driversdialog.h"
#include "../src/app/certificates.h"
#include "../src/common/pdfreport.h"
#include "../src/viruses/bootentriesdialog.h"
#include "../src/backup/clonezilladialog.h"
#include "../src/backup/evidencedialog.h"
#include "../src/backup/imagebackup.h"
#include "../src/backup/savefilesdialog.h"
#include "../src/tools/linuxbootdialog.h"
#include "../src/tools/tryrepairdialog.h"
#include "../src/tools/testboot.h"
#include "../src/tools/testbootdialog.h"
#include "../src/backup/newids.h"
#include "../src/backup/newidsdialog.h"
#include "../src/viruses/hashlookup.h"
#include "../src/viruses/efivars.h"
#include "testsign.h"
#include "../src/usb/windowsusbjob.h"
#include "../src/usb/windowsusbdialog.h"
#include "slowdisk.h"
#include "../src/addons/addons.h"
#include "../src/addons/addonsdialog.h"
#include "../src/backup/copydialogs.h"
#include "../src/common/format.h"
#include "../src/app/mainwindow.h"
#include "../src/tools/systemtools.h"
#include "../src/app/theme.h"
#include "../src/app/tools.h"
#include "../src/drives/udisks.h"
#include "../src/app/translations.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QGuiApplication>
#include <QCheckBox>
#include <QStorageInfo>
#include <QBuffer>
#include <QCommandLinkButton>
#include <QSettings>
#include <QPainter>
#include <QPdfWriter>
#include <QPlainTextEdit>
#include <QThread>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTcpServer>
#include <QListView>
#include <QTabWidget>
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
#include <QAbstractScrollArea>
#include <QDropEvent>
#include <QMimeData>
#include <QInputDialog>
#include <QMouseEvent>
#include <QListWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QShortcut>
#include <QMenuBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTimeZone>
#include <QTableView>
#include <QRadioButton>
#include <QElapsedTimer>
#include <QLayout>
#include <QMessageBox>
#include <QProcess>
#include <QRandomGenerator>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QEventLoop>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QScreen>
#include <QStackedWidget>
#include <QSplitter>

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
        // Pinned add-on actions carry their add-on id and label; they're never DiskAnvil's own.
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
            if (e->placeholderText() != shortDevice(target))
                continue;
            // Typed a letter at a time, the box keeps the keyboard (hidden for a moment, it lost it
            // to Cancel, and a space in "Disk 4" pressed it).
            e->setFocus();
            const QString name = shortDevice(target);
            bool kept = true;
            for (int n = 1; n <= name.size(); ++n) {
                e->setText(name.left(n));
                kept = kept && dialog->focusWidget() == e;
            }
            report(kept, QStringLiteral("typing the drive's name keeps the keyboard in the box"));
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
        // A length no other test uses, so only this command's sleeps are counted.
        std::unique_ptr<AddonOutputWindow> w(run(QStringLiteral("sleep 61.7 & echo started; sleep 61.7")));
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
        const qint64 stopped = timer.elapsed();
        QByteArray left;
        waitUntil([&] {
            pids.start(QStringLiteral("pgrep"), {QStringLiteral("-f"), QStringLiteral("^sleep 61.7$")});
            pids.waitForFinished();
            left = pids.readAllStandardOutput().trimmed();
            return left.isEmpty();
        }, 3000);
        report(!w->isRunning() && stopped < 5000 && left.isEmpty(),
               QStringLiteral("Stop ends the command and everything it started"), QString::number(stopped) + QStringLiteral(" ms ") + QString::fromLatin1(left));
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
    report(warnings.size() == 2 && warnings.value(0).contains(QLatin1String("DiskAnvil's own")) && warnings.value(1).contains(QLatin1String("Ctrl"))
               && Addons::shortcut(keyOf(QStringLiteral("Look Around"))) == QLatin1String("Ctrl+Alt+K"),
           QStringLiteral("the Add-ons window refuses a shortcut DiskAnvil uses, or one without Ctrl/Alt"), warnings.join(QStringLiteral(" / ")));
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
    luks.write("diskanvil-test");
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
            input->setTextValue(QStringLiteral("diskanvil-test"));
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

// Back Up Encryption Header and Put Back Encryption Header, through the main window, on a LUKS
// partition of a loop device: the copy is cryptsetup's own bytes, and after the header is
// wrecked (the partition no longer shows as LUKS) it's put back and opens with its passphrase.
void luksHeaderThroughWindow()
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
    const auto cryptsetup = [](const QStringList &args, const QByteArray &input = {}) {
        QProcess p;
        p.start(QStringLiteral("cryptsetup"), args);
        p.waitForStarted();
        p.write(input);
        p.closeWriteChannel();
        p.waitForFinished(60000);
        return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    };
    report(cryptsetup({QStringLiteral("luksFormat"), QStringLiteral("--batch-mode"), QStringLiteral("--type"), QStringLiteral("luks2"), QStringLiteral("--pbkdf"),
                       QStringLiteral("pbkdf2"), QStringLiteral("--pbkdf-force-iterations"), QStringLiteral("1000"), QStringLiteral("--key-file=-"), part},
                      "header test")
               == 0,
           QStringLiteral("LUKS header: a LUKS test partition"));
    const QString theirs = dir.filePath(QStringLiteral("theirs.img"));
    cryptsetup({QStringLiteral("luksHeaderBackup"), part, QStringLiteral("--header-backup-file"), theirs});
    const QString ours = dir.filePath(QStringLiteral("ours.img"));

    UDisks udisks;
    udisks.setInteractive(false);
    MainWindow window(&udisks);
    window.resize(1200, 800);
    window.show();
    const auto volume = [&]() -> const Volume * {
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
        return volume() && volume()->fsType == QLatin1String("crypto_LUKS");
    }, 15000);
    QString pickFile;
    QStringList boxes;
    Answerer answerer;
    answerer.answer = [&](QWidget *modal) {
        if (auto *picker = qobject_cast<QFileDialog *>(modal)) {
            picker->selectFile(pickFile);
            if (auto *name = picker->findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
                name->setText(pickFile);
            static_cast<QDialog *>(picker)->accept();
            return true;
        }
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            boxes << box->text();
            box->accept();
            return true;
        }
        if (auto *confirm = modal->findChild<QLineEdit *>(QStringLiteral("confirm")); confirm && qobject_cast<QDialog *>(modal)) {
            boxes << modal->findChild<QLabel *>()->text();
            confirm->setText(shortDevice(part));
            static_cast<QDialog *>(modal)->accept();
            return true;
        }
        return false;
    };
    const auto file = [](const QString &path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    window.selectDevice(part);
    auto *backup = window.findChild<QAction *>(QStringLiteral("luksBackup"));
    report(backup && backup->isEnabled(), QStringLiteral("LUKS header: Back Up Encryption Header is offered on the LUKS partition"));
    pickFile = ours;
    if (backup)
        backup->trigger();
    waitUntil([&] { return !boxes.isEmpty(); }, 30000);
    report(!file(ours).isEmpty() && file(ours) == file(theirs) && boxes.join(QLatin1Char(' ')).contains(QLatin1String("Keep it safe")),
           QStringLiteral("LUKS header: the copy is the same bytes as cryptsetup's, and it says to keep it safe"), boxes.join(QStringLiteral(" | ")).left(200));

    // Wrecked: the partition's first MiB zeroed; it no longer shows as LUKS.
    {
        QFile device(part);
        if (device.open(QIODevice::ReadWrite)) {
            device.write(QByteArray(1024 * 1024, '\0'));
            device.close();
        }
    }
    sh(QStringLiteral("udevadm"), {QStringLiteral("settle")});
    waitUntil([&] {
        udisks.refresh();
        return volume() && volume()->fsType.isEmpty();
    }, 15000);
    window.selectDevice(part);
    auto *restore = window.findChild<QAction *>(QStringLiteral("luksRestore"));
    report(volume() && volume()->fsType.isEmpty() && restore && restore->isEnabled(),
           QStringLiteral("LUKS header: wrecked, it shows as unknown, and Put Back is still offered"));
    boxes.clear();
    pickFile = ours;
    if (restore)
        restore->trigger();
    waitUntil([&] { return boxes.size() >= 2; }, 30000);
    report(boxes.value(0).contains(QLatin1String("damaged or gone")) && boxes.value(1).contains(QLatin1String("is back on")),
           QStringLiteral("LUKS header: it says the header is gone, then puts the copy back"), boxes.join(QStringLiteral(" | ")).left(300));
    report(cryptsetup({QStringLiteral("open"), QStringLiteral("--test-passphrase"), part, QStringLiteral("--key-file"), QStringLiteral("-")}, "header test") == 0,
           QStringLiteral("LUKS header: and the partition opens with its passphrase again"));
    sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
}

// Try the Repair First, as root, on a loop device holding an ext4 with a real error: the repair
// fixes the stand-in, the partition itself still has its error, and throwing it away leaves
// nothing behind (no device-mapper device, no loop device, no file).
void tryRepairFirst()
{
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("broken.img"));
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("64M"), image});
    sh(QStringLiteral("mkfs.ext4"), {QStringLiteral("-q"), image});
    sh(QStringLiteral("debugfs"), {QStringLiteral("-w"), QStringLiteral("-R"), QStringLiteral("set_inode_field <2> links_count 7"), image});
    QString loop;
    sh(QStringLiteral("losetup"), {QStringLiteral("-f"), QStringLiteral("--show"), image}, &loop);
    loop = loop.trimmed();
    report(sh(QStringLiteral("e2fsck"), {QStringLiteral("-fn"), loop}) == 4, QStringLiteral("try first: a partition with a real error"));
    {
        TryRepairDialog dialog(loop, QStringLiteral("ext4"), QStringLiteral("broken (test)"));
        dialog.show();
        dialog.findChild<QPushButton *>(QStringLiteral("try"))->click();
        auto *status = dialog.findChild<QLabel *>(QStringLiteral("status"));
        waitUntil([&] { return status->text().startsWith(QLatin1String("The repair ran")) || status->text().contains(QLatin1String("couldn't")); }, 60000);
        report(status->text().startsWith(QLatin1String("The repair ran")) && dialog.findChild<QPlainTextEdit *>(QStringLiteral("output"))->toPlainText().contains(QLatin1String("MODIFIED")),
               QStringLiteral("try first: the repair runs on the stand-in"), status->text().left(160));
        report(sh(QStringLiteral("e2fsck"), {QStringLiteral("-fn"), loop}) == 4 && sh(QStringLiteral("e2fsck"), {QStringLiteral("-fn"), QStringLiteral("/dev/mapper/diskanvil-try")}) == 0,
               QStringLiteral("try first: the stand-in is repaired, the partition itself untouched"));
        findButton(&dialog, QStringLiteral("Throw It Away"))->click();
    }
    QString mapped;
    sh(QStringLiteral("dmsetup"), {QStringLiteral("ls")}, &mapped);
    report(!mapped.contains(QLatin1String("diskanvil-try")) && !QFile::exists(QStringLiteral("/tmp/diskanvil-try.changes")),
           QStringLiteral("try first: thrown away, nothing is left behind"), mapped.trimmed());
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

// A NAS's drives plugged into this PC: two loop devices holding a RAID 1 named the way a
// Synology names its arrays, with LVM and ext4 on it, stopped. The window says they're here and
// not opened; Open Read-Only assembles the array read-only and switches LVM on, and the file on
// it can be read.
void nasDrives()
{
    QTemporaryDir dir;
    QStringList loops;
    for (const char *name : {"nas1.img", "nas2.img"}) {
        const QString image = dir.filePath(QLatin1String(name));
        sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("96M"), image});
        QString loop;
        sh(QStringLiteral("losetup"), {QStringLiteral("-f"), QStringLiteral("--show"), image}, &loop);
        loops << loop.trimmed();
    }
    const QString md = QStringLiteral("/dev/md/dfnas");
    const int created = sh(QStringLiteral("mdadm"), {QStringLiteral("--create"), md, QStringLiteral("--level=1"), QStringLiteral("--raid-devices=2"),
                                                     QStringLiteral("--metadata=1.2"), QStringLiteral("--homehost=DiskStation"), QStringLiteral("--name=2"),
                                                     QStringLiteral("--assume-clean"), QStringLiteral("--run"), QStringLiteral("--quiet"), loops.value(0), loops.value(1)});
    sh(QStringLiteral("pvcreate"), {QStringLiteral("-q"), md});
    sh(QStringLiteral("vgcreate"), {QStringLiteral("-q"), QStringLiteral("vgdfnas"), md});
    sh(QStringLiteral("lvcreate"), {QStringLiteral("-q"), QStringLiteral("-l"), QStringLiteral("100%FREE"), QStringLiteral("-n"), QStringLiteral("volume_1"), QStringLiteral("vgdfnas")});
    const QString lv = QStringLiteral("/dev/vgdfnas/volume_1");
    sh(QStringLiteral("mkfs.ext4"), {QStringLiteral("-q"), lv});
    const QString mnt = dir.filePath(QStringLiteral("mnt"));
    QDir().mkpath(mnt);
    sh(QStringLiteral("mount"), {lv, mnt});
    {
        QFile f(mnt + QStringLiteral("/photos.txt"));
        if (f.open(QIODevice::WriteOnly))
            f.write("on the NAS\n");
    }
    sh(QStringLiteral("umount"), {mnt});
    sh(QStringLiteral("vgchange"), {QStringLiteral("-q"), QStringLiteral("-an"), QStringLiteral("vgdfnas")});
    sh(QStringLiteral("mdadm"), {QStringLiteral("--stop"), md});
    sh(QStringLiteral("udevadm"), {QStringLiteral("settle")});
    report(created == 0, QStringLiteral("NAS: a stopped RAID 1 with LVM and ext4 stands in for a NAS's drives"));

    QStringList boxes;
    {
        UDisks udisks;
        udisks.setInteractive(false);
        MainWindow window(&udisks);
        window.show();
        NoticeBar *bar = nullptr;
        waitUntil([&] {
            udisks.refresh();
            for (NoticeBar *b : window.findChildren<NoticeBar *>(QStringLiteral("closed"))) {
                if (!b->isHidden() && b->text().contains(QLatin1String("DiskStation:2")))
                    bar = b;
            }
            return bar != nullptr;
        }, 20000);
        report(bar != nullptr, QStringLiteral("NAS: a bar says its drives are here and not opened"), bar ? bar->text() : QString());
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
        if (bar) {
            if (QPushButton *open = findButton(bar, QStringLiteral("Open Read-Only…")))
                open->click();
        }
        waitUntil([&] { return boxes.size() >= 2; }, 120000);
    }
    report(boxes.value(1).startsWith(QLatin1String("Opened")), QStringLiteral("NAS: Open Read-Only opens it"), boxes.join(QStringLiteral(" | ")).left(300));
    // Opened by mdadm's scan, the array has the name it was made with: found through its drive.
    const QStringList holders = QDir(QStringLiteral("/sys/block/%1/holders").arg(QFileInfo(loops.value(0)).fileName())).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    const QString array = holders.value(0);
    QString state;
    {
        QFile f(QStringLiteral("/sys/block/%1/md/array_state").arg(array));
        if (f.open(QIODevice::ReadOnly))
            state = QString::fromLatin1(f.readAll()).trimmed();
    }
    sh(QStringLiteral("mount"), {QStringLiteral("-o"), QStringLiteral("ro"), lv, mnt});
    QByteArray read;
    {
        QFile photos(mnt + QStringLiteral("/photos.txt"));
        if (photos.open(QIODevice::ReadOnly))
            read = photos.readAll();
    }
    report(state == QLatin1String("readonly") && read == "on the NAS\n", QStringLiteral("NAS: the array is read-only, and the file on it reads back"),
           array + QStringLiteral(" ") + state + QStringLiteral(" | ") + QString::fromLatin1(read));
    sh(QStringLiteral("umount"), {mnt});
    sh(QStringLiteral("vgchange"), {QStringLiteral("-q"), QStringLiteral("-an"), QStringLiteral("vgdfnas")});
    sh(QStringLiteral("mdadm"), {QStringLiteral("--stop"), array.isEmpty() ? md : QStringLiteral("/dev/") + array});
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

// Make a DiskAnvil Live USB writes the boot code for BIOS PCs into the stick's image file instead
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

// Inside DiskAnvil Live, Make a DiskAnvil Live USB copies the stick it runs from. Here the stick
// made a moment ago (mounted at `sourceRoot`, build ID "test") stands in for it, with a fake
// rescue conf: the dialog finds it by its build ID, won't copy it onto itself, and copies it
// onto a second stick with every file checked.
void copyRunningStick(UDisks &udisks, const QString &sourceLoop, const QString &sourceRoot, const QTemporaryDir &dir)
{
    const QString realConf = rescue::rescueConfPath;
    rescue::rescueConfPath = dir.filePath(QStringLiteral("diskanvil-live.conf"));
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
        QPushButton *make = findButton(&dialog, QStringLiteral("Make the DiskAnvil Live USB"));
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
        report(boxes.size() == 1 && boxes[0].startsWith(QLatin1String("The DiskAnvil Live USB is ready")), QStringLiteral("the copy is ready"),
               boxes.join(QStringLiteral(" | ")));
    }
    RescueUsbDialog::openStickForTest = nullptr;
    const QString root = mountStick(udisks, loopPath, QStringLiteral("DISKANVIL"));
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

// Make a DiskAnvil Live USB on a loop device of the user's own (UDisks lets you set those up and
// change them without a password), through the dialog like a user would.
// DISKANVIL_TEST_RESCUE_ISO uses a real rescue image instead of a small made-up one, and with
// it DISKANVIL_TEST_STICK keeps the stick's image file afterwards, to boot it in a VM.
void rescueUsb()
{
    QTemporaryDir dir;
    QString iso = qEnvironmentVariable("DISKANVIL_TEST_RESCUE_ISO");
    if (iso.isEmpty()) {
        const QString tree = dir.filePath(QStringLiteral("tree"));
        QMap<QString, QByteArray> files;
        files[QStringLiteral(".disk/diskanvil-live")] = "DiskAnvil Live\nversion=0.0.1\nbuilt=2026-10-09\nid=test\n";
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
            out << "SKIP  Make a DiskAnvil Live USB: xorriso isn't installed" << Qt::endl;
            return;
        }
    }
    // Only with a real image: the other scenarios would make their own sticks over it.
    QString stickImage = qEnvironmentVariable("DISKANVIL_TEST_RESCUE_ISO").isEmpty() ? QString() : qEnvironmentVariable("DISKANVIL_TEST_STICK");
    if (stickImage.isEmpty())
        stickImage = dir.filePath(QStringLiteral("stick.img"));
    const bool big = QFileInfo(iso).size() > 100 * 1024 * 1024;
    QFile::remove(stickImage);
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), big ? QStringLiteral("3G") : QStringLiteral("600M"), stickImage});

    const QString loopPath = userLoop(stickImage);
    report(!loopPath.isEmpty(), QStringLiteral("Make a DiskAnvil Live USB: a loop device stands in for the stick"));
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
        QPushButton *make = findButton(&dialog, QStringLiteral("Make the DiskAnvil Live USB"));
        image->setText(iso);
        report(info->text().contains(QLatin1String("DiskAnvil Live")), QStringLiteral("the dialog reads the image"), info->text());
        report(make && !make->isEnabled(), QStringLiteral("nothing happens before the device name is typed"));
        confirm->setText(shortDevice(device));
        report(make && make->isEnabled(), QStringLiteral("typing it unlocks the button"));
        if (make)
            make->click();
        waitUntil([&] { return !dialog.isVisible() || boxes.size() > 0; }, big ? 900000 : 120000);
        waitUntil([&] { return !dialog.isVisible(); }, 5000);
        report(boxes.size() == 1 && boxes[0].startsWith(QLatin1String("The DiskAnvil Live USB is ready")), QStringLiteral("it reports the stick ready"),
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
            if (v.label == QLatin1String("DISKANVIL"))
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
        const QString root = mountStick(udisks, loopPath, QStringLiteral("DISKANVIL"));
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
        report(stickInfo && stickInfo->text().contains(QLatin1String("has DiskAnvil Live")) && openLogs && openLogs->isVisibleTo(&again),
               QStringLiteral("opened again, it says the stick has DiskAnvil Live and offers its logs"), stickInfo ? stickInfo->text() : QString());
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

// Windows Drivers and Services on a pretend Windows (the test hives, and our own tiny program as
// its driver): the driver shows its maker from its version resource, newest first, and
// switching it off sets it to Disabled in the SYSTEM hive, with a backup.
void driversWindow()
{
    QTemporaryDir dir;
    const QString data = QStringLiteral(SOURCE_DIR "/tests/data/");
    const QString root = dir.filePath(QStringLiteral("drive"));
    const auto put = [](const QString &path, const QByteArray &bytes) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
    };
    const auto read = [](const QString &path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    put(root + QStringLiteral("/Windows/System32/config/SOFTWARE"), read(data + QStringLiteral("registry/software.hiv")));
    put(root + QStringLiteral("/Windows/System32/config/SYSTEM"), read(data + QStringLiteral("registry/system.hiv")));
    put(root + QStringLiteral("/Windows/System32/drivers/drv.sys"), read(data + QStringLiteral("pe/versioned.exe")));
    put(root + QStringLiteral("/Users/Public/svc.exe"), "MZ");
    put(root + QStringLiteral("/Windows/System32/goodsvc.dll"), "MZ");
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
    DriversDialog dialog(root);
    dialog.show();
    auto *list = dialog.findChild<QTreeWidget *>(QStringLiteral("drivers"));
    waitUntil([&] { return list && list->topLevelItemCount() > 0; }, 30000);
    QTreeWidgetItem *drv = nullptr;
    QStringList names;
    for (int i = 0; list && i < list->topLevelItemCount(); ++i) {
        names << list->topLevelItem(i)->text(0) + QLatin1Char('/') + list->topLevelItem(i)->text(1);
        if (list->topLevelItem(i)->text(0) == QLatin1String("Drv"))
            drv = list->topLevelItem(i);
    }
    report(drv && drv->text(1) == QLatin1String("DiskAnvil Test Company") && drv->text(3) == QLatin1String("Driver") && names.size() >= 3,
           QStringLiteral("drivers: listed with who made each, from its version resource"), names.join(QStringLiteral(", ")));
    if (drv) {
        list->clearSelection();
        drv->setSelected(true);
        if (auto *off = dialog.findChild<QPushButton *>(QStringLiteral("switchOff")))
            off->click();
    }
    waitUntil([&] { return boxes.size() >= 2; }, 10000);
    reg::Hive system;
    QString error;
    system.open(root + QStringLiteral("/Windows/System32/config/SYSTEM"), &error);
    const auto start = system.value(system.key(QStringLiteral("ControlSet001\\Services\\Drv")), QStringLiteral("Start"));
    const QStringList backups = QDir(root + QStringLiteral("/DiskAnvil backups")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    report(start && start->number() == 4 && backups.size() == 1 && boxes.value(1).startsWith(QLatin1String("Switched off 1")),
           QStringLiteral("drivers: switched off, Disabled in the SYSTEM hive, with a backup"), boxes.join(QStringLiteral(" | ")).left(300));
}

// Boot Menu Entries on pretend firmware variables (efivarfs' files in a folder): the entries in
// their order, then moving one up, starting one next time and removing one, as efibootmgr's
// commands (captured, never run), and the report.
void bootMenuWindow()
{
    QTemporaryDir vars;
    const auto put = [&vars](const QString &name, const QByteArray &value) {
        QFile f(vars.filePath(name + QStringLiteral("-") + QLatin1String(efi::kGlobal)));
        return f.open(QIODevice::WriteOnly) && f.write(QByteArray("\x07\x00\x00\x00", 4) + value) > 0;
    };
    put(QStringLiteral("Boot0000"), efiboot::loadOption(QStringLiteral("Windows Boot Manager"), 1, 2048, 204800, QStringLiteral("11111111-2222-3333-4444-555555555555"),
                                                        QStringLiteral("\\EFI\\Microsoft\\Boot\\bootmgfw.efi")));
    put(QStringLiteral("Boot0003"), efiboot::loadOption(QStringLiteral("debian"), 1, 2048, 204800, QStringLiteral("11111111-2222-3333-4444-555555555555"),
                                                        QStringLiteral("\\EFI\\debian\\shimx64.efi")));
    put(QStringLiteral("BootOrder"), efiboot::orderValue({0, 3}));
    put(QStringLiteral("BootCurrent"), efiboot::orderValue({0}));
    const QString realVars = efi::varsDir;
    efi::varsDir = vars.path();
    QStringList ran;
    BootEntriesDialog::runForTest = [&ran](const QStringList &command, QString *) {
        ran << command.join(QLatin1Char(' '));
        return true;
    };
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
    UDisks udisks;
    udisks.setInteractive(false);
    {
        BootEntriesDialog dialog(&udisks);
        dialog.show();
        auto *list = dialog.findChild<QTreeWidget *>(QStringLiteral("entries"));
        QStringList names;
        for (int i = 0; list && i < list->topLevelItemCount(); ++i)
            names << list->topLevelItem(i)->text(0) + QLatin1Char('/') + list->topLevelItem(i)->text(3);
        report(names == QStringList({QStringLiteral("Windows Boot Manager/On, started this time"), QStringLiteral("debian/On")})
                   && list->topLevelItem(0)->text(2).contains(QLatin1String("isn't here")),
               QStringLiteral("boot menu: the entries in order, the one started this time, and a missing partition said"), names.join(QStringLiteral(" | ")));
        list->setCurrentItem(list->topLevelItem(1));
        findButton(&dialog, QStringLiteral("Up"))->click();
        dialog.findChild<QPushButton *>(QStringLiteral("saveOrder"))->click();
        list->setCurrentItem(list->topLevelItem(1));
        dialog.findChild<QPushButton *>(QStringLiteral("next"))->click();
        list->setCurrentItem(list->topLevelItem(0));
        dialog.findChild<QPushButton *>(QStringLiteral("remove"))->click();
        report(ran == QStringList({QStringLiteral("efibootmgr -o 0003,0000"), QStringLiteral("efibootmgr -n 0003"), QStringLiteral("efibootmgr -b 0000 -B")})
                   && boxes.value(0).contains(QLatin1String("started from this time")),
               QStringLiteral("boot menu: the order, next time and removing, as efibootmgr's commands; removing the one in use is said"),
               ran.join(QStringLiteral(" | ")) + QStringLiteral(" || ") + boxes.join(QStringLiteral(" | ")).left(200));
        const QString text = BootEntriesDialog::report(&udisks, efiboot::read());
        report(text.contains(QLatin1String("Firmware: UEFI.")) && text.contains(QLatin1String("Boot0003 debian: \\EFI\\debian\\shimx64.efi")),
               QStringLiteral("boot menu: the report says how the PC starts"), text.left(400));
    }
    BootEntriesDialog::runForTest = nullptr;
    efi::varsDir = realVars;
}

// The text of a PDF, as pdftotext reads it.
QString pdfText(const QString &pdf)
{
    QProcess p;
    p.start(QStringLiteral("pdftotext"), {QStringLiteral("-layout"), pdf, QStringLiteral("-")});
    p.waitForFinished(30000);
    // The fonts join some letters (fi, fl): read as the letters.
    return QString::fromUtf8(p.readAllStandardOutput()).replace(QChar(0xFB01), QStringLiteral("fi")).replace(QChar(0xFB02), QStringLiteral("fl"));
}

// A certificate of erasure from a record (no drive needed), saved as a PDF whose text says what
// was erased, how, and for whom.
void certificateWindow()
{
    QTemporaryDir dir;
    const QString realStore = erasure::storePath;
    erasure::storePath = dir.filePath(QStringLiteral("erasures.json"));
    erasure::Record r;
    r.model = QStringLiteral("WDC WD10EZEX-08WN4A0");
    r.serial = QStringLiteral("WD-WCC6Y0TEST");
    r.device = QStringLiteral("/dev/sdz");
    r.size = 1000204886016;
    r.method = QStringLiteral("Every sector written with zeros (Wipe Disk)");
    r.zeros = true;
    r.started = QDateTime(QDate(2026, 10, 11), QTime(9, 0));
    r.finished = QDateTime(QDate(2026, 10, 11), QTime(11, 32));
    r.app = QStringLiteral("DiskAnvil test");
    erasure::remember(r);
    const QVector<erasure::Record> all = erasure::all();
    report(all.size() == 1 && all[0].id.size() == 12 && all[0].serial == r.serial, QStringLiteral("certificate: the erasure is remembered, with a number of its own"));
    const QString pdf = dir.filePath(QStringLiteral("cert.pdf"));
    UDisks udisks;
    udisks.setInteractive(false);
    Answerer answerer;
    answerer.answer = [&](QWidget *modal) {
        if (auto *picker = qobject_cast<QFileDialog *>(modal)) {
            picker->selectFile(pdf);
            if (auto *name = picker->findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
                name->setText(pdf);
            static_cast<QDialog *>(picker)->accept();
            return true;
        }
        return false;
    };
    {
        CertificateDialog dialog(&udisks, all.value(0));
        dialog.show();
        dialog.findChild<QLineEdit *>(QStringLiteral("by"))->setText(QStringLiteral("Sam Tester"));
        dialog.findChild<QLineEdit *>(QStringLiteral("for"))->setText(QStringLiteral("Order 4711"));
        report(!dialog.findChild<QPushButton *>(QStringLiteral("check"))->isEnabled(), QStringLiteral("certificate: Check It waits for the drive to be connected"));
        dialog.findChild<QPushButton *>(QStringLiteral("save"))->click();
    }
    const QString text = pdfText(pdf);
    if (qEnvironmentVariableIsSet("DISKANVIL_UITEST_SHOTS"))
        QFile::copy(pdf, qEnvironmentVariable("DISKANVIL_UITEST_SHOTS") + QStringLiteral("/certificate.pdf"));
    report(text.contains(QLatin1String("Certificate of Erasure")) && text.contains(QLatin1String("WD-WCC6Y0TEST")) && text.contains(QLatin1String("Order 4711"))
               && text.contains(QLatin1String("Sam Tester")) && text.contains(all.value(0).id) && text.contains(QLatin1String("Wipe Disk")),
           QStringLiteral("certificate: the PDF says what was erased, how, for whom and by whom"), text.left(300));
    erasure::storePath = realStore;
}

// As root: Wipe Disk on a loop device full of data. The erasure is recorded, Check It reads it
// back as zeros, and the certificate says so.
void wipeCertificate()
{
    QTemporaryDir dir;
    const QString realStore = erasure::storePath;
    erasure::storePath = dir.filePath(QStringLiteral("erasures.json"));
    const QString image = dir.filePath(QStringLiteral("old.img"));
    {
        QFile f(image);
        if (f.open(QIODevice::WriteOnly)) {
            QByteArray block(1024 * 1024, 'x');
            for (int i = 0; i < 32; ++i)
                f.write(block);
        }
    }
    QString loop;
    sh(QStringLiteral("losetup"), {QStringLiteral("-f"), QStringLiteral("--show"), image}, &loop);
    loop = loop.trimmed();
    UDisks udisks;
    udisks.setInteractive(false);
    const Disk *disk = nullptr;
    waitUntil([&] {
        udisks.refresh();
        for (const Disk &d : udisks.disks()) {
            if (d.device == loop)
                disk = &d;
        }
        return disk != nullptr;
    }, 15000);
    erasure::Record got;
    QObject::connect(&udisks, &UDisks::erased, [&got](const erasure::Record &r) {
        got = r;
        erasure::remember(r);
    });
    if (disk)
        udisks.wipe(*disk);
    waitUntil([&] { return !got.method.isEmpty(); }, 60000);
    report(got.zeros && got.device == loop && got.finished >= got.started && got.size == 32 * 1024 * 1024,
           QStringLiteral("wipe: the finished erasure is announced with the drive and the times"), got.method);
    {
        CertificateDialog dialog(&udisks, erasure::all().value(0));
        dialog.show();
        auto *check = dialog.findChild<QPushButton *>(QStringLiteral("check"));
        report(check->isEnabled(), QStringLiteral("wipe: Check It is there with the drive connected"));
        check->click();
        auto *summary = dialog.findChild<QLabel *>(QStringLiteral("summary"));
        waitUntil([&] { return summary->text().contains(QLatin1String("Read back")); }, 30000);
        report(summary->text().contains(QLatin1String("all zeros")), QStringLiteral("wipe: read back, it's all zeros"), summary->text().right(160));
    }
    report(erasure::all().value(0).checked.contains(QLatin1String("all zeros")), QStringLiteral("wipe: the check is kept with the record"));
    sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
    erasure::storePath = realStore;
}

// The drive report from Disk Health, for a drive of this PC's that reports SMART (read only):
// the PDF has the drive, its verdict and its attributes.
void driveReport()
{
    UDisks udisks;
    udisks.setInteractive(false);
    const Disk *drive = nullptr;
    for (const Disk &d : udisks.disks()) {
        if (!d.isLoop && d.health.state != Health::State::Unknown && !d.health.nvme)
            drive = &d;
    }
    if (!drive) {
        report(true, QStringLiteral("drive report: skipped, no drive here reports SMART"));
        return;
    }
    QTemporaryDir dir;
    HealthDialog dialog(&udisks, drive->blockPath);
    dialog.show();
    QApplication::processEvents();
    const QString pdf = dir.filePath(QStringLiteral("report.pdf"));
    QString error;
    const bool saved = pdfreport::save(dialog.reportHtml(), pdf, QStringLiteral("Drive Report"), &error);
    const QString text = pdfText(pdf);
    report(saved && text.contains(QLatin1String("Drive Report")) && text.contains(drive->serial) && text.contains(QLatin1String("Temperature"))
               && text.contains(QLatin1String("power-on-hours")),
           QStringLiteral("drive report: the PDF has the drive, its verdict, its facts and its SMART attributes"), error + text.left(200));
    if (qEnvironmentVariableIsSet("DISKANVIL_UITEST_SHOTS"))
        QFile::copy(pdf, qEnvironmentVariable("DISKANVIL_UITEST_SHOTS") + QStringLiteral("/report.pdf"));
}

// Make an Evidence Image of a loop device: as root, the .E01 checks out against its own MD5 and
// the report next to it has the case; as the user, reading the drive needs the admin password,
// so without a prompt it's refused and said.
void evidenceWindow()
{
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("seized.img"));
    {
        QFile f(image);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QByteArray("evidence data ").repeated(40000));
            f.resize(4 * 1024 * 1024);
        }
    }
    QFile backing(image);
    QString loopPath;
    if (backing.open(QIODevice::ReadWrite)) {
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.UDisks2"), QStringLiteral("/org/freedesktop/UDisks2/Manager"),
                                                           QStringLiteral("org.freedesktop.UDisks2.Manager"), QStringLiteral("LoopSetup"));
        call << QVariant::fromValue(QDBusUnixFileDescriptor(backing.handle())) << QVariantMap{{QStringLiteral("auth.no_user_interaction"), true}};
        loopPath = QDBusConnection::systemBus().call(call, QDBus::Block, 30000).arguments().value(0).value<QDBusObjectPath>().path();
        backing.close();
    }
    UDisks udisks;
    udisks.setInteractive(false);
    const Disk *disk = nullptr;
    waitUntil([&] {
        udisks.refresh();
        disk = udisks.diskByPath(loopPath);
        return disk != nullptr;
    }, 15000);
    report(disk != nullptr, QStringLiteral("evidence: a loop device holds the drive"));
    if (!disk)
        return;
    const QString e01 = dir.filePath(QStringLiteral("seized.E01"));
    {
        EvidenceDialog dialog(&udisks, *disk, nullptr);
        dialog.show();
        auto *start = dialog.findChild<QPushButton *>(QStringLiteral("start"));
        report(!start->isEnabled(), QStringLiteral("evidence: nothing starts without a case number"));
        dialog.findChild<QLineEdit *>(QStringLiteral("case"))->setText(QStringLiteral("2026-0042"));
        dialog.findChild<QLineEdit *>(QStringLiteral("evidence"))->setText(QStringLiteral("ITEM-3"));
        dialog.findChild<QLineEdit *>(QStringLiteral("examiner"))->setText(QStringLiteral("Sam Tester"));
        dialog.findChild<QLineEdit *>(QStringLiteral("file"))->setText(e01);
        start->click();
        auto *status = dialog.findChild<QLabel *>(QStringLiteral("status"));
        waitUntil([&] { return status->text().startsWith(QLatin1String("Done")) || status->text().contains(QLatin1String("couldn")); }, 60000);
        if (geteuid() == 0) {
            report(status->text().startsWith(QLatin1String("Done")) && status->text().contains(QLatin1String("matches")), QStringLiteral("evidence: made and checked"),
                   status->text().left(200));
            const QString text = pdfText(dir.filePath(QStringLiteral("seized report.pdf")));
            report(ewf::verify(e01).isEmpty() && text.contains(QLatin1String("2026-0042")) && text.contains(QLatin1String("Read back from the file")),
                   QStringLiteral("evidence: the E01 checks out, and the report next to it has the case and the check"), text.left(200));
        } else {
            report(status->text().contains(QLatin1String("be opened to read it")), QStringLiteral("evidence: as the user without a password prompt, it's refused and said"),
                   status->text());
        }
    }
    cleanUpLoop(udisks, loopPath);
}

// Windows Product Key on this PC (no MSDM table, no Windows mounted): it says so in plain words,
// and Look in Another Windows asks which.
void productKeyWindow()
{
    UDisks udisks;
    udisks.setInteractive(false);
    QStringList asked;
    Answerer answerer;
    answerer.answer = [&](QWidget *modal) {
        if (auto *picker = qobject_cast<WindowsPicker *>(modal)) {
            asked << picker->windowTitle();
            picker->reject();
            return true;
        }
        return false;
    };
    ProductKeyDialog dialog(&udisks);
    dialog.show();
    QApplication::processEvents();
    auto *firmware = dialog.findChild<QLabel *>(QStringLiteral("firmwareKey"));
    const bool msdm = QFile::exists(QStringLiteral("/sys/firmware/acpi/tables/MSDM"));
    report(firmware && (msdm ? !firmware->text().isEmpty() : firmware->text().startsWith(QLatin1String("None"))),
           QStringLiteral("product key: the firmware's key, or why there's none"), firmware ? firmware->text() : QString());
    if (QPushButton *look = findButton(&dialog, QStringLiteral("Look in Another Windows…")))
        look->click();
    waitUntil([&] { return !asked.isEmpty(); }, 5000);
    report(asked.value(0) == QLatin1String("Which Windows?"), QStringLiteral("product key: Look in Another Windows asks which"));
    if (qEnvironmentVariableIsSet("DISKANVIL_UITEST_SHOTS"))
        dialog.grab().save(qEnvironmentVariable("DISKANVIL_UITEST_SHOTS") + QStringLiteral("/productkey.png"));
}

// Unlocking as VeraCrypt: the window's answers become UDisks' unlock options, with the types its
// D-Bus call wants (pim a uint32, hidden and system booleans, keyfiles a list of paths).
void veraCryptWindow()
{
    VeraCryptDialog dialog(QStringLiteral("sdb2"));
    dialog.show();
    QPushButton *unlock = findButton(&dialog, QStringLiteral("Unlock"));
    report(unlock && !unlock->isEnabled(), QStringLiteral("VeraCrypt: nothing to unlock with before a password or keyfile"));
    dialog.findChild<QLineEdit *>(QStringLiteral("password"))->setText(QStringLiteral("hunter2"));
    dialog.findChild<QSpinBox *>(QStringLiteral("pim"))->setValue(485);
    for (QCheckBox *box : dialog.findChildren<QCheckBox *>()) {
        if (box->text().startsWith(QLatin1String("A hidden volume")))
            box->setChecked(true);
    }
    const QVariantMap o = dialog.options();
    report(unlock && unlock->isEnabled() && dialog.password() == QLatin1String("hunter2") && o.value(QStringLiteral("pim")).metaType().id() == QMetaType::UInt
               && o.value(QStringLiteral("pim")).toUInt() == 485 && o.value(QStringLiteral("hidden")) == QVariant(true) && !o.contains(QStringLiteral("system"))
               && !o.contains(QStringLiteral("keyfiles")),
           QStringLiteral("VeraCrypt: password, PIM and hidden volume go to UDisks as its unlock options want them"));
    Volume v;
    v.encrypted = true;
    v.fsType = QStringLiteral("crypto_unknown");
    const QString unknown = volumeStatus(v);
    v.fsType = QStringLiteral("crypto_TCRYPT");
    report(unknown.contains(QLatin1String("maybe VeraCrypt")) && volumeStatus(v).startsWith(QLatin1String("VeraCrypt")),
           QStringLiteral("VeraCrypt: what UDisks can't tell is said to be maybe VeraCrypt"), unknown);
}

// Change the Sector Size: the sizes offered, the type-to-confirm, and the exact hdparm command,
// captured instead of run (nothing reaches a drive).
void sectorSizeWindow()
{
    UDisks udisks;
    udisks.setInteractive(false);
    Disk disk;
    disk.device = QStringLiteral("/dev/sdz");
    disk.blockPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/sdz");
    disk.model = QStringLiteral("Test drive");
    disk.size = 4000ull * 1000 * 1000 * 1000;
    drivepower::SectorSizes sizes;
    sizes.logical = 512;
    sizes.physical = 4096;
    sizes.supported = {512, 4096};
    QStringList ran;
    SectorSizeDialog::runForTest = [&ran](const QStringList &command, QString *) {
        ran = command;
        return true;
    };
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
        SectorSizeDialog dialog(&udisks, disk, sizes);
        dialog.show();
        auto *size = dialog.findChild<QComboBox *>(QStringLiteral("sectorSize"));
        QPushButton *change = findButton(&dialog, QStringLiteral("Change"));
        report(size && size->count() == 1 && size->currentData().toInt() == 4096 && change && !change->isEnabled(),
               QStringLiteral("sector size: only the other size is offered, and nothing starts before the name is typed"));
        dialog.findChild<QLineEdit *>(QStringLiteral("confirm"))->setText(QStringLiteral("sdz"));
        if (change && change->isEnabled())
            change->click();
        waitUntil([&] { return !boxes.isEmpty(); }, 10000);
    }
    report(ran.join(QLatin1Char(' ')) == QLatin1String("sudo -n hdparm --set-sector-size 4096 --please-destroy-my-drive /dev/sdz")
               && boxes.join(QLatin1Char(' ')).contains(QLatin1String("4096-byte sectors")),
           QStringLiteral("sector size: hdparm is asked to switch it, and it says what to do next"), ran.join(QLatin1Char(' ')) + QStringLiteral(" | ") + boxes.join(QLatin1Char(' ')));
    disk.isSystem = true;
    {
        SectorSizeDialog dialog(&udisks, disk, sizes);
        dialog.findChild<QLineEdit *>(QStringLiteral("confirm"))->setText(QStringLiteral("sdz"));
        QPushButton *change = findButton(&dialog, QStringLiteral("Change"));
        report(change && !change->isEnabled(), QStringLiteral("sector size: never on the drive the system runs from"));
    }
    SectorSizeDialog::runForTest = nullptr;
}

// Refresh Slow Areas on a loop device standing in for an SSD, after a scan whose reads are
// given: two slow pieces. As root they're written back and the image is byte for byte the same;
// as the user, opening the drive for writing is refused and the window says so.
void slowAreaRefresh()
{
    const bool asRoot = geteuid() == 0;
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("ssd.img"));
    {
        QFile f(image);
        if (f.open(QIODevice::WriteOnly)) {
            QByteArray block(1024 * 1024, '\0');
            for (int m = 0; m < 32; ++m) {
                for (qsizetype i = 0; i < block.size(); ++i)
                    block[i] = char((i * 7 + m * 13) & 0xFF);
                f.write(block);
            }
        }
    }
    const auto digest = [&image] {
        QFile f(image);
        return f.open(QIODevice::ReadOnly) ? QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256) : QByteArray();
    };
    const QByteArray before = digest();
    QFile backing(image);
    QString loopPath;
    if (backing.open(QIODevice::ReadWrite)) {
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.UDisks2"), QStringLiteral("/org/freedesktop/UDisks2/Manager"),
                                                           QStringLiteral("org.freedesktop.UDisks2.Manager"), QStringLiteral("LoopSetup"));
        call << QVariant::fromValue(QDBusUnixFileDescriptor(backing.handle())) << QVariantMap{{QStringLiteral("auth.no_user_interaction"), true}};
        loopPath = QDBusConnection::systemBus().call(call, QDBus::Block, 30000).arguments().value(0).value<QDBusObjectPath>().path();
        backing.close();
    }
    report(!loopPath.isEmpty(), QStringLiteral("SSD refresh: a loop device stands in for the SSD"));
    if (loopPath.isEmpty())
        return;
    UDisks udisks;
    udisks.setInteractive(false);
    const Disk *ssd = nullptr;
    waitUntil([&] {
        udisks.refresh();
        ssd = udisks.diskByPath(loopPath);
        return ssd != nullptr;
    }, 15000);
    if (!ssd)
        return;
    QStringList boxes;
    Answerer answerer;
    answerer.answer = [&](QWidget *modal) {
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            boxes << box->text();
            for (QAbstractButton *b : box->buttons()) {
                if (b->text() == QLatin1String("Refresh")) {
                    b->click();
                    return true;
                }
            }
            box->accept();
            return true;
        }
        return false;
    };
    BadSectorsDialog::loopIsSsdForTest = true;
    {
        BadSectorsDialog dialog(&udisks, *ssd);
        dialog.show();
        QVector<ReadSample> samples;
        for (quint64 off = 0; off < 32ull * 1024 * 1024; off += 1024 * 1024)
            samples.append({off, 1024 * 1024, quint32(off == 3 * 1024 * 1024 || off == 20 * 1024 * 1024 ? 250 : 2), true});
        dialog.showScanForTest(samples);
        QPushButton *refresh = dialog.findChild<QPushButton *>(QStringLiteral("refresh"));
        report(refresh && refresh->isVisibleTo(&dialog) && refresh->isEnabled(), QStringLiteral("SSD refresh: after a scan with slow pieces, it's offered"));
        if (refresh)
            refresh->click();
        if (asRoot) {
            waitUntil([&] { return std::any_of(boxes.cbegin(), boxes.cend(), [](const QString &b) { return b.startsWith(QLatin1String("Refreshed")); }); }, 60000);
            report(std::any_of(boxes.cbegin(), boxes.cend(), [](const QString &b) { return b.startsWith(QLatin1String("Refreshed 2")); }),
                   QStringLiteral("SSD refresh: the two slow MiB are refreshed, and it says how fast they read before and after"),
                   boxes.join(QStringLiteral(" | ")).left(300));
        } else {
            QLabel *status = nullptr;
            waitUntil([&] {
                for (QLabel *l : dialog.findChildren<QLabel *>()) {
                    if (l->text().contains(QLatin1String("Couldn't open the drive for writing")))
                        status = l;
                }
                return status != nullptr;
            }, 60000);
            report(status != nullptr, QStringLiteral("SSD refresh: as the user without a password prompt, it can't open the drive and says so"));
        }
    }
    BadSectorsDialog::loopIsSsdForTest = false;
    cleanUpLoop(udisks, loopPath);
    report(digest() == before, QStringLiteral("SSD refresh: the drive holds exactly what it did"));
}

// Format as an SD Card on a 3 GB loop device standing in for a card. As root: the card gets one
// FAT32 partition 4 MiB in, with its name. As the user: writing to a drive directly needs the
// admin password, so without a prompt it's refused, and the window says so.
void sdCardFormat()
{
    const bool asRoot = geteuid() == 0;
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("card.img"));
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("3G"), image});
    QFile backing(image);
    QString loopPath;
    if (backing.open(QIODevice::ReadWrite)) {
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.UDisks2"), QStringLiteral("/org/freedesktop/UDisks2/Manager"),
                                                           QStringLiteral("org.freedesktop.UDisks2.Manager"), QStringLiteral("LoopSetup"));
        call << QVariant::fromValue(QDBusUnixFileDescriptor(backing.handle())) << QVariantMap{{QStringLiteral("auth.no_user_interaction"), true}};
        loopPath = QDBusConnection::systemBus().call(call, QDBus::Block, 30000).arguments().value(0).value<QDBusObjectPath>().path();
        backing.close();
    }
    report(!loopPath.isEmpty(), QStringLiteral("SD card: a loop device stands in for the card"));
    if (loopPath.isEmpty())
        return;
    UDisks udisks;
    udisks.setInteractive(false);
    const Disk *card = nullptr;
    waitUntil([&] {
        udisks.refresh();
        card = udisks.diskByPath(loopPath);
        return card != nullptr;
    }, 15000);
    if (!card) {
        report(false, QStringLiteral("SD card: the loop device shows up"));
        return;
    }
    const QString device = card->device;
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
        SdCardDialog dialog(&udisks, *card, 9);
        dialog.show();
        QApplication::processEvents();
        const QString text = [&] {
            QStringList all;
            for (QLabel *l : dialog.findChildren<QLabel *>())
                all << l->text();
            return all.join(QLatin1Char(' '));
        }();
        report(text.contains(QLatin1String("FAT32")) && text.contains(QLatin1String("32 KiB")) && text.contains(QLatin1String("4 MiB")),
               QStringLiteral("SD card: the window says what the card gets (FAT32, 32 KiB clusters, 4 MiB erase blocks)"), text.left(400));
        QPushButton *format = findButton(&dialog, QStringLiteral("Format"));
        report(format && !format->isEnabled(), QStringLiteral("SD card: nothing starts before the device name is typed"));
        dialog.findChild<QLineEdit *>(QStringLiteral("sdLabel"))->setText(QStringLiteral("Holiday"));
        dialog.findChild<QLineEdit *>(QStringLiteral("sdConfirm"))->setText(shortDevice(device));
        if (format)
            format->click();
        if (asRoot) {
            waitUntil([&] { return !boxes.isEmpty(); }, 120000);
            report(boxes.join(QLatin1Char(' ')).contains(QLatin1String("the card is ready")), QStringLiteral("SD card: it's formatted, and says so"),
                   boxes.join(QStringLiteral(" | ")));
        } else {
            QLabel *status = nullptr;
            waitUntil([&] {
                for (QLabel *l : dialog.findChildren<QLabel *>()) {
                    if (l->text().contains(QLatin1String("Couldn't open")))
                        status = l;
                }
                return status != nullptr;
            }, 60000);
            report(status != nullptr, QStringLiteral("SD card: as the user without a password prompt, it can't open the card and says so"));
        }
    }
    if (asRoot) {
        waitUntil([&] {
            udisks.refresh();
            card = udisks.diskByPath(loopPath);
            return card && card->volumes.size() == 1 && !card->volumes[0].fsType.isEmpty();
        }, 20000);
        report(card && card->tableType == QLatin1String("dos") && card->volumes.size() == 1 && card->volumes[0].fsType == QLatin1String("vfat")
                   && card->volumes[0].offset == 4 * 1024 * 1024 && card->volumes[0].label == QLatin1String("HOLIDAY"),
               QStringLiteral("SD card: an MBR with one FAT32 partition 4 MiB in, named HOLIDAY"),
               card && !card->volumes.isEmpty() ? QStringLiteral("%1 %2 %3").arg(card->volumes[0].fsType).arg(card->volumes[0].offset).arg(card->volumes[0].label) : QString());
    }
    cleanUpLoop(udisks, loopPath);
}

// Save My Files through its window, on a loop device holding an ext4 drive with a person's
// folders on it (made with mkfs.ext4 -d, no root needed): it's mounted read-only to look, the
// folders and bookmarks are found, saved to a folder elsewhere, and what couldn't be read is
// listed at the end.
void saveFilesWindow()
{
    QTemporaryDir dir;
    const QString tree = dir.filePath(QStringLiteral("tree"));
    const auto put = [](const QString &path, const QByteArray &bytes) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
    };
    put(tree + QStringLiteral("/home/carol/Documents/letter.txt"), "Dear Sam,\n");
    put(tree + QStringLiteral("/home/carol/Pictures/2024/dog.jpg"), QByteArray(70000, 'd'));
    put(tree + QStringLiteral("/home/carol/Documents/private.txt"), "not for others\n");
    put(tree + QStringLiteral("/home/carol/.config/google-chrome/Default/Bookmarks"),
        R"({"roots": {"bookmark_bar": {"type": "folder", "name": "Bookmarks bar", "children": [{"type": "url", "name": "News", "url": "https://example.com/news"}]}}})");
    const QString image = dir.filePath(QStringLiteral("drive.img"));
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("64M"), image});
    const int made = sh(QStringLiteral("mkfs.ext4"), {QStringLiteral("-q"), QStringLiteral("-L"), QStringLiteral("OLDPC"), QStringLiteral("-d"), tree, image});
    // One file no one but root can read, made so inside the image (mkfs has to read it first).
    sh(QStringLiteral("debugfs"), {QStringLiteral("-w"), QStringLiteral("-R"), QStringLiteral("set_inode_field /home/carol/Documents/private.txt mode 0100000"), image});
    QFile backing(image);
    QString loopPath;
    if (made == 0 && backing.open(QIODevice::ReadWrite)) {
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.UDisks2"), QStringLiteral("/org/freedesktop/UDisks2/Manager"),
                                                           QStringLiteral("org.freedesktop.UDisks2.Manager"), QStringLiteral("LoopSetup"));
        call << QVariant::fromValue(QDBusUnixFileDescriptor(backing.handle())) << QVariantMap{{QStringLiteral("auth.no_user_interaction"), true}};
        loopPath = QDBusConnection::systemBus().call(call, QDBus::Block, 30000).arguments().value(0).value<QDBusObjectPath>().path();
        backing.close();
    }
    report(!loopPath.isEmpty(), QStringLiteral("save files: a loop device holds an old PC's drive"));
    if (loopPath.isEmpty())
        return;
    UDisks udisks;
    udisks.setInteractive(false);
    waitUntil([&] {
        udisks.refresh();
        const Disk *d = udisks.diskByPath(loopPath);
        return d && !d->volumes.isEmpty() && d->volumes.first().fsType == QLatin1String("ext4");
    }, 15000);
    const QString to = dir.filePath(QStringLiteral("usb"));
    QDir().mkpath(to);
    {
        SaveFilesDialog dialog(&udisks);
        dialog.show();
        auto *drives = dialog.findChild<QListWidget *>(QStringLiteral("drives"));
        int row = -1;
        for (int i = 0; drives && i < drives->count(); ++i) {
            if (drives->item(i)->text().contains(QLatin1String("OLDPC")))
                row = i;
        }
        report(row >= 0, QStringLiteral("save files: the drive is offered, not mounted yet"), drives && row >= 0 ? drives->item(row)->text() : QString());
        if (row < 0) {
            cleanUpLoop(udisks, loopPath);
            return;
        }
        drives->setCurrentRow(row);
        dialog.findChild<QPushButton *>(QStringLiteral("next"))->click();
        auto *found = dialog.findChild<QTreeWidget *>(QStringLiteral("found"));
        waitUntil([&] { return found->topLevelItemCount() > 0; }, 30000);
        QStringList what;
        for (QTreeWidgetItemIterator it(found); *it; ++it)
            what << (*it)->text(0);
        report(what.contains(QLatin1String("carol")) && what.contains(QLatin1String("Documents")) && what.contains(QLatin1String("Pictures"))
                   && what.contains(QLatin1String("Chrome's bookmarks (Default)")),
               QStringLiteral("save files: mounted read-only, carol's folders and bookmarks are found"), what.join(QStringLiteral(", ")));
        dialog.findChild<QLineEdit *>(QStringLiteral("to"))->setText(to);
        auto *save = dialog.findChild<QPushButton *>(QStringLiteral("save"));
        report(save && save->isEnabled(), QStringLiteral("save files: a folder on another drive can be saved to"));
        if (save)
            save->click();
        auto *summary = dialog.findChild<QLabel *>(QStringLiteral("summary"));
        waitUntil([&] { return summary && !summary->text().isEmpty(); }, 60000);
        report(summary && summary->text().startsWith(QLatin1String("Saved 2 file")) && summary->text().contains(QLatin1String("private.txt")),
               QStringLiteral("save files: saved, and the file that couldn't be read is listed"), summary ? summary->text().right(300) : QString());
        const QStringList saved = QDir(to).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QFile letter(to + QLatin1Char('/') + saved.value(0) + QStringLiteral("/carol/Documents/letter.txt"));
        report(saved.size() == 1 && letter.open(QIODevice::ReadOnly) && letter.readAll() == "Dear Sam,\n"
                   && QFile::exists(to + QLatin1Char('/') + saved.value(0) + QStringLiteral("/carol/Bookmarks/Chrome (Default).html")),
               QStringLiteral("save files: the letter and the bookmarks are there"));
    }
    cleanUpLoop(udisks, loopPath);
}

// Repair Linux Boot as the user, on a loop device holding a pretend Debian disk (an EFI
// partition and an ext4 root whose fstab names it): it's found and its repair is explained;
// Repair stays off, since a boot is repaired from DiskAnvil Live.
void linuxBootWindow()
{
    QTemporaryDir dir;
    const QString tree = dir.filePath(QStringLiteral("root"));
    const auto put = [](const QString &path, const QByteArray &bytes) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(bytes) >= 0;
    };
    put(tree + QStringLiteral("/etc/os-release"), "PRETTY_NAME=\"Debian GNU/Linux 13 (trixie)\"\nID=debian\n");
    put(tree + QStringLiteral("/etc/fstab"), "UUID=6d0a7f6e-5b1c-4c2a-9d11-0123456789ab / ext4 errors=remount-ro 0 1\nUUID=1234-ABCD /boot/efi vfat umask=0077 0 1\n");
    put(tree + QStringLiteral("/usr/sbin/grub-install"), "");
    const QString image = dir.filePath(QStringLiteral("debian.img"));
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("96M"), image});
    QProcess sfdisk;
    sfdisk.start(QStringLiteral("sfdisk"), {QStringLiteral("-q"), image});
    sfdisk.waitForStarted();
    sfdisk.write("label: gpt\nstart=2048, size=65536, type=U\nstart=67584, type=L\n");
    sfdisk.closeWriteChannel();
    sfdisk.waitForFinished();
    sh(QStringLiteral("mkfs.fat"), {QStringLiteral("--offset=2048"), QStringLiteral("-i"), QStringLiteral("1234ABCD"), image, QStringLiteral("32768")});
    const int made = sh(QStringLiteral("mkfs.ext4"), {QStringLiteral("-q"), QStringLiteral("-E"), QStringLiteral("offset=%1").arg(67584LL * 512),
                                                      QStringLiteral("-U"), QStringLiteral("6d0a7f6e-5b1c-4c2a-9d11-0123456789ab"), QStringLiteral("-d"), tree, image,
                                                      QStringLiteral("60M")});
    QFile backing(image);
    QString loopPath;
    if (made == 0 && backing.open(QIODevice::ReadWrite)) {
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.UDisks2"), QStringLiteral("/org/freedesktop/UDisks2/Manager"),
                                                           QStringLiteral("org.freedesktop.UDisks2.Manager"), QStringLiteral("LoopSetup"));
        call << QVariant::fromValue(QDBusUnixFileDescriptor(backing.handle())) << QVariantMap{{QStringLiteral("auth.no_user_interaction"), true}};
        loopPath = QDBusConnection::systemBus().call(call, QDBus::Block, 30000).arguments().value(0).value<QDBusObjectPath>().path();
        backing.close();
    }
    report(!loopPath.isEmpty(), QStringLiteral("linux boot: a loop device holds a Debian disk"));
    if (loopPath.isEmpty())
        return;
    UDisks udisks;
    udisks.setInteractive(false);
    waitUntil([&] {
        udisks.refresh();
        const Disk *d = udisks.diskByPath(loopPath);
        return d && d->volumes.size() == 2 && d->volumes[1].fsType == QLatin1String("ext4") && d->volumes[0].fsType == QLatin1String("vfat");
    }, 15000);
    {
        LinuxBootDialog dialog(&udisks);
        dialog.show();
        auto *installs = dialog.findChild<QListWidget *>(QStringLiteral("installs"));
        waitUntil([&] { return installs && installs->count() > 0; }, 30000);
        int row = -1;
        for (int i = 0; installs && i < installs->count(); ++i) {
            if (installs->item(i)->text().startsWith(QLatin1String("Debian GNU/Linux 13")))
                row = i;
        }
        report(row >= 0, QStringLiteral("linux boot: the Debian on it is found"), installs && installs->count() ? installs->item(0)->text() : QString());
        if (row >= 0)
            installs->setCurrentRow(row);
        auto *plan = dialog.findChild<QLabel *>(QStringLiteral("plan"));
        auto *repair = dialog.findChild<QPushButton *>(QStringLiteral("repair"));
        report(plan && plan->text().contains(QLatin1String("grub-install")) && repair && !repair->isEnabled(),
               QStringLiteral("linux boot: its repair is explained, and left to DiskAnvil Live"), plan ? plan->text().left(300) : QString());
    }
    cleanUpLoop(udisks, loopPath);
}

// Restore from Clonezilla onto a loop device's partition: the folder's images are listed, one too
// big for the partition is refused, and the ext4 is put back exactly (as root); as the user,
// writing needs the admin password, so without a prompt it's refused and said.
void clonezillaRestore()
{
    const bool asRoot = geteuid() == 0;
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("target.img"));
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("24M"), image});
    QProcess sfdisk;
    sfdisk.start(QStringLiteral("sfdisk"), {QStringLiteral("-q"), image});
    sfdisk.waitForStarted();
    sfdisk.write("label: gpt\nstart=2048, size=20480, type=L\n");
    sfdisk.closeWriteChannel();
    sfdisk.waitForFinished();
    QFile backing(image);
    QString loopPath;
    if (backing.open(QIODevice::ReadWrite)) {
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.UDisks2"), QStringLiteral("/org/freedesktop/UDisks2/Manager"),
                                                           QStringLiteral("org.freedesktop.UDisks2.Manager"), QStringLiteral("LoopSetup"));
        call << QVariant::fromValue(QDBusUnixFileDescriptor(backing.handle())) << QVariantMap{{QStringLiteral("auth.no_user_interaction"), true}};
        loopPath = QDBusConnection::systemBus().call(call, QDBus::Block, 30000).arguments().value(0).value<QDBusObjectPath>().path();
        backing.close();
    }
    report(!loopPath.isEmpty(), QStringLiteral("clonezilla: a loop device with a 10 MiB partition stands in"));
    if (loopPath.isEmpty())
        return;
    UDisks udisks;
    udisks.setInteractive(false);
    const Volume *part = nullptr;
    waitUntil([&] {
        udisks.refresh();
        const Disk *d = udisks.diskByPath(loopPath);
        part = d && d->volumes.size() == 1 ? &d->volumes[0] : nullptr;
        return part != nullptr;
    }, 15000);
    if (!part)
        return;
    const QString device = part->device;
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
        ClonezillaDialog dialog(&udisks, *part);
        dialog.show();
        dialog.openFolder(QStringLiteral(SOURCE_DIR "/tests/data/clonezilla/image"));
        auto *list = dialog.findChild<QListWidget *>(QStringLiteral("images"));
        auto *note = dialog.findChild<QLabel *>(QStringLiteral("note"));
        auto *restore = dialog.findChild<QPushButton *>(QStringLiteral("restore"));
        list->setCurrentRow(1); // the 36 MiB FAT32: too big for 10 MiB
        report(list->count() == 3 && note->text().contains(QLatin1String("smaller")), QStringLiteral("clonezilla: its images are listed, and one too big is refused"),
               note->text());
        list->setCurrentRow(0);
        dialog.findChild<QLineEdit *>(QStringLiteral("confirm"))->setText(shortDevice(device));
        report(restore->isEnabled(), QStringLiteral("clonezilla: the ext4 fits, and can be restored once the name is typed"));
        restore->click();
        waitUntil([&] { return (note->text().startsWith(QLatin1String("Done")) && !boxes.isEmpty()) || note->text().contains(QLatin1String("couldn't")); }, 60000);
        if (asRoot)
            report(note->text().startsWith(QLatin1String("Done")) && !boxes.isEmpty(), QStringLiteral("clonezilla: restored"),
                   note->text() + QStringLiteral(" | ") + boxes.join(QStringLiteral(" | ")));
        else
            report(note->text().contains(QLatin1String("couldn't be opened")), QStringLiteral("clonezilla: as the user without a password prompt, it's refused and said"),
                   note->text());
    }
    if (asRoot) {
        QFile p(device);
        QByteArray restored;
        if (p.open(QIODevice::ReadOnly))
            restored = p.read(8 * 1024 * 1024);
        QFile sums(QStringLiteral(SOURCE_DIR "/tests/data/clonezilla/sha256.txt"));
        const QByteArray want = sums.open(QIODevice::ReadOnly) ? sums.readLine().left(64) : QByteArray();
        report(QCryptographicHash::hash(restored, QCryptographicHash::Sha256).toHex() == want,
               QStringLiteral("clonezilla: the partition holds exactly the ext4 Clonezilla saved"));
    }
    cleanUpLoop(udisks, loopPath);
}

// Try Starting It on a loop device holding a boot sector of our own (it writes DFBOOT to the
// serial port): opened read-only, handed to QEMU, and it starts (BIOS, since it's MBR).
void testBootWindow()
{
    if (testboot::qemu().isEmpty()) {
        report(true, QStringLiteral("test boot: skipped, no QEMU here"));
        return;
    }
    QTemporaryDir dir;
    QByteArray sector = QByteArray::fromHex("fa31c08ed8be167cbaf803ac84c07403eeebf8f4ebfd") + QByteArray("DFBOOT\r\n", 9);
    sector = sector.leftJustified(510, '\0') + QByteArray("\x55\xaa", 2);
    const QString image = dir.filePath(QStringLiteral("boot.img"));
    {
        QFile f(image);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(sector);
            f.resize(8 * 1024 * 1024);
        }
    }
    QFile backing(image);
    QString loopPath;
    if (backing.open(QIODevice::ReadWrite)) {
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.UDisks2"), QStringLiteral("/org/freedesktop/UDisks2/Manager"),
                                                           QStringLiteral("org.freedesktop.UDisks2.Manager"), QStringLiteral("LoopSetup"));
        call << QVariant::fromValue(QDBusUnixFileDescriptor(backing.handle())) << QVariantMap{{QStringLiteral("auth.no_user_interaction"), true}};
        loopPath = QDBusConnection::systemBus().call(call, QDBus::Block, 30000).arguments().value(0).value<QDBusObjectPath>().path();
        backing.close();
    }
    UDisks udisks;
    udisks.setInteractive(false);
    const Disk *disk = nullptr;
    waitUntil([&] {
        udisks.refresh();
        disk = udisks.diskByPath(loopPath);
        return disk != nullptr;
    }, 15000);
    report(disk != nullptr, QStringLiteral("test boot: a loop device holds a drive that starts"));
    if (!disk)
        return;
    const QString serial = dir.filePath(QStringLiteral("serial.txt"));
    TestBootDialog::serialForTest = serial;
    {
        TestBootDialog dialog(&udisks, disk, QString());
        dialog.show();
        auto *bios = dialog.findChildren<QRadioButton *>().value(1);
        report(bios && bios->isChecked(), QStringLiteral("test boot: an MBR drive is started the BIOS way"));
        dialog.findChild<QPushButton *>(QStringLiteral("start"))->click();
        auto *note = dialog.findChild<QLabel *>(QStringLiteral("note"));
        QByteArray said;
        waitUntil([&] {
            QFile log(serial);
            if (log.open(QIODevice::ReadOnly))
                said = log.readAll();
            return said.contains("DFBOOT") || note->text().contains(QLatin1String("isn't")) || note->text().contains(QLatin1String("didn't"))
                || note->text().contains(QLatin1String("couldn't"));
        }, 30000);
        if (geteuid() == 0)
            report(said.contains("DFBOOT"), QStringLiteral("test boot: the drive starts in QEMU, read through its handle"),
                   note->text() + QStringLiteral(" | ") + QString::fromLatin1(said.left(80)));
        else
            report(note->text().contains(QLatin1String("couldn't be opened")) || said.contains("DFBOOT"),
                   QStringLiteral("test boot: as the user without a password prompt, reading the drive is refused and said"), note->text());
    }
    // QEMU goes when it's done: it's stopped here (it'd otherwise sit halted).
    for (QProcess *qemu : udisks.findChildren<QProcess *>()) {
        qemu->kill();
        qemu->waitForFinished(5000);
        if (const QByteArray said = qemu->readAllStandardError(); !said.isEmpty())
            out << "QEMU said: " << said.left(400) << Qt::endl;
    }
    TestBootDialog::serialForTest.clear();
    cleanUpLoop(udisks, loopPath);
}

// Start Other PCs from This One: the HTTP server hands out live/ and nothing else; the helper
// (dry run, stand-in files and address) writes the right menu and dnsmasq command; the window
// lists the PCs as dnsmasq (a stand-in) and the server see them, and Stop ends it.
void netBootWindow()
{
    QTemporaryDir dir;
    const QString medium = dir.filePath(QStringLiteral("medium"));
    const QByteArray system = QByteArray(6 * 1024 * 1024 + 123, 's');
    const auto put = [](const QString &path, const QByteArray &data) {
        QDir().mkpath(QFileInfo(path).path());
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
    };
    put(medium + QStringLiteral("/live/filesystem.squashfs"), system);
    put(medium + QStringLiteral("/live/vmlinuz"), "kernel");
    put(medium + QStringLiteral("/live/initrd.img"), "initrd");
    put(medium + QStringLiteral("/EFI/BOOT/BOOTX64.EFI"), "shim");
    put(medium + QStringLiteral("/boot/grub/netboot/grubx64.efi"), "grubnet");
    for (const char *bios : {"pxelinux.0", "ldlinux.c32", "menu.c32", "libutil.c32"})
        put(medium + QStringLiteral("/boot/pxelinux/") + QLatin1String(bios), "pxelinux");
    put(medium + QStringLiteral("/.disk/diskanvil-live"), "DiskAnvil Live\nversion=0.0.5\n");
    put(dir.filePath(QStringLiteral("secret.txt")), "secret");
    // A request, and what came back.
    const auto ask = [](quint16 port, const QByteArray &request) {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, port);
        if (!socket.waitForConnected(3000))
            return QByteArray();
        socket.write(request);
        QByteArray got;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 10000) {
            QCoreApplication::processEvents();
            if (socket.waitForReadyRead(50))
                got += socket.readAll();
            if (socket.state() == QAbstractSocket::UnconnectedState)
                break;
        }
        return got + socket.readAll();
    };
    {
        QTcpServer busy;
        busy.listen(QHostAddress::AnyIPv4, 8099);
        netboot::FileServer server(medium);
        QStringList finished;
        QObject::connect(&server, &netboot::FileServer::finished, [&](const QString &pc, bool whole) { finished << pc + (whole ? QStringLiteral(" whole") : QStringLiteral(" cut")); });
        const bool listening = server.listen();
        report(listening && busy.isListening() && server.port() > 8099 && server.port() <= 8199,
               QStringLiteral("netboot: the server takes the next port when one is busy"), QString::number(server.port()));
        const QByteArray got = ask(server.port(), "GET /live/filesystem.squashfs HTTP/1.1\r\nHost: x\r\n\r\n");
        const qsizetype body = got.indexOf("\r\n\r\n");
        report(got.startsWith("HTTP/1.1 200 OK") && got.contains("Content-Length: " + QByteArray::number(system.size())) && body > 0 && got.mid(body + 4) == system,
               QStringLiteral("netboot: the system file comes over HTTP whole"), QString::number(got.size()));
        waitUntil([&] { return !finished.isEmpty(); }, 3000);
        report(finished.value(0).endsWith(QLatin1String("whole")), QStringLiteral("netboot: and the server says that PC has it all"), finished.join(QLatin1Char(',')));
        bool refused = true;
        for (const QByteArray &path : {QByteArrayLiteral("/../secret.txt"), QByteArrayLiteral("/live/../../secret.txt"), QByteArrayLiteral("/secret.txt"),
                                       QByteArrayLiteral("/live/"), QByteArrayLiteral("/live/%2e%2e/x"), QByteArrayLiteral("/EFI/BOOT/BOOTX64.EFI")})
            refused = refused && ask(server.port(), "GET " + path + " HTTP/1.1\r\n\r\n").startsWith("HTTP/1.1 404");
        report(refused, QStringLiteral("netboot: nothing outside live/ is handed out"));
        report(ask(server.port(), "POST /live/vmlinuz HTTP/1.1\r\n\r\n").startsWith("HTTP/1.1 405") && ask(server.port(), "HEAD /live/vmlinuz HTTP/1.1\r\n\r\n").endsWith("\r\n\r\n"),
               QStringLiteral("netboot: only GET (and HEAD, without the file)"));
    }
    // The helper, dry: its menu, links and dnsmasq command, for 192.168.37.42/22.
    const QString helper = QStringLiteral(SOURCE_DIR "/rescue/files/usr/local/bin/diskanvil-live-netboot");
    const QString state = dir.filePath(QStringLiteral("state"));
    QProcess dry;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("DISKANVIL_NETBOOT_MEDIUM"), medium);
    env.insert(QStringLiteral("DISKANVIL_NETBOOT_STATE"), state);
    env.insert(QStringLiteral("DISKANVIL_NETBOOT_ADDRESS"), QStringLiteral("192.168.37.42/22"));
    env.insert(QStringLiteral("DISKANVIL_NETBOOT_DRY"), QStringLiteral("1"));
    dry.setProcessEnvironment(env);
    dry.start(QStringLiteral("bash"), {helper, QStringLiteral("start"), QStringLiteral("eth9"), QStringLiteral("8123")});
    dry.waitForFinished(10000);
    const QStringList args = QString::fromUtf8(dry.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QFile menu(state + QStringLiteral("/grub/grub.cfg"));
    const QString cfg = menu.open(QIODevice::ReadOnly) ? QString::fromUtf8(menu.readAll()) : QString();
    QFile biosMenu(state + QStringLiteral("/pxelinux.cfg/default"));
    const QString biosCfg = biosMenu.open(QIODevice::ReadOnly) ? QString::fromUtf8(biosMenu.readAll()) : QString();
    report(dry.exitCode() == 0 && args.contains(QStringLiteral("--dhcp-range=192.168.36.0,proxy")) && args.contains(QStringLiteral("--interface=eth9"))
               && args.contains(QStringLiteral("--tftp-root=") + state) && args.contains(QStringLiteral("--pxe-service=X86-64_EFI,DiskAnvil Live,bootx64.efi"))
               && args.contains(QStringLiteral("--pxe-service=x86PC,DiskAnvil Live,pxelinux")),
           QStringLiteral("netboot: dnsmasq answers as a proxy for the card's own network"), args.join(QLatin1Char(' ')) + QString::fromUtf8(dry.readAllStandardError()));
    report(cfg.contains(QLatin1String("fetch=http://192.168.37.42:8123/live/filesystem.squashfs")) && cfg.contains(QLatin1String("DiskAnvil Live 0.0.5"))
               && QFileInfo(state + QStringLiteral("/bootx64.efi")).canonicalFilePath() == QFileInfo(medium + QStringLiteral("/EFI/BOOT/BOOTX64.EFI")).canonicalFilePath()
               && QFileInfo(state + QStringLiteral("/grubx64.efi")).canonicalFilePath() == QFileInfo(medium + QStringLiteral("/boot/grub/netboot/grubx64.efi")).canonicalFilePath()
               && QFile::exists(state + QStringLiteral("/live/initrd.img")) && QFile::exists(state + QStringLiteral("/pxelinux.0"))
               && QFile::exists(state + QStringLiteral("/menu.c32")) && QFile::exists(state + QStringLiteral("/ldlinux.c32"))
               && biosCfg.contains(QLatin1String("APPEND boot=live ip=dhcp fetch=http://192.168.37.42:8123/live/filesystem.squashfs"))
               && biosCfg.contains(QLatin1String("DiskAnvil Live 0.0.5")) && biosCfg.contains(QLatin1String("LOCALBOOT 0")),
           QStringLiteral("netboot: the menus (UEFI and BIOS) say where the system is, and the boot files are linked from the stick"), cfg.left(400) + biosCfg.left(400));
    QProcess bad;
    bad.setProcessEnvironment(env);
    bad.start(QStringLiteral("bash"), {helper, QStringLiteral("start"), QStringLiteral("eth0;reboot"), QStringLiteral("8123")});
    bad.waitForFinished(10000);
    QProcessEnvironment noFiles = env;
    noFiles.insert(QStringLiteral("DISKANVIL_NETBOOT_MEDIUM"), dir.filePath(QStringLiteral("nothing")));
    QProcess missing;
    missing.setProcessEnvironment(noFiles);
    missing.start(QStringLiteral("bash"), {helper, QStringLiteral("start"), QStringLiteral("eth9"), QStringLiteral("8123")});
    missing.waitForFinished(10000);
    report(bad.exitCode() == 2 && missing.exitCode() == 3 && QString::fromUtf8(missing.readAllStandardError()).contains(QLatin1String("own files")),
           QStringLiteral("netboot: an odd card name, or no stick to hand out, is refused"));

    // The window, with a stand-in for dnsmasq that says what a real one does.
    if (netboot::cards().isEmpty()) {
        out << "SKIP  netboot window: this PC has no network" << Qt::endl;
        return;
    }
    QFile fake(dir.filePath(QStringLiteral("netboot")));
    if (fake.open(QIODevice::WriteOnly))
        fake.write("#!/bin/sh\necho \"$3\" > " + dir.path().toUtf8() + "/port\necho \"dnsmasq-dhcp: 123 PXE(eth9) 52:54:00:AA:BB:CC proxy\" >&2\n"
                   "sleep 0.3\necho \"dnsmasq-tftp: sent /run/diskanvil-live/netboot/live/initrd.img to 10.9.8.7\" >&2\nexec sleep 60\n");
    fake.close();
    fake.setPermissions(fake.permissions() | QFileDevice::ExeOwner);
    netboot::helperForTest = fake.fileName();
    netboot::mediumForTest = medium;
    {
        NetBootDialog dialog;
        dialog.show();
        auto *pcs = dialog.findChild<QLabel *>(QStringLiteral("pcs"));
        findButton(&dialog, QStringLiteral("Start"))->click();
        waitUntil([&] { return pcs->text().contains(QLatin1String("10.9.8.7")); }, 5000);
        report(pcs->text().contains(QLatin1String("52:54:00:aa:bb:cc</b>: asked")) && pcs->text().contains(QLatin1String("10.9.8.7</b>: has the start files")),
               QStringLiteral("netboot window: it lists the PCs that asked, and what they've had"), pcs->text());
        QFile portFile(dir.filePath(QStringLiteral("port")));
        const quint16 port = portFile.open(QIODevice::ReadOnly) ? quint16(portFile.readAll().trimmed().toUInt()) : 0;
        const QByteArray got = ask(port, "GET /live/filesystem.squashfs HTTP/1.1\r\n\r\n");
        waitUntil([&] { return pcs->text().contains(QLatin1String("has DiskAnvil Live")); }, 3000);
        report(got.endsWith(system) && pcs->text().contains(QLatin1String("127.0.0.1</b>: has DiskAnvil Live")),
               QStringLiteral("netboot window: a PC that took the system shows up as having it"), pcs->text());
        findButton(&dialog, QStringLiteral("Stop"))->click();
        report(!dialog.serving() && ask(port, "GET /live/vmlinuz HTTP/1.1\r\n\r\n").isEmpty(), QStringLiteral("netboot window: Stop ends dnsmasq and the server"));
    }
    netboot::helperForTest.clear();
    netboot::mediumForTest.clear();
}

// Put DiskAnvil Live on This PC, on a loop device standing in for the PC's drive (GPT, a partition
// and free space): the partition goes at the end of the free space with every file checked, the
// boot menu script gets that drive and partition (handed to the test, not run: this PC's boot menu
// is never touched); Update keeps the logs; Take It Off removes the entry and the partition.
void liveOnPcWindow()
{
    QTemporaryDir dir;
    const QString tree = dir.filePath(QStringLiteral("tree"));
    QMap<QString, QByteArray> files;
    files[QStringLiteral(".disk/diskanvil-live")] = "DiskAnvil Live\nversion=0.0.4\nbuilt=2026-10-12\nid=test\n";
    files[QStringLiteral("EFI/BOOT/BOOTX64.EFI")] = QByteArray(300000, 'x');
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
    const QString iso = dir.filePath(QStringLiteral("live.iso"));
    if (sh(QStringLiteral("xorriso"), {QStringLiteral("-as"), QStringLiteral("mkisofs"), QStringLiteral("-quiet"), QStringLiteral("-J"), QStringLiteral("-joliet-long"),
                                       QStringLiteral("-R"), QStringLiteral("-o"), iso, tree}) != 0) {
        out << "SKIP  Live on this PC: xorriso isn't installed" << Qt::endl;
        return;
    }
    const QString image = dir.filePath(QStringLiteral("pc.img"));
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("4G"), image});
    QProcess sfdisk;
    sfdisk.start(QStringLiteral("sfdisk"), {QStringLiteral("-q"), image});
    sfdisk.waitForStarted();
    sfdisk.write("label: gpt\nstart=2048, size=2097152, type=L\n");
    sfdisk.closeWriteChannel();
    sfdisk.waitForFinished();
    const QString loopPath = userLoop(image);
    report(!loopPath.isEmpty(), QStringLiteral("Live on this PC: a 4 GB loop device stands in for the PC's drive"));
    if (loopPath.isEmpty())
        return;
    UDisks udisks;
    udisks.setInteractive(false);
    const Disk *pc = nullptr;
    waitUntil([&] {
        udisks.refresh();
        pc = udisks.diskByPath(loopPath);
        return pc && pc->volumes.size() == 1;
    }, 15000);
    const QString device = pc ? pc->device : QString();
    const quint64 driveSize = pc ? pc->size : 0;
    LiveOnPcDialog::allowLoopDevicesForTest = true;
    QStringList scripts, scriptArgs;
    LiveOnPcDialog::rootForTest = [&](const QString &script, const QStringList &args, QString *said) {
        scripts << script;
        scriptArgs << args.join(QLatin1Char(' '));
        *said = QStringLiteral("Boot0009");
        return true;
    };
    QStringList boxes;
    Answerer answerer;
    answerer.answer = [&](QWidget *modal) {
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            boxes << box->text();
            if (box->button(QMessageBox::Yes))
                box->button(QMessageBox::Yes)->click();
            else
                box->accept();
            return true;
        }
        return false;
    };
    const auto pick = [&](LiveOnPcDialog &dialog) {
        auto *targets = dialog.findChild<QComboBox *>();
        for (int i = 0; i < targets->count(); ++i) {
            if (targets->itemData(i).toString() == loopPath)
                targets->setCurrentIndex(i);
        }
    };
    const auto livePart = [&]() -> const Volume * {
        udisks.refresh();
        pc = udisks.diskByPath(loopPath);
        for (const Volume &v : pc ? pc->volumes : QVector<Volume>()) {
            if (v.label == QLatin1String("DISKANVIL"))
                return &v;
        }
        return nullptr;
    };
    {
        LiveOnPcDialog dialog(&udisks);
        dialog.show();
        dialog.findChild<QLineEdit *>(QStringLiteral("image"))->setText(iso);
        pick(dialog);
        QPushButton *put = findButton(&dialog, QStringLiteral("Put It on This PC"));
        auto *plan = dialog.findChild<QLabel *>(QStringLiteral("plan"));
        report(put && put->isEnabled() && plan->text().contains(QLatin1String("2.00 GB partition")), QStringLiteral("Live on this PC: it says what it'll do"), plan->text());
        put->click();
        waitUntil([&] { return boxes.size() >= 2; }, 180000);
        waitUntil([&] { const Volume *v = livePart(); return v && v->mounts().isEmpty(); }, 15000);
    }
    const Volume *part = livePart();
    report(boxes.value(1).startsWith(QLatin1String("DiskAnvil Live is on this PC")), QStringLiteral("Live on this PC: done"), boxes.join(QStringLiteral(" | ")).left(300));
    report(part && part->fsType == QLatin1String("vfat") && part->number == 2 && part->size == 2048ull * 1024 * 1024 && part->offset + part->size == driveSize - 1024 * 1024
               && part->mounts().isEmpty(),
           QStringLiteral("Live on this PC: a 2 GB FAT32 DISKANVIL partition at the end of the free space, unmounted after"),
           part ? QStringLiteral("%1 %2 at %3 size %4").arg(part->fsType).arg(part->number).arg(part->offset).arg(part->size) : QString());
    report(scripts.value(0) == liveonpc::addScript() && scriptArgs.value(0) == device + QStringLiteral(" 2"),
           QStringLiteral("Live on this PC: the boot menu script gets that drive and partition"), scriptArgs.join(QStringLiteral(" | ")));
    if (part) {
        const QString root = mountStick(udisks, loopPath, QStringLiteral("DISKANVIL"));
        int listed = 0;
        const int good = filesMatchingSums(root, &listed);
        report(listed > 0 && good == listed, QStringLiteral("Live on this PC: every file on it matches sha256sum.txt"), QStringLiteral("%1 of %2").arg(good).arg(listed));
        QDir().mkpath(root + QStringLiteral("/logs/2026-10-12 10.00"));
        QFile log(root + QStringLiteral("/logs/2026-10-12 10.00/boot.log"));
        if (log.open(QIODevice::WriteOnly))
            log.write("log");
        log.close();
        QFile old(root + QStringLiteral("/left-from-before.txt"));
        if (old.open(QIODevice::WriteOnly))
            old.write("old");
        old.close();
        sh(QStringLiteral("sync"), {});
    }
    // Update: this version over it, its logs kept, the boot menu left alone.
    boxes.clear();
    scripts.clear();
    {
        LiveOnPcDialog dialog(&udisks);
        dialog.show();
        dialog.findChild<QLineEdit *>(QStringLiteral("image"))->setText(iso);
        pick(dialog);
        QPushButton *update = findButton(&dialog, QStringLiteral("Update It"));
        report(update && update->isVisibleTo(&dialog) && update->isEnabled(), QStringLiteral("Live on this PC: opened again, it offers Update and Take It Off"));
        if (update)
            update->click();
        waitUntil([&] { return !boxes.isEmpty(); }, 180000);
        waitUntil([&] { const Volume *v = livePart(); return v && v->mounts().isEmpty(); }, 15000);
    }
    const QString again = mountStick(udisks, loopPath, QStringLiteral("DISKANVIL"));
    report(boxes.value(0).contains(QLatin1String("updated")) && scripts.isEmpty() && QFile::exists(again + QStringLiteral("/logs/2026-10-12 10.00/boot.log"))
               && !QFile::exists(again + QStringLiteral("/left-from-before.txt")) && QFile::exists(again + QStringLiteral("/live/filesystem.squashfs")),
           QStringLiteral("Live on this PC: Update replaces it and keeps the logs, without touching the boot menu"), boxes.join(QStringLiteral(" | ")).left(200));
    if (const Volume *v = livePart())
        udisks.unmount(*v);
    waitUntil([&] { const Volume *v = livePart(); return v && v->mounts().isEmpty(); }, 10000);
    // Take It Off.
    boxes.clear();
    {
        LiveOnPcDialog dialog(&udisks);
        dialog.show();
        pick(dialog);
        QPushButton *off = findButton(&dialog, QStringLiteral("Take It Off"));
        if (off)
            off->click();
        waitUntil([&] { return boxes.size() >= 2; }, 60000);
    }
    waitUntil([&] { return livePart() == nullptr; }, 15000);
    report(!livePart() && scripts.value(0) == liveonpc::removeScript() && pc && pc->volumes.size() == 1,
           QStringLiteral("Live on this PC: Take It Off removes the entry and the partition, and nothing else"), boxes.join(QStringLiteral(" | ")).left(300));
    LiveOnPcDialog::allowLoopDevicesForTest = false;
    LiveOnPcDialog::rootForTest = nullptr;
    cleanUpLoop(udisks, loopPath);
}

// Let Someone Help: on a PC it says what to use instead; on DiskAnvil Live (stand-ins for x11vnc
// and the helper here) it shows the address and passwords, says who connected and when they've
// gone, and Stop ends the screen sharing and switches SSH off.
void remoteHelpWindow()
{
    {
        RemoteHelpDialog pc;
        report(!findButton(&pc, QStringLiteral("Let Them In")), QStringLiteral("Let Someone Help: on a PC it says what does this there instead"));
    }
    QTemporaryDir dir;
    const auto script = [&](const QString &name, const QByteArray &text) {
        QFile f(dir.filePath(name));
        if (f.open(QIODevice::WriteOnly))
            f.write(text);
        f.close();
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner);
        return f.fileName();
    };
    const QByteArray d = dir.path().toUtf8();
    remotehelp::x11vncForTest = script(QStringLiteral("x11vnc"), "#!/bin/sh\nfor a; do case $a in rm:*) f=${a#rm:};; esac; done\ncp \"$f\" " + d
                                                                 + "/vnc-password; rm -f \"$f\"\necho PORT=5901\nsleep 0.5\necho \"12/10/2026 Got connection from client 10.1.2.3\" >&2\n"
                                                                   "sleep 1.5\necho \"client_count: 0\" >&2\nexec sleep 60\n");
    remotehelp::helperForTest = script(QStringLiteral("helper"), "#!/bin/sh\necho \"$1\" >> " + d + "/helper.log\n[ \"$1\" = ssh-on ] && cat > " + d + "/ssh-password\nexit 0\n");
    {
        RemoteHelpDialog dialog;
        dialog.show();
        dialog.findChild<QCheckBox *>(QStringLiteral("terminal"))->setChecked(true);
        auto *status = dialog.findChild<QLabel *>(QStringLiteral("status"));
        auto *details = dialog.findChild<QLabel *>(QStringLiteral("details"));
        findButton(&dialog, QStringLiteral("Let Them In"))->click();
        waitUntil([&] { return status->text().contains(QLatin1String("10.1.2.3")); }, 5000);
        const auto read = [&](const QString &name) {
            QFile f(dir.filePath(name));
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
        };
        const QString vnc = read(QStringLiteral("vnc-password")), ssh = read(QStringLiteral("ssh-password"));
        report(vnc.size() == 8 && ssh.size() == 12 && details->text().contains(vnc) && details->text().contains(ssh) && details->text().contains(QLatin1String(":5901")),
               QStringLiteral("Let Someone Help: the passwords given out are the ones x11vnc and SSH got, with the port it took"), details->text());
        report(status->text().contains(QLatin1String("Connected: 10.1.2.3")), QStringLiteral("Let Someone Help: it says who connected"), status->text());
        waitUntil([&] { return status->text().contains(QLatin1String("gone")); }, 5000);
        report(status->text().contains(QLatin1String("gone")), QStringLiteral("Let Someone Help: and when they've gone"), status->text());
        findButton(&dialog, QStringLiteral("Stop"))->click();
        report(!dialog.helping() && read(QStringLiteral("helper.log")) == QLatin1String("ssh-on\nssh-off") && !details->text().contains(vnc),
               QStringLiteral("Let Someone Help: Stop ends it, SSH goes off, and the passwords go"), read(QStringLiteral("helper.log")));
        // Closing it with them still in stops it too.
        findButton(&dialog, QStringLiteral("Let Them In"))->click();
        waitUntil([&] { return dialog.helping(); }, 3000);
        dialog.reject();
        report(!dialog.helping() && read(QStringLiteral("helper.log")).endsWith(QLatin1String("ssh-on\nssh-off")) && read(QStringLiteral("helper.log")).count(QLatin1String("ssh-off")) == 2,
               QStringLiteral("Let Someone Help: closing the window stops it as well"));
    }
    remotehelp::x11vncForTest.clear();
    remotehelp::helperForTest.clear();
}

// DiskAnvil was called DiskForge: its settings (the theme's name too) and its data folder move over
// once, and not again over newer ones.
void oldNameMoveOver()
{
    QTemporaryDir dir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, dir.filePath(QStringLiteral("config")));
    QStandardPaths::setTestModeEnabled(true);
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    QDir(data + QStringLiteral("/diskforge")).removeRecursively();
    QDir(data + QStringLiteral("/diskanvil")).removeRecursively();
    {
        QSettings before(QStringLiteral("diskforge"), QStringLiteral("diskforge"));
        before.setValue(QStringLiteral("theme"), QStringLiteral("diskforge-live"));
        before.setValue(QStringLiteral("certificates/by"), QStringLiteral("Danny's PC shop"));
        QSettings addons(QStringLiteral("diskforge"), QStringLiteral("addons"));
        addons.setValue(QStringLiteral("trusted/a"), true);
    }
    QDir().mkpath(data + QStringLiteral("/diskforge/clamav"));
    QFile list(data + QStringLiteral("/diskforge/clamav/main.cvd"));
    if (list.open(QIODevice::WriteOnly))
        list.write("list");
    list.close();
    const oldname::Moved moved = oldname::moveOver();
    QSettings now(QStringLiteral("diskanvil"), QStringLiteral("diskanvil"));
    report(moved.settings && now.value(QStringLiteral("theme")).toString() == QLatin1String("diskanvil-live")
               && now.value(QStringLiteral("certificates/by")).toString() == QLatin1String("Danny's PC shop")
               && QSettings(QStringLiteral("diskanvil"), QStringLiteral("addons")).value(QStringLiteral("trusted/a")).toBool(),
           QStringLiteral("old name: DiskForge's settings move over, its Live theme under the new name"));
    report(moved.folders.contains(data + QStringLiteral("/diskanvil")) && QFile::exists(data + QStringLiteral("/diskanvil/clamav/main.cvd"))
               && !QFileInfo::exists(data + QStringLiteral("/diskforge")),
           QStringLiteral("old name: and its folder (the virus list in it)"), moved.folders.join(QStringLiteral(", ")));
    // Once only: newer settings aren't written over.
    now.setValue(QStringLiteral("theme"), QStringLiteral("system"));
    now.sync();
    const oldname::Moved again = oldname::moveOver();
    report(!again.settings && again.folders.isEmpty() && QSettings(QStringLiteral("diskanvil"), QStringLiteral("diskanvil")).value(QStringLiteral("theme")).toString() == QLatin1String("system"),
           QStringLiteral("old name: only once"));
    QDir(data + QStringLiteral("/diskanvil")).removeRecursively();
    QStandardPaths::setTestModeEnabled(false);
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation));
}

// Write-Protect Every Drive: off a PC it says why not; as root, with the helper from the repo
// limited to a loop device of its own, it makes the drive and its partition read-only (a mounted
// one goes read-only too), shows the bar, and stops the Format and Wipe buttons; one drive can be
// let through, and Switch Off makes them writable again. Nothing outside the loop device is touched.
void writeBlockMode()
{
    if (::geteuid() != 0) {
        UDisks udisks;
        udisks.setInteractive(false);
        MainWindow window(&udisks);
        auto *action = window.findChild<QAction *>(QStringLiteral("writeBlock"));
        report(action && !action->isEnabled() && action->toolTip().contains(QLatin1String("Only DiskAnvil Live")),
               QStringLiteral("Write-Protect Every Drive: on a PC it's off, and says why"), action ? action->toolTip() : QString());
        return;
    }
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("drive.img"));
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
    const QString name = loop.section(QLatin1Char('/'), -1), part = loop + QStringLiteral("p1");
    waitUntil([&] { return QFile::exists(part); }, 5000);
    sh(QStringLiteral("mkfs.ext4"), {QStringLiteral("-q"), part});
    const QString mnt = dir.filePath(QStringLiteral("mnt"));
    QDir().mkpath(mnt);
    sh(QStringLiteral("mount"), {part, mnt});
    const QString state = dir.filePath(QStringLiteral("state"));
    qputenv("DISKANVIL_WRITEBLOCK_STATE", state.toLocal8Bit());
    qputenv("DISKANVIL_WRITEBLOCK_ONLY", name.toLocal8Bit());
    writeblock::helperForTest = QStringLiteral(SOURCE_DIR "/rescue/files/usr/local/bin/diskanvil-live-writeblock");
    const auto ro = [](const QString &device) {
        QString said;
        sh(QStringLiteral("blockdev"), {QStringLiteral("--getro"), device}, &said);
        return said.trimmed() == QLatin1String("1");
    };
    const auto canWrite = [&] {
        QFile f(mnt + QStringLiteral("/test.txt"));
        return f.open(QIODevice::WriteOnly) && f.write("x") == 1;
    };
    report(canWrite() && !ro(loop), QStringLiteral("Write-Protect Every Drive: the test drive starts writable"));

    UDisks udisks;
    udisks.setInteractive(false);
    MainWindow window(&udisks);
    window.resize(1200, 800);
    window.show();
    auto *action = window.findChild<QAction *>(QStringLiteral("writeBlock"));
    report(action && action->isEnabled(), QStringLiteral("Write-Protect Every Drive: offered on DiskAnvil Live (the helper is there)"));
    if (!action) {
        sh(QStringLiteral("umount"), {mnt});
        sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
        return;
    }
    action->trigger();
    const Disk *disk = nullptr;
    waitUntil([&] {
        udisks.refresh();
        disk = udisks.diskByPath(QString());
        for (const Disk &d : udisks.disks()) {
            if (d.device == loop)
                disk = &d;
        }
        return disk && disk->readOnly;
    }, 10000);
    QFile rule(state + QStringLiteral("/90-diskanvil-live-writeblock.rules"));
    const QByteArray ruleText = rule.open(QIODevice::ReadOnly) ? rule.readAll() : QByteArray();
    report(writeblock::isOn() && ro(loop) && ro(part) && !canWrite() && ruleText.contains("RUN+=\"/usr/sbin/blockdev --setro $env{DEVNAME}\""),
           QStringLiteral("Write-Protect Every Drive: the drive and its partition are read-only, the mounted one too, and new drives get a udev rule"));
    if (!QStandardPaths::findExecutable(QStringLiteral("udevadm")).isEmpty()) {
        QString said;
        const int verify = sh(QStringLiteral("udevadm"), {QStringLiteral("verify"), QStringLiteral("--no-style"), rule.fileName()}, &said);
        report(verify == 0, QStringLiteral("Write-Protect Every Drive: udev reads the rule without complaint"), said.trimmed());
    }
    // Root may open it for writing; the kernel refuses the write itself.
    QFile raw(loop);
    const bool opened = raw.open(QIODevice::ReadWrite | QIODevice::Unbuffered);
    const bool wrote = opened && raw.write(QByteArray(4096, 'w')) == 4096 && raw.flush();
    report(!wrote, QStringLiteral("Write-Protect Every Drive: the kernel refuses a write to the drive"), raw.errorString());
    raw.close();
    report(disk && disk->readOnly, QStringLiteral("Write-Protect Every Drive: UDisks sees it read-only"));
    QCoreApplication::processEvents();
    NoticeBar *bar = window.findChild<NoticeBar *>(QStringLiteral("writeBlock"));
    report(bar && bar->isVisibleTo(&window), QStringLiteral("Write-Protect Every Drive: a bar says so"), bar ? bar->text() : QString());
    // The drive picked: Wipe says why it's off. (Unmounted first: a drive mounted under /tmp counts as the system's.)
    sh(QStringLiteral("umount"), {mnt});
    udisks.refresh();
    auto *map = window.findChild<DiskMap *>();
    const QVector<Disk> &disks = udisks.disks();
    for (int i = 0; i < disks.size(); ++i) {
        if (disks[i].device == loop)
            map->selectDisk(i);
    }
    Q_EMIT map->selectionChanged(); // as a click would
    QCoreApplication::processEvents();
    QAction *wipe = nullptr;
    for (QAction *a : window.findChildren<QAction *>()) {
        if (a->text() == QStringLiteral("&Wipe Disk…"))
            wipe = a;
    }
    report(wipe && !wipe->isEnabled() && wipe->toolTip().contains(QLatin1String("write-protected")),
           QStringLiteral("Write-Protect Every Drive: Wipe Disk is off for it, and says why"),
           !wipe ? QStringLiteral("no Wipe Disk action") : QStringLiteral("%1, \"%2\"").arg(wipe->isEnabled() ? QStringLiteral("on") : QStringLiteral("off"), wipe->toolTip()));
    // One let through, to save to.
    QProcess allow;
    const QStringList command = writeblock::command({QStringLiteral("allow"), name});
    allow.start(command.first(), command.mid(1));
    allow.waitForFinished(30000);
    report(allow.exitCode() == 0 && !ro(loop) && !ro(part) && writeblock::allowed() == QStringList{name},
           QStringLiteral("Write-Protect Every Drive: one drive can be let through"), QString::fromLocal8Bit(allow.readAll()));
    // On again, and off from the bar.
    action->trigger(); // off (it was checked)
    report(!writeblock::isOn() && !ro(loop), QStringLiteral("Write-Protect Every Drive: off from the menu"));
    action->trigger(); // on
    QCoreApplication::processEvents();
    bar = window.findChild<NoticeBar *>(QStringLiteral("writeBlock"));
    QPushButton *off = bar ? findButton(bar, QStringLiteral("Switch Off")) : nullptr;
    report(ro(loop) && off, QStringLiteral("Write-Protect Every Drive: on again"));
    if (off)
        off->click();
    waitUntil([&] { return !writeblock::isOn(); }, 5000);
    sh(QStringLiteral("mount"), {part, mnt});
    report(!writeblock::isOn() && !ro(loop) && !ro(part) && canWrite(), QStringLiteral("Write-Protect Every Drive: Switch Off makes it writable again"));
    const auto barShown = [&] {
        QCoreApplication::processEvents();
        for (NoticeBar *b : window.findChildren<NoticeBar *>(QStringLiteral("writeBlock")))
            if (b->isVisibleTo(&window))
                return true;
        return false;
    };
    waitUntil([&] { return !barShown(); }, 3000);
    report(!barShown(), QStringLiteral("Write-Protect Every Drive: and the bar goes"));
    window.close();
    writeblock::helperForTest.clear();
    qunsetenv("DISKANVIL_WRITEBLOCK_STATE");
    qunsetenv("DISKANVIL_WRITEBLOCK_ONLY");
    sh(QStringLiteral("umount"), {mnt});
    sh(QStringLiteral("losetup"), {QStringLiteral("-d"), loop});
}

// Make a DiskAnvil Live USB with kept changes and a files drive, on a loop device: DiskAnvil
// Live's partition, a "persistence" one with live-boot's persistence.conf, and an exFAT drive on
// the rest. Then Update: only DiskAnvil Live's partition is made again; its logs, the kept
// changes and the files stay.
void rescueUsbLayout()
{
    QTemporaryDir dir;
    // DISKANVIL_TEST_RESCUE_ISO: the real DiskAnvil Live ISO instead of a made-up one, and
    // DISKANVIL_TEST_LAYOUT_STICK: where the stick is kept afterwards, laid out with kept changes and
    // a files drive, to start in a VM.
    const QString realIso = qEnvironmentVariable("DISKANVIL_TEST_RESCUE_ISO");
    const QString keptStick = qEnvironmentVariable("DISKANVIL_TEST_LAYOUT_STICK");
    const QString tree = dir.filePath(QStringLiteral("tree"));
    QMap<QString, QByteArray> files;
    files[QStringLiteral(".disk/diskanvil-live")] = "DiskAnvil Live\nversion=0.0.2\nbuilt=2026-10-12\nid=test\n";
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
    const QString iso = realIso.isEmpty() ? dir.filePath(QStringLiteral("rescue.iso")) : realIso;
    if (realIso.isEmpty() && sh(QStringLiteral("xorriso"), {QStringLiteral("-as"), QStringLiteral("mkisofs"), QStringLiteral("-quiet"), QStringLiteral("-J"), QStringLiteral("-joliet-long"),
                                                            QStringLiteral("-R"), QStringLiteral("-o"), iso, tree}) != 0) {
        out << "SKIP  Live USB layout: xorriso isn't installed" << Qt::endl;
        return;
    }
    const QString stickImage = keptStick.isEmpty() ? dir.filePath(QStringLiteral("stick.img")) : keptStick;
    QFile::remove(stickImage);
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), realIso.isEmpty() ? QStringLiteral("5G") : QStringLiteral("8G"), stickImage});
    const QString loopPath = userLoop(stickImage);
    report(!loopPath.isEmpty(), QStringLiteral("Live USB layout: a 5 GB loop device stands in for the stick"));
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
    writeBiosBootTo(stickImage);
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
        RescueUsbDialog dialog(&udisks, loopPath);
        dialog.show();
        dialog.findChild<QLineEdit *>(QStringLiteral("image"))->setText(iso);
        dialog.findChild<QCheckBox *>(QStringLiteral("keep"))->setChecked(true);
        dialog.findChild<QSpinBox *>(QStringLiteral("keepSize"))->setValue(1);
        dialog.findChild<QCheckBox *>(QStringLiteral("files"))->setChecked(true);
        dialog.findChild<QLineEdit *>(QStringLiteral("confirm"))->setText(shortDevice(device));
        findButton(&dialog, QStringLiteral("Make the DiskAnvil Live USB"))->click();
        waitUntil([&] { return !boxes.isEmpty(); }, 180000);
    }
    report(boxes.value(0).startsWith(QLatin1String("The DiskAnvil Live USB is ready")), QStringLiteral("Live USB layout: made"), boxes.join(QStringLiteral(" | ")).left(200));
    QStringList parts;
    waitUntil([&] {
        udisks.refresh();
        stick = udisks.diskByPath(loopPath);
        parts.clear();
        for (const Volume &v : stick ? stick->volumes : QVector<Volume>())
            parts << QStringLiteral("%1:%2:%3").arg(v.fsType, v.label).arg(v.size / (1024 * 1024));
        return parts.size() == 3;
    }, 20000);
    const QString systemMiB = QString::number(rescue::partitionSize(rescue::inspect(iso).bytes) / (1024 * 1024));
    report(parts.size() == 3 && parts[0] == QStringLiteral("vfat:DISKANVIL:") + systemMiB && parts[1] == QLatin1String("ext4:persistence:1024")
               && parts[2].startsWith(QLatin1String("exfat:ANVIL-FILES:")),
           QStringLiteral("Live USB layout: DiskAnvil Live's 2 GB, 1 GB keeping changes, and the files drive on the rest"), parts.join(QStringLiteral(", ")));
    const QString persistence = mountStick(udisks, loopPath, QStringLiteral("persistence"));
    QFile conf(persistence + QStringLiteral("/persistence.conf"));
    report(conf.open(QIODevice::ReadOnly) && conf.readAll() == "/ union\n", QStringLiteral("Live USB layout: the persistence partition has live-boot's persistence.conf"));
    conf.close();
    // Things that have to stay: a kept change, a saved file, a log; and one that has to go.
    { QFile f(persistence + QStringLiteral("/kept-change.txt")); if (f.open(QIODevice::WriteOnly)) f.write("kept"); }
    const QString filesRoot = mountStick(udisks, loopPath, QStringLiteral("ANVIL-FILES"));
    report(QFile::exists(filesRoot + QStringLiteral("/ISOs/ISO files go here.txt")),
           QStringLiteral("Live USB layout: the files drive has the ISOs folder the boot menu looks in"));
    { QFile f(filesRoot + QStringLiteral("/saved.txt")); if (f.open(QIODevice::WriteOnly)) f.write("saved"); }
    const QString system = mountStick(udisks, loopPath, QStringLiteral("DISKANVIL"));
    QDir().mkpath(system + QStringLiteral("/logs/2026-10-12 09.00"));
    { QFile f(system + QStringLiteral("/logs/2026-10-12 09.00/boot.log")); if (f.open(QIODevice::WriteOnly)) f.write("log"); }
    { QFile f(system + QStringLiteral("/old-version-file.txt")); if (f.open(QIODevice::WriteOnly)) f.write("old"); }
    sh(QStringLiteral("sync"), {});
    boxes.clear();
    {
        RescueUsbDialog dialog(&udisks, loopPath);
        dialog.show();
        dialog.findChild<QLineEdit *>(QStringLiteral("image"))->setText(iso);
        auto *update = dialog.findChild<QCheckBox *>(QStringLiteral("update"));
        report(update && update->isVisibleTo(&dialog), QStringLiteral("Live USB layout: opened again, it offers to update only"));
        update->setChecked(true);
        auto *make = findButton(&dialog, QStringLiteral("Make the DiskAnvil Live USB"));
        report(make->isEnabled(), QStringLiteral("Live USB layout: an update needs no name typed (nothing else is erased)"));
        make->click();
        waitUntil([&] { return !boxes.isEmpty(); }, 180000);
    }
    report(boxes.value(0).startsWith(QLatin1String("The DiskAnvil Live USB is ready")), QStringLiteral("Live USB layout: updated"), boxes.join(QStringLiteral(" | ")).left(200));
    const QString systemAgain = mountStick(udisks, loopPath, QStringLiteral("DISKANVIL"));
    const QString persistenceAgain = mountStick(udisks, loopPath, QStringLiteral("persistence"));
    const QString filesAgain = mountStick(udisks, loopPath, QStringLiteral("ANVIL-FILES"));
    report(!QFile::exists(systemAgain + QStringLiteral("/old-version-file.txt")) && QFile::exists(systemAgain + QStringLiteral("/logs/2026-10-12 09.00/boot.log"))
               && QFile::exists(systemAgain + QStringLiteral("/live/filesystem.squashfs")) && QFile::exists(persistenceAgain + QStringLiteral("/kept-change.txt"))
               && QFile::exists(filesAgain + QStringLiteral("/saved.txt")) && QFile::exists(persistenceAgain + QStringLiteral("/persistence.conf")),
           QStringLiteral("Live USB layout: the update replaced DiskAnvil Live's files, and kept its logs, the changes and the files"));
    if (!realIso.isEmpty()) {
        // Kept as it is, for a VM: it isn't made again with only the files drive.
        RescueUsbDialog::allowLoopDevicesForTest = false;
        RescueUsbDialog::openStickForTest = nullptr;
        cleanUpLoop(udisks, loopPath);
        out << "kept the stick at " << stickImage << Qt::endl;
        return;
    }
    // Made again with only the files drive: two partitions, and no persistence.conf anywhere.
    boxes.clear();
    {
        RescueUsbDialog dialog(&udisks, loopPath);
        dialog.show();
        dialog.findChild<QLineEdit *>(QStringLiteral("image"))->setText(iso);
        dialog.findChild<QCheckBox *>(QStringLiteral("update"))->setChecked(false);
        dialog.findChild<QCheckBox *>(QStringLiteral("keep"))->setChecked(false);
        dialog.findChild<QCheckBox *>(QStringLiteral("files"))->setChecked(true);
        dialog.findChild<QLineEdit *>(QStringLiteral("confirm"))->setText(shortDevice(device));
        findButton(&dialog, QStringLiteral("Make the DiskAnvil Live USB"))->click();
        waitUntil([&] { return !boxes.isEmpty(); }, 180000);
    }
    waitUntil([&] {
        udisks.refresh();
        stick = udisks.diskByPath(loopPath);
        parts.clear();
        for (const Volume &v : stick ? stick->volumes : QVector<Volume>())
            parts << QStringLiteral("%1:%2").arg(v.fsType, v.label);
        return parts.size() == 2;
    }, 20000);
    const QString filesOnly = mountStick(udisks, loopPath, QStringLiteral("ANVIL-FILES"));
    report(boxes.value(0).startsWith(QLatin1String("The DiskAnvil Live USB is ready")) && parts == QStringList{QStringLiteral("vfat:DISKANVIL"), QStringLiteral("exfat:ANVIL-FILES")}
               && QFile::exists(filesOnly + QStringLiteral("/ISOs/ISO files go here.txt")) && !QFile::exists(filesOnly + QStringLiteral("/persistence.conf")),
           QStringLiteral("Live USB layout: with only the files drive, it's DiskAnvil Live and the files drive"), (boxes + parts).join(QStringLiteral(" | ")).left(300));
    RescueUsbDialog::allowLoopDevicesForTest = false;
    RescueUsbDialog::openStickForTest = nullptr;
    cleanUpLoop(udisks, loopPath);
}

// Write Image to USB's "copy the files" way, with persistence, on a loop device of the user's
// own (no raw access needed, so no password): a Debian-live-like ISO with a long label.
// DISKANVIL_TEST_COPY_ISO uses a real ISO instead (one with live-boot persistence, like
// DiskAnvil Live), and DISKANVIL_TEST_STICK keeps the stick's image to boot it in a VM.
void isoCopy()
{
    QTemporaryDir dir;
    const QString realIso = qEnvironmentVariable("DISKANVIL_TEST_COPY_ISO");
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
    QString image = realIso.isEmpty() ? QString() : qEnvironmentVariable("DISKANVIL_TEST_STICK");
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
// and the ISO's own loop device gone at the end. DISKANVIL_TEST_WINDOWS_ISO uses a real
// Windows ISO instead (split at the real size), and DISKANVIL_TEST_STICK keeps the stick's
// image, to start Windows Setup from it in a VM.
void windowsUsb()
{
    const QString realIso = qEnvironmentVariable("DISKANVIL_TEST_WINDOWS_ISO");
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
    QString image = realIso.isEmpty() ? QString() : qEnvironmentVariable("DISKANVIL_TEST_STICK");
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

    // A dialog that goes without being closed (DiskAnvil quitting with it open) closes it too.
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

// The home screen (DiskAnvil Live's desktop, and Tools → Quick Fixes on a PC): which tiles are there,
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
    desktopFile("diskanvil-live-testdisk", "qterminal -e sudo testdisk %f");
    desktopFile("diskanvil-live-firmware", "systemctl reboot --firmware-setup");
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
        report(copy && copy->text() == QLatin1String("Make a DiskAnvil Live USB"), QStringLiteral("on a PC the stick tile makes a DiskAnvil Live USB"),
               copy ? copy->text() : QString());
        started.clear();
        if (auto *write = fixes.findChild<QAbstractButton *>(QStringLiteral("tile-writeimage")))
            write->click();
        report(started.size() == 1 && started[0].mid(1) == QStringList{QStringLiteral("--open"), QStringLiteral("write-image")}
                   && started[0][0] == QCoreApplication::applicationFilePath(),
               QStringLiteral("a tool tile starts DiskAnvil with that tool"), started.value(0).join(QLatin1Char(' ')));
    }
    {
        HomeWindow home(&udisks, HomeWindow::Mode::Desktop);
        const QStringList tiles = ids(home);
        report(tiles.contains(QLatin1String("drives")) && tiles.contains(QLatin1String("terminal")) && tiles.contains(QLatin1String("firmware"))
                   && !tiles.contains(QLatin1String("web")),
               QStringLiteral("the home screen: DiskAnvil, the tools and the programs that are there"), tiles.join(QLatin1Char(' ')));
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

        // A 1280x800 screen less the taskbar: every tile shows, none under the bottom edge.
        home.resize(1280, 752);
        home.show();
        QCoreApplication::processEvents();
        QStringList under;
        for (QAbstractButton *tile : home.findChildren<QAbstractButton *>()) {
            if (tile->objectName().startsWith(QLatin1String("tile-")) && tile->mapTo(&home, QPoint(0, tile->height())).y() > home.height())
                under << tile->objectName();
        }
        report(under.isEmpty() && home.layout()->minimumSize().height() <= 752,
               QStringLiteral("the home screen fits a 1280x800 screen with its taskbar"),
               QStringLiteral("needs %1 px; under the edge: %2").arg(home.layout()->minimumSize().height()).arg(under.join(QLatin1Char(' '))));
        home.hide();

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
// A ZIP whose files are stored as they are, for the office previews.
QByteArray storedZip(const QList<QPair<QByteArray, QByteArray>> &files)
{
    auto le16 = [](QByteArray &d, quint16 v) { d.append(char(v)).append(char(v >> 8)); };
    auto le32 = [](QByteArray &d, quint32 v) { d.append(char(v)).append(char(v >> 8)).append(char(v >> 16)).append(char(v >> 24)); };
    QByteArray out, central;
    for (const auto &[name, data] : files) {
        const quint32 crc = filetypes::crc32(reinterpret_cast<const uchar *>(data.constData()), data.size());
        const quint32 at = quint32(out.size());
        QByteArray local("PK\3\4", 4);
        le16(local, 10); le16(local, 0); le16(local, 0); le16(local, 0); le16(local, 0);
        le32(local, crc); le32(local, quint32(data.size())); le32(local, quint32(data.size()));
        le16(local, quint16(name.size())); le16(local, 0);
        out += local + name + data;
        QByteArray entry("PK\1\2", 4);
        le16(entry, 20); le16(entry, 10); le16(entry, 0); le16(entry, 0); le16(entry, 0); le16(entry, 0);
        le32(entry, crc); le32(entry, quint32(data.size())); le32(entry, quint32(data.size()));
        le16(entry, quint16(name.size())); le16(entry, 0); le16(entry, 0); le16(entry, 0); le16(entry, 0);
        le32(entry, 0); le32(entry, at);
        central += entry + name;
    }
    QByteArray end("PK\5\6", 4);
    le16(end, 0); le16(end, 0); le16(end, quint16(files.size())); le16(end, quint16(files.size()));
    le32(end, quint32(central.size())); le32(end, quint32(out.size()));
    le16(end, 0);
    return out + central + end;
}

// Previews come from the helper process (in its sandbox, where bwrap is here): a PDF's first
// page, a Word document's text, an MP3's cover and tags; a broken file gets no picture but
// doesn't take the helper down with it.
void previewsThroughHelper()
{
    QByteArray pdf;
    {
        QBuffer buffer(&pdf);
        buffer.open(QIODevice::WriteOnly);
        QPdfWriter writer(&buffer);
        writer.setTitle(QStringLiteral("Bank letter"));
        QPainter p(&writer);
        p.setFont(QFont(QStringLiteral("DejaVu Sans"), 24));
        p.drawText(QRect(0, 0, 4000, 1000), QStringLiteral("Your account is fine."));
    }
    const QByteArray docx = storedZip({{"word/document.xml", "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
                                                             "<w:body><w:p><w:r><w:t>Dear landlord,</w:t></w:r></w:p></w:body></w:document>"},
                                       {"docProps/core.xml", "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
                                                             "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\">"
                                                             "<dc:creator>Danny</dc:creator><dcterms:created>2024-07-14T13:22:05Z</dcterms:created>"
                                                             "</cp:coreProperties>"}});
    QByteArray cover;
    {
        QImage image(64, 64, QImage::Format_RGB32);
        image.fill(QColor(0x2b, 0xee, 0x8a));
        QBuffer buffer(&cover);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
    }
    QByteArray frames;
    for (int i = 0; i < 50; ++i)
        frames += QByteArray("\xFF\xFB\x90\x00", 4) + QByteArray(413, '\x55');
    auto frame = [](const char *id, const QByteArray &body) {
        QByteArray f(id, 4);
        const quint32 n = quint32(body.size());
        f.append(char(n >> 24)).append(char(n >> 16)).append(char(n >> 8)).append(char(n));
        return f + QByteArray(2, '\0') + body;
    };
    const QByteArray tags = frame("TIT2", QByteArray("\3Lost song", 10)) + frame("APIC", QByteArray("\0image/png\0\3\0", 13) + cover);
    const quint32 n = quint32(tags.size());
    const QByteArray mp3 = QByteArray("ID3\3\0\0", 6) + char((n >> 21) & 0x7F) + char((n >> 14) & 0x7F) + char((n >> 7) & 0x7F) + char(n & 0x7F) + tags + frames;
    const QByteArray broken = QByteArray("\xFF\xD8\xFF\xE0", 4) + QByteArray(5000, '\x13');

    // All of them one after the other on a pretend drive.
    struct Sample {
        QString name;
        QByteArray data;
    };
    const QVector<Sample> samples = {{QStringLiteral("letter.pdf"), pdf}, {QStringLiteral("rent.docx"), docx}, {QStringLiteral("song.mp3"), mp3},
                                     {QStringLiteral("broken.jpg"), broken}};
    QByteArray drive;
    QVector<lost::Found> files;
    for (const Sample &sample : samples) {
        lost::Found f;
        f.name = sample.name;
        f.type = filetypes::typeForName(sample.name);
        f.size = quint64(sample.data.size());
        f.extents = {{quint64(drive.size()), f.size}};
        files << f;
        drive += sample.data + QByteArray(4096 - sample.data.size() % 4096, '\0');
    }
    auto *loader = new PreviewLoader(lost::Source::fromMemory(drive));
    QHash<int, std::tuple<QImage, QString, bool>> results;
    QHash<int, QStringList> facts;
    QObject context;
    QObject::connect(loader, &PreviewLoader::ready, &context,
                     [&results, &facts](int index, int, const QImage &image, const QString &text, const QStringList &said, bool failed) {
                         results.insert(index, {image, text, failed});
                         facts.insert(index, said);
                     });
    QThread *thread = startOnThread(&context, loader);
    for (int i = int(files.size()) - 1; i >= 0; --i)
        loader->want(i, files[i], 480);
    waitUntil([&] { return results.size() == files.size(); }, 60000);
    loader->stop();
    thread->quit();
    thread->wait();

    const auto [pdfImage, pdfText, pdfFailed] = results.value(0);
    report(!pdfImage.isNull() && pdfText.contains(QLatin1String("Bank letter")) && pdfText.contains(QLatin1String("1 page")),
           QStringLiteral("Find Lost Files: a PDF shows its first page and title"), QString(pdfText).replace(QLatin1Char('\n'), QLatin1String(" / ")));
    report(facts.value(0).contains(QStringLiteral("Title\tBank letter")), QStringLiteral("and what it says about itself"),
           facts.value(0).join(QStringLiteral(" | ")).left(100));
    report(std::get<1>(results.value(1)) == QLatin1String("Dear landlord,"), QStringLiteral("a Word document shows its text"), std::get<1>(results.value(1)));
    report(facts.value(1).contains(QStringLiteral("Author\tDanny")) && facts.value(1).join(QString()).contains(QLatin1String("2024")),
           QStringLiteral("and who wrote it and when"), facts.value(1).join(QStringLiteral(" | ")));
    const auto [songImage, songText, songFailed] = results.value(2);
    report(!songImage.isNull() && songImage.pixel(5, 5) == QColor(0x2b, 0xee, 0x8a).rgb() && songText.contains(QLatin1String("Lost song")),
           QStringLiteral("an MP3 shows its cover and title"), QString(songText).replace(QLatin1Char('\n'), QLatin1String(" / ")));
    report(std::get<0>(results.value(3)).isNull() && results.size() == 4, QStringLiteral("a broken picture shows nothing and the helper carries on"));
    const QStringList command = PreviewLoader::helperCommand(true);
    if (!QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty())
        report(command.first().endsWith(QLatin1String("bwrap")) && command.contains(QStringLiteral("--unshare-all")),
               QStringLiteral("the helper runs in a bubblewrap sandbox"), command.first());
}

// The viewer's window: each kind of file shown its own way, and a damaged one as its bytes, saying why.
void viewerWindow()
{
    QTemporaryDir dir;
    QImage photo(600, 400, QImage::Format_RGB32);
    photo.fill(QColor(0x1f, 0x6f, 0xff));
    photo.save(dir.filePath(QStringLiteral("photo.png")));
    {
        QPdfWriter writer(dir.filePath(QStringLiteral("three.pdf")));
        QPainter p(&writer);
        for (int page = 1; page <= 3; ++page) {
            if (page > 1)
                writer.newPage();
            p.drawText(QRect(0, 0, 4000, 1000), QStringLiteral("Page %1").arg(page));
        }
    }
    const auto write = [&dir](const char *name, const QByteArray &data) {
        QFile f(dir.filePath(QString::fromLatin1(name)));
        if (f.open(QIODevice::WriteOnly))
            f.write(data);
        return f.fileName();
    };
    write("notes.txt", "Shopping list\nMilk\n");
    write("broken.jpg", QByteArray("\xFF\xD8\xFF\xE0", 4) + QByteArray(5000, '\x13'));
    write("rent.docx", storedZip({{"word/document.xml", "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
                                                        "<w:body><w:p><w:r><w:t>Dear landlord,</w:t></w:r></w:p></w:body></w:document>"}}));
    QByteArray wav("RIFF", 4);
    const QByteArray samples(8000, '\x80');
    const auto le32 = [](quint32 v) {
        QByteArray b(4, '\0');
        for (int i = 0; i < 4; ++i)
            b[i] = char(v >> (8 * i));
        return b;
    };
    wav += le32(36 + quint32(samples.size())) + QByteArray("WAVEfmt ", 8) + le32(16) + QByteArray("\x01\x00\x01\x00", 4) + le32(8000) + le32(8000)
         + QByteArray("\x01\x00\x08\x00", 4) + QByteArray("data", 4) + le32(quint32(samples.size())) + samples;
    write("beep.wav", wav);

    viewer::Window window;
    window.resize(1000, 700);
    window.show();
    const auto show = [&](const char *name) {
        viewer::Show s;
        s.name = QString::fromLatin1(name);
        s.type = filetypes::typeForName(s.name);
        s.kind = viewer::kindFor(s.type, s.name);
        s.path = dir.filePath(s.name);
        s.where = QStringLiteral("1 of 6");
        s.facts = {QStringLiteral("Created\tyesterday")};
        window.present(s);
        QCoreApplication::processEvents();
    };
    show("photo.png");
    report(window.shownKind() == viewer::Kind::Picture && window.problem().isEmpty(), QStringLiteral("viewer: a photo is shown"));
    show("three.pdf");
    report(window.shownKind() == viewer::Kind::Pdf && window.problem().isEmpty(), QStringLiteral("viewer: a PDF is shown page by page"),
           window.problem());
    const auto visibleText = [&window] {
        for (QPlainTextEdit *t : window.findChildren<QPlainTextEdit *>()) {
            if (t->isVisible())
                return t->toPlainText();
        }
        return QString();
    };
    show("notes.txt");
    report(window.shownKind() == viewer::Kind::Words && visibleText().startsWith(QLatin1String("Shopping list")),
           QStringLiteral("viewer: a text file is shown as words"));
    show("rent.docx");
    report(window.shownKind() == viewer::Kind::Words && visibleText().contains(QLatin1String("Dear landlord")),
           QStringLiteral("viewer: a Word document's words"));
    show("beep.wav");
    report(window.shownKind() == viewer::Kind::Song, QStringLiteral("viewer: a song gets the player"));
    show("broken.jpg");
    auto *parts = window.findChild<QTreeWidget *>(QStringLiteral("parts"));
    waitUntil([&] { return parts && parts->topLevelItemCount() > 0; }, 10000);
    QStringList partNames;
    bool damaged = false;
    for (int i = 0; parts && i < parts->topLevelItemCount(); ++i) {
        partNames << parts->topLevelItem(i)->text(0);
        damaged = damaged || parts->topLevelItem(i)->toolTip(0).contains(QLatin1String("damaged from here on"));
    }
    report(window.shownKind() == viewer::Kind::Bytes && window.problem().contains(QLatin1String("can't be shown")) && window.insideView()->isVisible()
               && partNames.value(0) == QStringLiteral("SOI · Start of the picture") && damaged,
           QStringLiteral("viewer: a damaged picture shows what it's made of and where it's damaged, and says why"),
           window.problem().left(60) + QStringLiteral(" | ") + partNames.join(QStringLiteral(", ")));
    auto *facts = window.findChild<QLabel *>(QStringLiteral("facts"));
    report(facts && facts->text().contains(QLatin1String("yesterday")), QStringLiteral("viewer: what's known about it is beside it"));

    // Changing bytes: typed over in hex, undone, typed again, and handed out to be saved.
    auto *hex = window.findChild<HexView *>(QStringLiteral("hex"));
    auto *change = window.findChild<QPushButton *>(QStringLiteral("changeBytes"));
    QMap<quint64, quint8> handed;
    QObject::connect(&window, &viewer::Window::saveEdited, &window, [&handed](const QMap<quint64, quint8> &edits) { handed = edits; });
    change->click();
    hex->moveTo(4);
    const auto type = [hex](const QString &keys) {
        for (const QChar c : keys) {
            QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
            QCoreApplication::sendEvent(hex, &press);
        }
    };
    type(QStringLiteral("4142"));
    const bool typed = hex->edits().size() == 2 && hex->edits().value(4) == 0x41 && hex->edits().value(5) == 0x42;
    hex->undo();
    hex->undo();
    const bool undone = hex->edits().size() == 1;
    hex->moveTo(5);
    type(QStringLiteral("7a"));
    window.findChild<QPushButton *>(QStringLiteral("saveEdited"))->click();
    report(typed && undone && handed.size() == 2 && handed.value(4) == 0x41 && handed.value(5) == 0x7A,
           QStringLiteral("viewer: bytes are changed in a copy by typing over them, undone, and handed out to be saved"),
           QStringLiteral("%1 changes").arg(handed.size()));
    window.findChild<QAction *>(QStringLiteral("insideAction"))->setChecked(false);
    report(!window.insideView()->isVisible(), QStringLiteral("viewer: Inside turns off again"));
    show("photo.png");
    window.findChild<QAction *>(QStringLiteral("insideAction"))->setChecked(true);
    waitUntil([&] { return parts->topLevelItemCount() > 0; }, 10000);
    report(window.insideView()->isVisible() && parts->topLevelItem(1) && parts->topLevelItem(1)->text(0).startsWith(QStringLiteral("IHDR · 600 × 400")),
           QStringLiteral("viewer: Inside shows any file's parts, a photo's too"),
           QStringLiteral("%1 %2 %3").arg(window.insideView()->isVisible()).arg(window.insideView()->isVisibleTo(&window)).arg(parts->topLevelItem(1) ? parts->topLevelItem(1)->text(0) : QString()));
}

// A compressed test file unpacked to `path`; empty when it couldn't be.
QString unpackTestFile(const QString &packed, const QString &path)
{
    QString error;
    const auto image = ImageSource::open(packed, &error);
    QFile out(path);
    if (!image || !out.open(QIODevice::WriteOnly)) {
        report(false, QStringLiteral("unpack %1").arg(QFileInfo(packed).fileName()), error);
        return {};
    }
    // Sparse: what's only zeros is left as a hole, so a big, mostly empty image takes up little
    // (in memory too, where the temporary folder is).
    QByteArray buffer(64 * 1024, Qt::Uninitialized);
    qint64 size = 0;
    for (bool more = true; more;) {
        qint64 filled = 0;
        for (qint64 n = 1; filled < buffer.size() && n > 0; filled += std::max<qint64>(n, 0))
            n = image->read(buffer.data() + filled, buffer.size() - filled);
        more = filled == buffer.size();
        if (buffer.left(filled).count('\0') != filled) {
            out.seek(size);
            out.write(buffer.constData(), filled);
        }
        size += filled;
    }
    out.resize(size);
    return path;
}

// One of tests/data/lostfs's images, unpacked into `dir`; empty when it couldn't be.
QString unpackLostFsImage(const QString &name, const QTemporaryDir &dir)
{
    return unpackTestFile(QStringLiteral(SOURCE_DIR "/tests/data/lostfs/") + name + QStringLiteral(".img.zst"), dir.filePath(name + QStringLiteral(".img")));
}

// A FAT32 image with deleted files (tests/data/lostfs): they show with their names, under their
// folders, and the deleted folder's files are found by picking it in the tree.
void lostFilesFromFileSystem()
{
    QTemporaryDir dir;
    const QString path = unpackLostFsImage(QStringLiteral("fat32"), dir);
    if (path.isEmpty())
        return;

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
    report(details->text().contains(QLatin1String("On the drive")) && details->text().contains(QLatin1String("Created"))
               && details->text().contains(QLatin1String("Last opened")),
           QStringLiteral("and when it was created, changed and last opened, from FAT's own records"));

    // DISKANVIL_UITEST_SHOTS=folder: a picture of the window as it is now, to look at.
    if (qEnvironmentVariableIsSet("DISKANVIL_UITEST_SHOTS")) {
        folders->setCurrentItem(folders->topLevelItem(0));
        QApplication::processEvents();
        dialog.grab().save(qEnvironmentVariable("DISKANVIL_UITEST_SHOTS") + QStringLiteral("/lostfiles-fat32.png"));
        folders->setCurrentItem(oldStuff);
    }

    // Unticking "Only deleted files" shows the two that are still there too.
    auto *deletedOnly = dialog.findChild<QCheckBox *>(QStringLiteral("deletedOnly"));
    folders->setCurrentItem(folders->topLevelItem(0));
    deletedOnly->setChecked(false);
    report(view->model()->rowCount() == 8, QStringLiteral("and the files that weren't deleted show when asked"),
           QString::number(view->model()->rowCount()));
}

// Virtual machines' disk files (tests/data/vdisk): Find Lost Files reads a VHDX as the disk inside
// it, and turns down one that only holds the changes made to another, saying which.
void lostFilesFromVirtualDisk()
{
    QTemporaryDir dir;
    for (const char *name : {"dynamic.vhdx", "child.qcow2"})
        unpackTestFile(QStringLiteral(SOURCE_DIR "/tests/data/vdisk/") + QLatin1String(name) + QStringLiteral(".zst"), dir.filePath(QLatin1String(name)));
    UDisks udisks;
    LostFilesDialog dialog(&udisks, QString());
    dialog.show();
    dialog.scanImage(dir.filePath(QStringLiteral("child.qcow2")));
    auto *note = dialog.findChild<QLabel *>(QStringLiteral("driveNote"));
    report(note->text().contains(QLatin1String("child.qcow2 can't be looked through")) && note->text().contains(QLatin1String("plain.qcow2")),
           QStringLiteral("Find Lost Files: a disk that's only changes to another says which one it needs"), note->text());
    dialog.scanImage(dir.filePath(QStringLiteral("dynamic.vhdx")));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("status"));
    waitUntil([&] { return status->text().startsWith(QLatin1String("Done")); }, 60000);
    dialog.findChild<QCheckBox *>(QStringLiteral("deletedOnly"))->setChecked(false);
    auto *view = dialog.findChild<QListView *>(QStringLiteral("files"));
    QStringList names;
    for (int r = 0; r < view->model()->rowCount(); ++r)
        names << view->model()->index(r, 0).data().toString().toLower();
    names.sort();
    report(status->text().contains(QLatin1String("dynamic.vhdx")) && names == QStringList({QStringLiteral("notes.txt"), QStringLiteral("numbers.txt")}),
           QStringLiteral("a VHDX is read as the disk inside it"), status->text() + QStringLiteral(" / ") + names.join(QStringLiteral(", ")));

    // DiskAnvil's own backup of a FAT drive, opened as it is: read a piece at a time.
    const QString fat = dir.filePath(QStringLiteral("stick.img"));
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("40M"), fat});
    sh(QStringLiteral("mkfs.fat"), {QStringLiteral("-F"), QStringLiteral("32"), fat});
    {
        QFile note(dir.filePath(QStringLiteral("holiday.txt")));
        if (note.open(QIODevice::WriteOnly))
            note.write("kept in a backup\n");
    }
    sh(QStringLiteral("mcopy"), {QStringLiteral("-i"), fat, dir.filePath(QStringLiteral("holiday.txt")), QStringLiteral("::/holiday.txt")});
    const QString backup = dir.filePath(QStringLiteral("stick.img.zst"));
    {
        BackupInfo info;
        info.kind = QStringLiteral("disk");
        info.size = quint64(QFileInfo(fat).size());
        BackupJob job(blockio::openFile(fat, blockio::FileMode::Read), backup, info);
        job.run();
    }
    dialog.scanImage(backup);
    waitUntil([&] { return status->text().startsWith(QLatin1String("Done")) && status->text().contains(QLatin1String("stick.img.zst")); }, 60000);
    names.clear();
    for (int r = 0; r < view->model()->rowCount(); ++r)
        names << view->model()->index(r, 0).data().toString().toLower();
    report(names.contains(QLatin1String("holiday.txt")), QStringLiteral("a DiskAnvil backup is looked through without restoring it"),
           status->text() + QStringLiteral(" / ") + names.join(QStringLiteral(", ")));
}

// Windows' earlier copies of a drive (tests/data/lostfs/shadow-job.ps1): once the drive's file
// systems are read, the window says Windows kept two; looking in them adds what has changed or
// gone since, in folders named after the copies, read from the copies themselves.
void lostFilesFromShadowCopies()
{
    QTemporaryDir dir;
    const QString path = unpackLostFsImage(QStringLiteral("ntfs-shadow"), dir);
    if (path.isEmpty())
        return;
    UDisks udisks;
    LostFilesDialog dialog(&udisks, QString());
    dialog.show();
    dialog.scanImage(path);
    auto *copies = dialog.findChild<QLabel *>(QStringLiteral("copies"));
    waitUntil([&] { return copies->isVisible(); }, 60000);
    report(copies->text().startsWith(QLatin1String("Windows kept 2 earlier versions of this drive")),
           QStringLiteral("Find Lost Files: it says Windows kept earlier versions of the drive"), copies->text());
    dialog.findChild<QPushButton *>(QStringLiteral("lookInCopies"))->click();
    waitUntil([&] { return copies->text().startsWith(QLatin1String("Changed or gone since")) || copies->text().contains(QLatin1String("the same")); },
              60000);
    report(copies->text().startsWith(QLatin1String("Changed or gone since the earlier versions: 7 files")),
           QStringLiteral("looking in them finds what's changed or gone since"), copies->text());

    auto *status = dialog.findChild<QLabel *>(QStringLiteral("status"));
    waitUntil([&] { return status->text().startsWith(QLatin1String("Done")); }, 180000);
    auto *folders = dialog.findChild<QTreeWidget *>(QStringLiteral("folders"));
    QList<QTreeWidgetItem *> roots;
    for (int i = 0; i < folders->topLevelItemCount(); ++i) {
        if (folders->topLevelItem(i)->data(0, Qt::UserRole).toString().startsWith(QLatin1String("WINVSS as it was on ")))
            roots << folders->topLevelItem(i);
    }
    report(roots.size() == 2, QStringLiteral("each earlier version has its own folder"), QString::number(roots.size()));
    if (roots.isEmpty())
        return;
    // The older one (its name sorts first): photo.jpg, gone from the drive since.
    std::sort(roots.begin(), roots.end(), [](QTreeWidgetItem *a, QTreeWidgetItem *b) { return a->data(0, Qt::UserRole).toString() < b->data(0, Qt::UserRole).toString(); });
    folders->setCurrentItem(roots.first());
    auto *view = dialog.findChild<QListView *>(QStringLiteral("files"));
    QStringList names;
    for (int r = 0; r < view->model()->rowCount(); ++r) {
        names << view->model()->index(r, 0).data().toString();
        if (names.last() == QLatin1String("photo.jpg"))
            view->setCurrentIndex(view->model()->index(r, 0));
    }
    names.sort();
    report(names == QStringList({QStringLiteral("big.bin"), QStringLiteral("letter.pdf"), QStringLiteral("notes.txt"), QStringLiteral("photo.jpg")}),
           QStringLiteral("the older version's folder has what's changed or gone since it"), names.join(QStringLiteral(", ")));
    auto *details = dialog.findChild<QLabel *>(QStringLiteral("details"));
    report(details->text().contains(QLatin1String("isn't on the drive anymore")), QStringLiteral("a file from an earlier version says where it's from"),
           details->text().left(300));
    if (qEnvironmentVariableIsSet("DISKANVIL_UITEST_SHOTS")) {
        waitUntil([] { return false; }, 1500); // its picture
        dialog.grab().save(qEnvironmentVariable("DISKANVIL_UITEST_SHOTS") + QStringLiteral("/lostfiles-copies.png"));
    }
}

// NTFS's change journal (tests/data/lostfs/journal-job.cmd): "Deleted recently" lists what's known
// to have been deleted when, newest first, and a file only its name is left of can't be ticked.
// The timeline: its rules on made-up files (dates that can't be real left out, oldest first,
// CSV quoted where needed), then on an NTFS drive whose change journal kept three deletions.
void timelineWindow()
{
    const qint64 day = 24LL * 3600 * 1000;
    const qint64 now = QDateTime(QDate(2026, 10, 12), QTime(12, 0), QTimeZone::UTC).toMSecsSinceEpoch();
    QVector<lost::Found> made(2);
    made[0].name = QStringLiteral("a, \"quoted\".txt");
    made[0].modified = now - 10 * day;
    made[0].created = now - 20 * day;
    made[0].accessed = 1000; // 1970: not real
    made[0].deleted = false;
    made[1].name = QStringLiteral("photo.jpg");
    made[1].inside = now - 30 * day;
    made[1].deletedAt = now - day;
    made[1].modified = now + 30 * day; // in the future: not real
    const QVector<timeline::Event> events = timeline::events(made, now);
    QStringList kinds;
    for (const timeline::Event &e : events)
        kinds << timeline::kindName(e.kind).section(QLatin1Char(' '), 0, 0);
    const QByteArray csv = timeline::csv(events, made);
    report(kinds == QStringList({QStringLiteral("Its"), QStringLiteral("Made"), QStringLiteral("Written"), QStringLiteral("Deleted")})
               && csv.contains("\"a, \"\"quoted\"\".txt\"") && csv.count('\n') == 5,
           QStringLiteral("timeline: oldest first, unreal dates left out, CSV quoted"), kinds.join(QLatin1Char(',')));

    QTemporaryDir dir;
    const QString path = unpackLostFsImage(QStringLiteral("ntfs-journal"), dir);
    if (path.isEmpty())
        return;
    UDisks udisks;
    LostFilesDialog lost(&udisks, QString());
    lost.show();
    lost.scanImage(path);
    auto *status = lost.findChild<QLabel *>(QStringLiteral("status"));
    waitUntil([&] { return status->text().startsWith(QLatin1String("Done")); }, 60000);
    TimelineDialog dialog(lost.files(), QStringLiteral("test drive"));
    dialog.show();
    const int all = dialog.shown();
    auto *view = dialog.findChild<QTableView *>(QStringLiteral("timeline"));
    int deletions = 0;
    for (int r = 0; r < view->model()->rowCount(); ++r)
        deletions += view->model()->index(r, 1).data().toString() == QLatin1String("Deleted") ? 1 : 0;
    dialog.findChild<QCheckBox *>(QStringLiteral("deletedOnly"))->setChecked(true);
    const int deletedFiles = dialog.shown();
    dialog.findChild<QLineEdit *>(QStringLiteral("words"))->setText(QStringLiteral("beach"));
    report(all > 10 && deletions >= 3 && deletedFiles < all && dialog.shown() > 0 && dialog.shown() < deletedFiles,
           QStringLiteral("timeline: the drive's events, its three deletions among them, filtered by deleted files and by name"),
           QStringLiteral("%1 events, %2 deletions, %3 of deleted files, %4 for beach").arg(all).arg(deletions).arg(deletedFiles).arg(dialog.shown()));
}

// A JPEG that says only where it was taken (EXIF's GPS folder), the way a phone writes it.
QByteArray gpsJpeg(double latitude, double longitude)
{
    QByteArray t("II*\0", 4);
    auto u16 = [](QByteArray &b, quint16 v) { b.append(char(v)).append(char(v >> 8)); };
    auto u32 = [](QByteArray &b, quint32 v) { b.append(char(v)).append(char(v >> 8)).append(char(v >> 16)).append(char(v >> 24)); };
    u32(t, 8);
    // IFD0: just the GPS folder's place (26 bytes on), then the GPS folder with four tags.
    u16(t, 1);
    u16(t, 0x8825), u16(t, 4), u32(t, 1), u32(t, 26);
    u32(t, 0);
    const quint32 values = 26 + 2 + 4 * 12 + 4;
    u16(t, 4);
    auto entry = [&](quint16 tag, quint16 type, quint32 count, quint32 value) { u16(t, tag), u16(t, type), u32(t, count), u32(t, value); };
    entry(1, 2, 2, latitude < 0 ? 'S' : 'N');
    entry(2, 5, 3, values);
    entry(3, 2, 2, longitude < 0 ? 'W' : 'E');
    entry(4, 5, 3, values + 24);
    u32(t, 0);
    for (const double v : {std::abs(latitude), std::abs(longitude)}) {
        const double minutes = (v - std::floor(v)) * 60;
        u32(t, quint32(v)), u32(t, 1), u32(t, quint32(minutes)), u32(t, 1), u32(t, quint32((minutes - std::floor(minutes)) * 60 * 1000)), u32(t, 1000);
    }
    const QByteArray app1 = QByteArray("Exif\0\0", 6) + t;
    const int length = int(app1.size()) + 2;
    return QByteArray("\xFF\xD8\xFF\xE1", 4) + char(length >> 8) + char(length & 0xFF) + app1 + QByteArray("\xFF\xDA\x00\x02\xFF\xD9", 6);
}

void placesWindow()
{
    // Three photos round Leeds, one in Paris, one in New York; one that doesn't say, and a document.
    const QVector<std::tuple<const char *, double, double>> spots = {{"leeds1.jpg", 53.7997, -1.5492}, {"leeds2.jpg", 53.8008, -1.5410},
                                                                     {"leeds3.jpg", 53.7950, -1.5600}, {"paris.jpg", 48.8584, 2.2945},
                                                                     {"newyork.jpg", 40.7484, -73.9857}};
    QVector<lost::Found> files;
    for (const auto &[name, lat, lon] : spots) {
        lost::Found f;
        f.name = QString::fromLatin1(name);
        f.type = filetypes::typeForName(f.name);
        f.data = gpsJpeg(lat, lon);
        f.size = quint64(f.data.size());
        files << f;
    }
    lost::Found plain;
    plain.name = QStringLiteral("nowhere.jpg");
    plain.type = filetypes::typeForName(plain.name);
    plain.data = QByteArray("\xFF\xD8\xFF\xD9", 4);
    plain.size = 4;
    lost::Found note;
    note.name = QStringLiteral("note.txt");
    note.type = filetypes::typeForName(note.name);
    note.data = "a note";
    note.size = 6;
    files << plain << note;
    PlacesDialog dialog(lost::Source::fromMemory(QByteArray(512, '\0')), files, QStringLiteral("test drive"));
    dialog.show();
    dialog.waitUntilDone();
    auto *places = dialog.findChild<QTreeWidget *>(QStringLiteral("places"));
    auto *list = dialog.findChild<QListWidget *>(QStringLiteral("files"));
    QTreeWidgetItem *top = places ? places->topLevelItem(0) : nullptr;
    report(dialog.located() == 5 && places && places->topLevelItemCount() == 3 && top && top->text(0).contains(QLatin1String("Leeds")) && top->text(1) == QLatin1String("3"),
           QStringLiteral("places: five photos say where, by place, the most first"), top ? top->text(0) + QLatin1Char(' ') + top->text(1) : QStringLiteral("none"));
    if (!top || !list)
        return;
    Q_EMIT places->itemClicked(top, 0);
    int viewed = -1;
    QObject::connect(&dialog, &PlacesDialog::view, [&viewed](int index) { viewed = index; });
    list->setCurrentRow(0);
    Q_EMIT list->itemActivated(list->currentItem());
    report(list->count() == 3 && viewed >= 0 && viewed <= 2, QStringLiteral("places: a place lists its photos, and one opens from there"),
           QStringLiteral("%1 listed, opened %2").arg(list->count()).arg(viewed));
    if (qEnvironmentVariableIsSet("DISKANVIL_UITEST_SHOTS")) {
        dialog.grab().save(qEnvironmentVariable("DISKANVIL_UITEST_SHOTS") + QStringLiteral("/places.png"));
        if (auto *map = dialog.findChild<PlacesMap *>(QStringLiteral("map"))) {
            map->showAll();
            dialog.grab().save(qEnvironmentVariable("DISKANVIL_UITEST_SHOTS") + QStringLiteral("/places-all.png"));
        }
    }
}

// The viewer's Enhance panel: sharpened, saved as a new picture that says what was done to it.
void enhanceWindow()
{
    QTemporaryDir dir;
    QImage stripes(320, 240, QImage::Format_RGB32);
    for (int y = 0; y < stripes.height(); ++y) {
        for (int x = 0; x < stripes.width(); ++x)
            stripes.setPixel(x, y, (x / 8) % 2 ? qRgb(200, 190, 180) : qRgb(40, 50, 60));
    }
    const QString path = dir.path() + QStringLiteral("/stripes.png");
    stripes.save(path);
    viewer::Window window;
    viewer::Show show;
    show.kind = viewer::Kind::Picture;
    show.type = filetypes::typeForName(QStringLiteral("stripes.png"));
    show.path = path;
    show.name = QStringLiteral("stripes.png");
    window.present(show);
    window.show();
    auto *action = window.findChild<QAction *>(QStringLiteral("enhanceAction"));
    report(action && action->isVisible(), QStringLiteral("viewer: Enhance is there for a picture"));
    if (!action)
        return;
    // On a small screen: icons only in the toolbar, and Details makes way for Enhance.
    window.resize(900, 600);
    QCoreApplication::processEvents();
    auto *bar = window.findChild<QToolBar *>();
    action->setChecked(true);
    QCoreApplication::processEvents();
    QAction *detailsAction = nullptr;
    for (QAction *a : bar->actions()) {
        if (a->text() == QLatin1String("Details"))
            detailsAction = a;
    }
    report(bar && bar->toolButtonStyle() == Qt::ToolButtonIconOnly && detailsAction && !detailsAction->isChecked(),
           QStringLiteral("viewer: on a small screen only icons fit, and Details makes way for Enhance"),
           QStringLiteral("window %1 wide, toolbar style %2, Details %3").arg(window.width()).arg(int(bar->toolButtonStyle()))
               .arg(detailsAction ? (detailsAction->isChecked() ? QStringLiteral("on") : QStringLiteral("off")) : QStringLiteral("missing")));
    window.resize(2400, 1000);
    QCoreApplication::processEvents();
    report(bar->toolButtonStyle() == Qt::ToolButtonTextBesideIcon, QStringLiteral("viewer: on a big one the words come back"));
    window.findChild<QSlider *>(QStringLiteral("sharpen"))->setValue(50);
    window.findChild<QSlider *>(QStringLiteral("brightness"))->setValue(20);
    QByteArray png;
    QString name;
    QObject::connect(&window, &viewer::Window::savePicture, [&](const QString &n, const QByteArray &p) {
        name = n;
        png = p;
    });
    window.findChild<QPushButton *>(QStringLiteral("saveEnhanced"))->click();
    waitUntil([&] { return !png.isEmpty(); }, 20000);
    QImage saved;
    saved.loadFromData(png, "PNG");
    report(name == QLatin1String("stripes (enhanced).png") && saved.size() == stripes.size() && saved.text(QStringLiteral("Description")).contains(QLatin1String("sharpened 50%"))
               && saved.text(QStringLiteral("Description")).contains(QLatin1String("brightness +20")) && qGray(saved.pixel(4, 4)) > qGray(stripes.pixel(4, 4)),
           QStringLiteral("viewer: an enhanced picture is saved new, saying what was done to it"), saved.text(QStringLiteral("Description")));

    // Profiles: DiskAnvil's own are listed, and choosing one sets the sliders.
    auto *profiles = window.findChild<QComboBox *>(QStringLiteral("profiles"));
    const int dark = profiles ? profiles->findText(QStringLiteral("Too dark")) : -1;
    report(dark > 0 && profiles->findText(QStringLiteral("Night camera")) > 0 && profiles->findText(QStringLiteral("Scanned page")) > 0,
           QStringLiteral("viewer: DiskAnvil's own enhance profiles are listed"));
    if (dark > 0) {
        profiles->setCurrentIndex(dark);
        Q_EMIT profiles->activated(dark);
    }
    report(window.findChild<QSlider *>(QStringLiteral("shadows"))->value() == 50 && window.findChild<QSlider *>(QStringLiteral("brightness"))->value() == 20,
           QStringLiteral("viewer: choosing a profile sets what it says"));
    window.findChild<QSlider *>(QStringLiteral("sharpen"))->setValue(30);
    report(profiles && profiles->currentIndex() == 0, QStringLiteral("viewer: changing a slider afterwards makes them your own settings again"));
    // Saved ones come from Find Lost Files (the viewer can't see the home folder) and are listed after.
    enhance::Profile mine;
    mine.name = QStringLiteral("My CCTV");
    mine.settings.contrast = 33;
    mine.settings.denoise = 2;
    window.setProfiles({enhance::profileToFile(mine), QByteArray("not a profile")}, QStringLiteral("Added 1 profile: My CCTV."));
    const int own = profiles ? profiles->findText(QStringLiteral("My CCTV")) : -1;
    report(own > dark && window.findChild<QLabel *>(QStringLiteral("profileNote"))->text().contains(QLatin1String("Added")),
           QStringLiteral("viewer: saved profiles are listed after DiskAnvil's, and what happened is said"));
    if (own > 0) {
        profiles->setCurrentIndex(own);
        Q_EMIT profiles->activated(own);
    }
    report(window.findChild<QSlider *>(QStringLiteral("contrast"))->value() == 33, QStringLiteral("viewer: a saved profile is used the same way"));
    // A profile file dropped on the viewer goes to Find Lost Files to be checked and kept.
    QStringList dropped;
    QObject::connect(&window, &viewer::Window::profileAsked,
                     [&](viewer::ProfileDo what, const QString &, const QByteArray &, const QStringList &files) {
                         if (what == viewer::ProfileDo::Dropped)
                             dropped = files;
                     });
    const QString given = dir.path() + QStringLiteral("/given.anvil-enhance");
    {
        QMimeData data;
        data.setUrls({QUrl::fromLocalFile(given), QUrl::fromLocalFile(dir.path() + QStringLiteral("/photo.jpg"))});
        QDragEnterEvent enter(QPointF(10, 10), Qt::CopyAction, &data, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &enter);
        QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &data, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &drop);
        report(enter.isAccepted() && dropped == QStringList{given}, QStringLiteral("viewer: a profile file dropped on it is passed on to be added (a photo isn't)"),
               dropped.join(QLatin1Char(' ')));
    }

    // A spot: a box drawn on the picture is shown on its own, and that's what's saved.
    auto *picture = window.findChild<QAbstractScrollArea *>(QStringLiteral("picture"));
    window.findChild<QPushButton *>(QStringLiteral("pickSpot"))->setChecked(true);
    QCoreApplication::processEvents();
    if (picture) {
        // The picture is shown whole (it's smaller than the window), centred: its corner, then 100 by 60 of it.
        QWidget *port = picture->viewport();
        const QPointF corner((port->width() - stripes.width()) / 2.0, (port->height() - stripes.height()) / 2.0);
        const auto mouse = [port](QEvent::Type type, QPointF at) {
            QMouseEvent e(type, at, port->mapToGlobal(at), Qt::LeftButton, type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(port, &e);
        };
        mouse(QEvent::MouseButtonPress, corner + QPointF(40, 50));
        mouse(QEvent::MouseMove, corner + QPointF(90, 80));
        mouse(QEvent::MouseMove, corner + QPointF(140, 110));
        mouse(QEvent::MouseButtonRelease, corner + QPointF(140, 110));
    }
    QCoreApplication::processEvents();
    png.clear();
    window.findChild<QPushButton *>(QStringLiteral("saveEnhanced"))->click();
    waitUntil([&] { return !png.isEmpty(); }, 20000);
    saved.loadFromData(png, "PNG");
    report(window.findChild<QPushButton *>(QStringLiteral("wholePicture"))->isEnabled() && name.contains(QLatin1String("(spot)"))
               && saved.width() == 100 * 4 && saved.height() == 60 * 4 && saved.text(QStringLiteral("Description")).contains(QStringLiteral("100 × 60 pixels from 40, 50")),
           QStringLiteral("viewer: a spot picked is saved on its own, bigger, saying where it was"),
           QStringLiteral("%1, %2x%3: %4").arg(name).arg(saved.width()).arg(saved.height()).arg(saved.text(QStringLiteral("Description"))));
    if (qEnvironmentVariableIsSet("DISKANVIL_UITEST_SHOTS"))
        window.grab().save(qEnvironmentVariable("DISKANVIL_UITEST_SHOTS") + QStringLiteral("/enhance-spot.png"));
    window.findChild<QPushButton *>(QStringLiteral("wholePicture"))->click();
    png.clear();
    window.findChild<QPushButton *>(QStringLiteral("saveEnhanced"))->click();
    waitUntil([&] { return !png.isEmpty(); }, 20000);
    saved.loadFromData(png, "PNG");
    report(saved.size() == stripes.size() && !name.contains(QLatin1String("spot")), QStringLiteral("viewer: Whole Picture goes back to all of it"),
           QStringLiteral("%1 %2x%3").arg(name).arg(saved.width()).arg(saved.height()));

    show.kind = viewer::Kind::Words;
    window.present(show);
    report(!action->isVisible() && !action->isChecked(), QStringLiteral("viewer: Enhance goes away for what isn't a picture"));
}

// The viewer with a film: frames shown and one saved; and a slowed-down copy made by ffmpeg.
void filmTools()
{
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (ffmpeg.isEmpty() || ffprobe.isEmpty()) {
        out << "SKIP  film tools: ffmpeg isn't installed (it makes the test film)" << Qt::endl;
        return;
    }
    QTemporaryDir dir;
    const QString film = dir.path() + QStringLiteral("/pattern.mp4");
    QProcess::execute(ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("lavfi"),
                               QStringLiteral("-i"), QStringLiteral("testsrc=duration=2:size=320x240:rate=25"), QStringLiteral("-f"), QStringLiteral("lavfi"),
                               QStringLiteral("-i"), QStringLiteral("sine=frequency=440:duration=2"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
                               QStringLiteral("-shortest"), film});
    const auto length = [&ffprobe](const QString &file) {
        QProcess probe;
        probe.start(ffprobe, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"), QStringLiteral("format=duration"),
                              QStringLiteral("-of"), QStringLiteral("csv=p=0"), file});
        probe.waitForFinished(30000);
        return probe.readAllStandardOutput().trimmed().toDouble();
    };
    report(QFileInfo(film).size() > 1000 && length(film) > 1.9, QStringLiteral("film tools: a 2-second test film to work on"));

    // A quarter of the speed, sharpened, in black and white: four times as long, no sound.
    enhance::Settings s;
    s.sharpen = 30;
    s.grey = true;
    const QString slow = dir.path() + QStringLiteral("/slow.mp4");
    QProcess run;
    run.start(ffmpeg, enhance::ffmpegArguments(film, slow, 0.25, s));
    run.waitForFinished(120000);
    QProcess streams;
    streams.start(ffprobe, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"), QStringLiteral("stream=codec_type"),
                            QStringLiteral("-of"), QStringLiteral("csv=p=0"), slow});
    streams.waitForFinished(30000);
    const QByteArray kinds = streams.readAllStandardOutput();
    const double slowLength = length(slow);
    report(run.exitCode() == 0 && slowLength > 7.5 && slowLength < 8.5 && kinds.contains("video") && !kinds.contains("audio"),
           QStringLiteral("film tools: a copy at a quarter of the speed lasts four times as long (the sound left out that slow)"),
           QStringLiteral("%1 s, streams: %2").arg(slowLength).arg(QString::fromLatin1(kinds.simplified())));
    const QString half = dir.path() + QStringLiteral("/half.mp4");
    QProcess::execute(ffmpeg, enhance::ffmpegArguments(film, half, 0.5, enhance::Settings()));
    report(length(half) > 3.8 && length(half) < 4.2, QStringLiteral("film tools: at half speed, with its sound slowed too"), QString::number(length(half)));

#ifdef DISKANVIL_MEDIA
    viewer::Window window;
    viewer::Show show;
    show.kind = viewer::Kind::Film;
    show.type = filetypes::typeForName(QStringLiteral("pattern.mp4"));
    show.path = film;
    show.name = QStringLiteral("pattern.mp4");
    window.present(show);
    window.show();
    auto *action = window.findChild<QAction *>(QStringLiteral("enhanceAction"));
    auto *speed = window.findChild<QComboBox *>(QStringLiteral("speed"));
    report(action && action->isVisible() && speed && speed->count() == 6, QStringLiteral("film tools: Enhance and the speeds are there for a film"));
    if (!action || !speed)
        return;
    speed->setCurrentIndex(speed->findData(0.25));
    {
        QEventLoop playing;
        QTimer::singleShot(1500, &playing, &QEventLoop::quit);
        playing.exec();
    }
    action->setChecked(true);
    window.findChild<QSlider *>(QStringLiteral("brightness"))->setValue(30);
    QByteArray png;
    QString name;
    QObject::connect(&window, &viewer::Window::savePicture, [&](const QString &n, const QByteArray &p) {
        name = n;
        png = p;
    });
    window.findChild<QPushButton *>(QStringLiteral("saveEnhanced"))->click();
    waitUntil([&] { return !png.isEmpty(); }, 10000);
    QImage frame;
    frame.loadFromData(png, "PNG");
    report(frame.size() == QSize(320, 240) && name.startsWith(QLatin1String("pattern frame ")) && frame.text(QStringLiteral("Description")).contains(QLatin1String("brightness +30")),
           QStringLiteral("film tools: the frame on the screen saved whole, saying where in the film it's from"), name);
    double exportSpeed = 0;
    QObject::connect(&window, &viewer::Window::exportFilm, [&exportSpeed](double rate, const enhance::Settings &, qint64) { exportSpeed = rate; });
    window.findChild<QPushButton *>(QStringLiteral("exportFilm"))->click();
    report(exportSpeed == 0.25, QStringLiteral("film tools: a slowed-down copy is asked for at the speed chosen"));
    // A spot in a paused frame (a face on a security camera): the frame becomes a still to pick in.
    window.findChild<QSlider *>(QStringLiteral("brightness"))->setValue(0);
    window.findChild<QPushButton *>(QStringLiteral("pickSpot"))->setChecked(true);
    QCoreApplication::processEvents();
    auto *picture = window.findChild<QAbstractScrollArea *>(QStringLiteral("picture"));
    const bool still = picture && picture->isVisible() && window.findChild<QSlider *>(QStringLiteral("focus"))->isEnabled();
    if (picture) {
        QWidget *port = picture->viewport();
        const QPointF corner((port->width() - 320) / 2.0, (port->height() - 240) / 2.0);
        const auto mouse = [port](QEvent::Type type, QPointF at) {
            QMouseEvent e(type, at, port->mapToGlobal(at), Qt::LeftButton, type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(port, &e);
        };
        mouse(QEvent::MouseButtonPress, corner + QPointF(100, 80));
        mouse(QEvent::MouseMove, corner + QPointF(180, 120));
        mouse(QEvent::MouseButtonRelease, corner + QPointF(180, 120));
    }
    png.clear();
    window.findChild<QPushButton *>(QStringLiteral("saveEnhanced"))->click();
    waitUntil([&] { return !png.isEmpty(); }, 10000);
    frame.loadFromData(png, "PNG");
    report(still && name.startsWith(QLatin1String("pattern frame ")) && name.contains(QLatin1String("(spot)")) && frame.width() >= 300 && std::abs(frame.width() - 2 * frame.height()) <= 16
               && frame.text(QStringLiteral("Description")).contains(QLatin1String("in, only the spot")),
           QStringLiteral("film tools: a spot picked in the paused frame is saved on its own, bigger, saying when and where"),
           QStringLiteral("%1 %2x%3: %4").arg(name).arg(frame.width()).arg(frame.height()).arg(frame.text(QStringLiteral("Description"))));
    window.findChild<QPushButton *>(QStringLiteral("wholePicture"))->click();
    QCoreApplication::processEvents();
    report(!picture->isVisible() && !window.findChild<QSlider *>(QStringLiteral("focus"))->isEnabled(),
           QStringLiteral("film tools: Back to the Film goes back to it"));
#endif
}

// What This PC Did on a Windows made of the test hives (tests/data/registry/activity.ps1).
void activityWindow()
{
    QTemporaryDir dir;
    const QString data = QStringLiteral(SOURCE_DIR "/tests/data/registry/");
    const auto put = [](const QString &path, const QString &from) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile::copy(from, path);
    };
    put(dir.path() + QStringLiteral("/Windows/System32/config/SOFTWARE"), data + QStringLiteral("activity-software.hiv"));
    put(dir.path() + QStringLiteral("/Windows/System32/config/SYSTEM"), data + QStringLiteral("activity-system.hiv"));
    put(dir.path() + QStringLiteral("/Users/tester/NTUSER.DAT"), data + QStringLiteral("activity-ntuser.hiv"));
    ActivityDialog dialog(dir.path());
    dialog.show();
    dialog.waitUntilRead();
    const int all = dialog.shown();
    auto *kind = dialog.findChild<QComboBox *>(QStringLiteral("kind"));
    auto *words = dialog.findChild<QLineEdit *>(QStringLiteral("words"));
    kind->setCurrentIndex(kind->findData(int(activity::Kind::UsbDevice)));
    const int usb = dialog.shown();
    kind->setCurrentIndex(0);
    words->setText(QStringLiteral("tax"));
    const int tax = dialog.shown();
    words->clear();
    report(all >= 10 && usb == 2 && tax == 1, QStringLiteral("activity window: every event, then just USB drives, then words"),
           QStringLiteral("%1 in all, %2 USB, %3 for tax").arg(all).arg(usb).arg(tax));
    QLabel *overview = nullptr;
    for (QLabel *l : dialog.findChildren<QLabel *>()) {
        if (l->text().startsWith(QLatin1String("<table>")))
            overview = l;
    }
    report(overview && overview->text().contains(QLatin1String("ANVIL-PC")) && overview->text().contains(QLatin1String("GMT Standard Time"))
               && overview->text().contains(QLatin1String("Sam Tester")),
           QStringLiteral("activity window: what the PC is, at the top"), overview ? overview->text().left(300) : QStringLiteral("no overview"));
    QCoreApplication::processEvents();
    if (qEnvironmentVariableIsSet("DISKANVIL_UITEST_SHOTS"))
        dialog.grab().save(qEnvironmentVariable("DISKANVIL_UITEST_SHOTS") + QStringLiteral("/activity.png"));
}

void lostFilesDeletedRecently()
{
    QTemporaryDir dir;
    const QString path = unpackLostFsImage(QStringLiteral("ntfs-journal"), dir);
    if (path.isEmpty())
        return;
    UDisks udisks;
    LostFilesDialog dialog(&udisks, QString());
    dialog.show();
    dialog.scanImage(path);
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("status"));
    waitUntil([&] { return status->text().startsWith(QLatin1String("Done")); }, 60000);
    auto *folders = dialog.findChild<QTreeWidget *>(QStringLiteral("folders"));
    auto *view = dialog.findChild<QListView *>(QStringLiteral("files"));
    QTreeWidgetItem *recent = nullptr;
    for (QTreeWidgetItemIterator it(folders); *it; ++it) {
        if ((*it)->data(0, Qt::UserRole).toString() == LostFilter::kDeletedRecently)
            recent = *it;
    }
    report(recent && recent->text(0) == QLatin1String("Deleted recently (3)") && folders->indexOfTopLevelItem(recent) == 1,
           QStringLiteral("Find Lost Files: \"Deleted recently\" is right under \"Everywhere\""), recent ? recent->text(0) : QStringLiteral("missing"));
    if (!recent)
        return;
    folders->setCurrentItem(recent);
    QStringList rows;
    for (int r = 0; r < view->model()->rowCount(); ++r)
        rows << view->model()->index(r, 0).data().toString();
    report(rows.size() == 3 && rows.value(0).startsWith(QLatin1String("beach.jpg (deleted ")), QStringLiteral("it lists them newest first, with when"),
           rows.join(QStringLiteral(", ")));

    int plan = -1;
    for (int r = 0; r < rows.size(); ++r) {
        if (rows[r].startsWith(QLatin1String("plan.txt")))
            plan = r;
    }
    const QModelIndex planRow = view->model()->index(plan, 0);
    report(plan >= 0 && !(planRow.flags() & Qt::ItemIsUserCheckable) && !planRow.data(Qt::CheckStateRole).isValid(),
           QStringLiteral("a file that's only a name has no tick box"));
    view->setCurrentIndex(planRow);
    auto *details = dialog.findChild<QLabel *>(QStringLiteral("details"));
    report(details->text().contains(QLatin1String("size not known")) && details->text().contains(QLatin1String("Was in WINJRNL/Drafts"))
               && details->text().contains(QLatin1String(">Deleted<")) && details->text().contains(QLatin1String("nothing of it to save")),
           QStringLiteral("its details say where it was, when it went and that there's nothing to save"), details->text().left(240));
    for (QPushButton *b : dialog.findChildren<QPushButton *>()) {
        if (b->text() == QLatin1String("Tick All Shown"))
            b->click();
    }
    auto *ticked = dialog.findChild<QLabel *>(QStringLiteral("ticked"));
    report(ticked->text().startsWith(QLatin1String("One file ticked")), QStringLiteral("\"Tick All Shown\" leaves it out"), ticked->text());

    if (qEnvironmentVariableIsSet("DISKANVIL_UITEST_SHOTS")) {
        QApplication::processEvents();
        dialog.grab().save(qEnvironmentVariable("DISKANVIL_UITEST_SHOTS") + QStringLiteral("/lostfiles-recent.png"));
    }
}

// A virus list of our own for Scan for Viruses: made-up "viruses" by their MD5, packed into
// ClamAV's unsigned list kind with its own sigtool, in DiskAnvil's list folder.
bool makeVirusList(const QVector<QPair<QByteArray, const char *>> &viruses, const QString &work, QString *problem)
{
    QByteArray lines;
    for (const auto &[data, name] : viruses)
        lines += QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex() + ':' + QByteArray::number(data.size()) + ':' + name + '\n';
    QDir().mkpath(work + QStringLiteral("/old"));
    for (const auto &[file, data] : {qMakePair(QStringLiteral("daily.hdb"), lines), qMakePair(QStringLiteral("COPYING"), QByteArray("test list\n"))}) {
        QFile f(work + QLatin1Char('/') + file);
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
            *problem = f.errorString();
            return false;
        }
    }
    QFile::remove(work + QStringLiteral("/daily.cud"));
    QProcess sig;
    sig.setWorkingDirectory(work);
    sig.setProcessChannelMode(QProcess::MergedChannels);
    sig.start(QStringLiteral("sigtool"), {QStringLiteral("--build=daily.cud"), QStringLiteral("--unsigned"), QStringLiteral("--cvd-version=1"),
                                          QStringLiteral("--datadir=old")});
    sig.write("diskanvil-test\n");
    sig.closeWriteChannel();
    sig.waitForFinished(60000);
    const QString own = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/diskanvil/clamav");
    QDir().mkpath(own);
    QFile::remove(own + QStringLiteral("/daily.cud"));
    if (!QFile::copy(work + QStringLiteral("/daily.cud"), own + QStringLiteral("/daily.cud"))) {
        *problem = QString::fromUtf8(sig.readAll());
        return false;
    }
    return true;
}

// Scan for Viruses, with real ClamAV and a list of our own: a folder, then (virusScanOnDrive)
// a drive the window mounts itself.
void virusScanThroughWindow()
{
    const QString sigtool = QStandardPaths::findExecutable(QStringLiteral("sigtool"));
    if (QStandardPaths::findExecutable(QStringLiteral("clamscan")).isEmpty() || sigtool.isEmpty()) {
        out << "SKIP  Scan for Viruses: ClamAV isn't installed" << Qt::endl;
        return;
    }
    QTemporaryDir dir;
    const QString drive = dir.filePath(QStringLiteral("drive"));
    auto write = [](const QString &path, const QByteArray &data) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
    };
    const QByteArray trojan = "pretend this is a trojan\n";
    const QByteArray ransom = QByteArray(5000, 'r') + "pretend this is ransomware";
    write(drive + QStringLiteral("/Users/a/Downloads/setup.exe"), trojan);
    write(drive + QStringLiteral("/Users/a/Downloads/tool.exe"), "a clean program\n");
    write(drive + QStringLiteral("/Windows/System32/helper.dll"), ransom);
    write(drive + QStringLiteral("/Users/a/Pictures/cat.jpg"), trojan); // not looked at by a quick scan

    UDisks udisks;
    const bool systemList = !QDir(QStringLiteral("/var/lib/clamav")).entryList({QStringLiteral("daily.*")}).isEmpty();
    if (!systemList) {
        VirusDialog dialog(&udisks);
        auto *engine = dialog.findChild<QLabel *>(QStringLiteral("engine"));
        QPushButton *download = findButton(&dialog, QStringLiteral("Download the List"));
        QPushButton *scan = findButton(&dialog, QStringLiteral("Scan"));
        report(engine->text().contains(QLatin1String("doesn't have its list of viruses yet")) && download && scan && !scan->isEnabled(),
               QStringLiteral("Scan for Viruses: without a list it says so, and offers the download"), engine->text().left(100));
    }

    QString problem;
    if (!makeVirusList({{trojan, "Win.Trojan.DiskAnvilTest-1"}, {ransom, "Win.Ransomware.DiskAnvilTest-2"}}, dir.filePath(QStringLiteral("list")),
                       &problem)) {
        report(false, QStringLiteral("make a test virus list with sigtool"), problem);
        return;
    }

    VirusDialog dialog(&udisks);
    dialog.show();
    auto *engine = dialog.findChild<QLabel *>(QStringLiteral("engine"));
    report(engine->text().contains(QLatin1String("list of viruses from today")) && engine->text().contains(QLatin1String("2 known")),
           QStringLiteral("with a list, it says how old it is"), engine->text().left(100));
    dialog.scanFolder(drive);
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("status"));
    waitUntil([&] { return status->text().startsWith(QLatin1String("Done")); }, 60000);
    auto *hits = dialog.findChild<QTreeWidget *>(QStringLiteral("hits"));
    QStringList found;
    for (int i = 0; i < hits->topLevelItemCount(); ++i)
        found << hits->topLevelItem(i)->text(0) + QLatin1Char('=') + hits->topLevelItem(i)->text(1).section(QLatin1Char(':'), 0, 0);
    found.sort();
    report(status->text().startsWith(QLatin1String("Done: found 2 thing")) && status->text().contains(QLatin1String(" in 3 files."))
               && found == QStringList{QStringLiteral("/Users/a/Downloads/setup.exe=A Trojan"), QStringLiteral("/Windows/System32/helper.dll=Ransomware")},
           QStringLiteral("a quick scan finds both, in plain words"), status->text() + QLatin1Char(' ') + found.join(QStringLiteral(", ")));
    if (qEnvironmentVariableIsSet("DISKANVIL_UITEST_SHOTS"))
        dialog.grab().save(qEnvironmentVariable("DISKANVIL_UITEST_SHOTS") + QStringLiteral("/virusscan.png"));

    // Delete: asked first, with a warning for Windows' own folders; then gone, and in the log.
    QStringList boxes;
    Answerer answerer;
    answerer.answer = [&](QWidget *modal) {
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            boxes << box->text();
            box->button(QMessageBox::Yes)->click();
            return true;
        }
        return false;
    };
    dialog.deleteTicked();
    waitUntil([&] { return status->text().startsWith(QLatin1String("Deleted")); }, 10000);
    QFile log(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/diskanvil/virus-scan.log"));
    const QString logged = log.open(QIODevice::ReadOnly) ? QString::fromUtf8(log.readAll()) : QString();
    report(boxes.size() == 1 && boxes[0].contains(QLatin1String("Delete these 2 file")) && boxes[0].contains(QLatin1String("Windows' own folders"))
               && !QFile::exists(drive + QStringLiteral("/Users/a/Downloads/setup.exe")) && !QFile::exists(drive + QStringLiteral("/Windows/System32/helper.dll"))
               && QFile::exists(drive + QStringLiteral("/Users/a/Downloads/tool.exe")) && logged.contains(QLatin1String("deleted ") + drive),
           QStringLiteral("deleting asks first, warns about Windows' folders, and logs it"), status->text().left(100));
}

// A task the way Task Scheduler stores one: UTF-16 with a byte order mark.
QByteArray schedulerTask(const QString &command, const QString &arguments)
{
    const QString xml = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
                                       "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
                                       "  <Settings><Enabled>true</Enabled></Settings>\r\n"
                                       "  <Actions Context=\"Author\"><Exec><Command>%1</Command><Arguments>%2</Arguments></Exec></Actions>\r\n"
                                       "</Task>\r\n")
                            .arg(command.toHtmlEscaped(), arguments.toHtmlEscaped());
    QByteArray out("\xff\xfe", 2);
    for (QChar c : xml) {
        out += char(c.unicode() & 0xff);
        out += char(c.unicode() >> 8);
    }
    return out;
}

// A Windows drive (made from the test hives in tests/data/registry): what starts with it is
// read first, its programs are scanned even when a quick scan wouldn't look at them, and an
// entry that starts something the scan found says so.
void virusScanWindowsDrive()
{
    if (QStandardPaths::findExecutable(QStringLiteral("clamscan")).isEmpty() || QStandardPaths::findExecutable(QStringLiteral("sigtool")).isEmpty()) {
        out << "SKIP  Scan for Viruses on a Windows drive: ClamAV isn't installed" << Qt::endl;
        return;
    }
    QTemporaryDir dir;
    const QByteArray trojan = "pretend this trojan starts with windows\n";
    QString problem;
    if (!makeVirusList({{trojan, "Win.Trojan.DiskAnvilTest-4"}}, dir.filePath(QStringLiteral("list")), &problem)) {
        report(false, QStringLiteral("make a test virus list with sigtool"), problem);
        return;
    }
    const QString root = dir.filePath(QStringLiteral("drive"));
    auto put = [&root](const QString &path, const QByteArray &data) {
        QDir().mkpath(QFileInfo(root + QLatin1Char('/') + path).absolutePath());
        QFile f(root + QLatin1Char('/') + path);
        if (f.open(QIODevice::WriteOnly))
            f.write(data);
    };
    auto fixture = [](const char *name) {
        QFile f(QStringLiteral(SOURCE_DIR "/tests/data/registry/") + QLatin1String(name));
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    put(QStringLiteral("Windows/System32/config/SOFTWARE"), fixture("software.hiv"));
    put(QStringLiteral("Windows/System32/config/SYSTEM"), fixture("system.hiv"));
    put(QStringLiteral("Users/tester/NTUSER.DAT"), fixture("ntuser.hiv"));
    put(QStringLiteral("Users/tester/AppData/Local/Temp/xkqzvbnw.exe"), trojan);       // the Run key's Updater
    put(QStringLiteral("Users/tester/AppData/Roaming/upd/payload.dat"), trojan);       // a task's library
    for (const char *file : {"Windows/System32/rundll32.exe", "Windows/System32/SecurityHealthSystray.exe", "Windows/System32/cmd.exe",
                             "Program Files (x86)/Old App/old.exe", "Windows/System32/userinit.exe", "Windows/System32/drivers/drv.sys",
                             "Windows/System32/goodsvc.dll", "Users/tester/AppData/Local/Discord/Update.exe", "Windows/explorer.exe"})
        put(QLatin1String(file), "MZ");
    put(QStringLiteral("Windows/System32/Tasks/Sync"), schedulerTask(QStringLiteral("%windir%\\system32\\rundll32.exe"),
                                                                      QStringLiteral("C:\\Users\\tester\\AppData\\Roaming\\upd\\payload.dat,Start")));

    UDisks udisks;
    VirusDialog dialog(&udisks);
    dialog.show();
    dialog.scanFolder(root);
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("status"));
    waitUntil([&] { return status->text().startsWith(QLatin1String("Done")); }, 60000);
    auto *tabs = dialog.findChild<QTabWidget *>(QStringLiteral("tabs"));
    auto *hits = dialog.findChild<QTreeWidget *>(QStringLiteral("hits"));
    auto *list = dialog.findChild<QTreeWidget *>(QStringLiteral("startup"));
    QStringList found;
    for (int i = 0; i < hits->topLevelItemCount(); ++i)
        found << hits->topLevelItem(i)->text(0);
    found.sort();
    report(found == QStringList{QStringLiteral("/Users/tester/AppData/Local/Temp/xkqzvbnw.exe"), QStringLiteral("/Users/tester/AppData/Roaming/upd/payload.dat")}
               && hits->topLevelItem(0)->text(1).contains(QLatin1String("It starts with Windows")),
           QStringLiteral("Scan for Viruses: what starts with Windows is scanned too, even a .dat"), found.join(QStringLiteral(", ")));
    QStringList odd;
    QString updater;
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        odd << list->topLevelItem(i)->text(0);
        if (list->topLevelItem(i)->text(0) == QLatin1String("Updater"))
            updater = list->topLevelItem(i)->text(2);
    }
    report(tabs->isTabVisible(1) && tabs->tabText(1).startsWith(QLatin1String("Starts with Windows (")) && odd.size() >= 10 && odd.contains(QStringLiteral("Sync"))
               && updater.contains(QLatin1String("the virus scan found")) && status->text().contains(QLatin1String("Starts with Windows:")),
           QStringLiteral("the odd startup entries are listed, and the found ones say so"), tabs->tabText(1) + QStringLiteral(": ") + odd.join(QStringLiteral(", ")));
    auto *showAll = dialog.findChild<QCheckBox *>(QStringLiteral("showAll"));
    const int oddCount = list->topLevelItemCount();
    showAll->setChecked(true);
    report(list->topLevelItemCount() > oddCount, QStringLiteral("and everything that starts, when asked"), QString::number(list->topLevelItemCount()));
    showAll->setChecked(false);
    if (qEnvironmentVariableIsSet("DISKANVIL_UITEST_SHOTS")) {
        tabs->setCurrentIndex(1);
        dialog.grab().save(qEnvironmentVariable("DISKANVIL_UITEST_SHOTS") + QStringLiteral("/virus-startup.png"));
        tabs->setCurrentIndex(0);
        dialog.grab().save(qEnvironmentVariable("DISKANVIL_UITEST_SHOTS") + QStringLiteral("/virus-files.png"));
    }

    // Switching them off: what the scan found is ticked already; four more are ticked here.
    QStringList ticked;
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = list->topLevelItem(i);
        if (QStringList{QStringLiteral("Windows Update Helper"), QStringLiteral("Shell"), QStringLiteral("BootExecute"), QStringLiteral("Load")}.contains(item->text(0)))
            item->setCheckState(0, Qt::Checked);
        if (item->checkState(0) == Qt::Checked)
            ticked << item->text(0);
    }
    ticked.sort();
    report(ticked == QStringList{QStringLiteral("BootExecute"), QStringLiteral("Load"), QStringLiteral("Shell"), QStringLiteral("Sync"), QStringLiteral("Updater"),
                                 QStringLiteral("Windows Update Helper")},
           QStringLiteral("what starts the malware found is ticked to switch off already"), ticked.join(QStringLiteral(", ")));
    QStringList boxes;
    Answerer answerer;
    answerer.answer = [&](QWidget *modal) {
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            boxes << box->text();
            box->button(QMessageBox::Yes)->click();
            return true;
        }
        return false;
    };
    dialog.switchOffTicked();
    waitUntil([&] { return status->text().startsWith(QLatin1String("Switched off")); }, 10000);
    const QStringList backups = QDir(root + QStringLiteral("/DiskAnvil backups")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    const QString backup = root + QStringLiteral("/DiskAnvil backups/") + backups.value(0);
    QStringList kept = QDir(backup).entryList(QDir::Files);
    kept.sort();
    report(status->text().startsWith(QLatin1String("Switched off 6")) && boxes.size() == 1 && boxes[0].contains(QLatin1String("backup"))
               && backups.size() == 1 && QFile::exists(backup + QStringLiteral("/Tasks/Sync"))
               && kept == QStringList{QStringLiteral("NTUSER.DAT (tester)"), QStringLiteral("SOFTWARE"), QStringLiteral("SYSTEM"),
                                      QStringLiteral("What DiskAnvil changed.txt")},
           QStringLiteral("switching off asks first, and keeps a backup with a note"), status->text().left(80) + QStringLiteral(" | ") + kept.join(QStringLiteral(", ")));

    reg::Hive software, system, user;
    QString error;
    software.open(root + QStringLiteral("/Windows/System32/config/SOFTWARE"), &error);
    system.open(root + QStringLiteral("/Windows/System32/config/SYSTEM"), &error);
    user.open(root + QStringLiteral("/Users/tester/NTUSER.DAT"), &error);
    auto text = [](const reg::Hive &h, const QString &key, const QString &name) {
        const std::optional<reg::Value> v = h.value(h.key(key), name);
        return v ? v->text() : QStringLiteral("(gone)");
    };
    report(text(software, QStringLiteral("Microsoft\\Windows\\CurrentVersion\\Run"), QStringLiteral("Updater")) == QLatin1String("(gone)")
               && text(software, QStringLiteral("Microsoft\\Windows\\CurrentVersion\\Run"), QStringLiteral("Helper")) != QLatin1String("(gone)")
               && text(software, QStringLiteral("Microsoft\\Windows NT\\CurrentVersion\\Winlogon"), QStringLiteral("Shell")) == QLatin1String("explorer.exe")
               && text(system, QStringLiteral("ControlSet001\\Services\\EvilSvc"), QStringLiteral("Start")) == QLatin1String("4")
               && text(system, QStringLiteral("ControlSet001\\Control\\Session Manager"), QStringLiteral("BootExecute")) == QLatin1String("autocheck autochk *")
               && text(user, QStringLiteral("Software\\Microsoft\\Windows NT\\CurrentVersion\\Windows"), QStringLiteral("Load")) == QLatin1String("(gone)")
               && !QFile::exists(root + QStringLiteral("/Windows/System32/Tasks/Sync")) && software.problems() + system.problems() + user.problems() == 0,
           QStringLiteral("the registry files have only those changes, and the task is gone"), error);

    dialog.scanFolder(root);
    waitUntil([&] { return status->text().startsWith(QLatin1String("Done")); }, 60000);
    QStringList still;
    for (int i = 0; i < list->topLevelItemCount(); ++i)
        still << list->topLevelItem(i)->text(0);
    report(!still.contains(QStringLiteral("Updater")) && !still.contains(QStringLiteral("Windows Update Helper")) && !still.contains(QStringLiteral("Shell"))
               && !still.contains(QStringLiteral("BootExecute")) && !still.contains(QStringLiteral("Load")) && !still.contains(QStringLiteral("Sync"))
               && still.contains(QStringLiteral("Helper")),
           QStringLiteral("scanned again, what was switched off isn't odd any more"), still.join(QStringLiteral(", ")));
}

// Give New IDs: a drive and its byte-for-byte copy (a GPT with an EFI partition and an ext4
// Linux whose fstab and boot entry name them by ID), both connected. The window warns, and
// the copy gets its own IDs, its Linux's settings changed to match.
void newIdsThroughWindow()
{
    QTemporaryDir dir;
    const QString original = dir.filePath(QStringLiteral("original.img")), copy = dir.filePath(QStringLiteral("copy.img"));
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("96M"), original});
    QProcess sfdisk;
    sfdisk.start(QStringLiteral("sfdisk"), {QStringLiteral("-q"), original});
    sfdisk.waitForStarted();
    sfdisk.write("label: gpt\nstart=2048, size=40960, type=U\nstart=43008, type=L\n");
    sfdisk.closeWriteChannel();
    sfdisk.waitForFinished();
    // The Linux's own files: they name the file systems the way an installer would.
    const QString efiUuid = QStringLiteral("1234-ABCD"), rootUuid = QStringLiteral("7c9e6679-7425-40de-944b-e07fc1f90ae7");
    const QString tree = dir.filePath(QStringLiteral("root"));
    QDir().mkpath(tree + QStringLiteral("/etc"));
    QFile fstab(tree + QStringLiteral("/etc/fstab"));
    if (fstab.open(QIODevice::WriteOnly))
        fstab.write(QStringLiteral("UUID=%1 / ext4 defaults 0 1\nUUID=%2 /boot vfat umask=0077 0 2\n").arg(rootUuid, efiUuid).toUtf8());
    fstab.close();
    QString partitions;
    sh(QStringLiteral("sfdisk"), {QStringLiteral("--part-uuid"), original, QStringLiteral("2")}, &partitions);
    const QString rootPart = partitions.trimmed().toLower();
    const QString entries = dir.filePath(QStringLiteral("arch.conf"));
    QFile entry(entries);
    if (entry.open(QIODevice::WriteOnly))
        entry.write(QStringLiteral("title Arch\nlinux /vmlinuz-linux\noptions root=PARTUUID=%1 rw\n").arg(rootPart).toUtf8());
    entry.close();
    sh(QStringLiteral("mkfs.vfat"), {QStringLiteral("-i"), QString(efiUuid).remove(QLatin1Char('-')), QStringLiteral("--offset=2048"), original,
                                     QStringLiteral("20480")});
    sh(QStringLiteral("mmd"), {QStringLiteral("-i"), original + QStringLiteral("@@1M"), QStringLiteral("::/loader"), QStringLiteral("::/loader/entries")});
    sh(QStringLiteral("mcopy"), {QStringLiteral("-i"), original + QStringLiteral("@@1M"), entries, QStringLiteral("::/loader/entries/arch.conf")});
    sh(QStringLiteral("mke2fs"), {QStringLiteral("-q"), QStringLiteral("-t"), QStringLiteral("ext4"), QStringLiteral("-U"), rootUuid, QStringLiteral("-d"), tree,
                                  QStringLiteral("-E"), QStringLiteral("offset=%1").arg(43008 * 512), original, QStringLiteral("70M")});
    QFile::copy(original, copy);

    // Both connected.
    QStringList loops;
    for (const QString &image : {original, copy}) {
        QFile backing(image);
        if (!backing.open(QIODevice::ReadWrite))
            continue;
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.UDisks2"), QStringLiteral("/org/freedesktop/UDisks2/Manager"),
                                                           QStringLiteral("org.freedesktop.UDisks2.Manager"), QStringLiteral("LoopSetup"));
        call << QVariant::fromValue(QDBusUnixFileDescriptor(backing.handle())) << QVariantMap{{QStringLiteral("auth.no_user_interaction"), true}};
        loops << QDBusConnection::systemBus().call(call, QDBus::Block, 30000).arguments().value(0).value<QDBusObjectPath>().path();
    }
    report(loops.size() == 2 && !loops.contains(QString()), QStringLiteral("Give New IDs: a drive and its copy are connected (loop devices)"));
    if (loops.size() != 2 || loops.contains(QString()))
        return;
    UDisks udisks;
    udisks.setInteractive(false);
    waitUntil([&] {
        udisks.refresh();
        const Disk *a = udisks.diskByPath(loops[0]), *b = udisks.diskByPath(loops[1]);
        return a && b && a->volumes.size() == 2 && b->volumes.size() == 2 && !b->volumes[1].uuid.isEmpty();
    }, 15000);
    QVector<newids::Clash> found;
    for (const newids::Clash &c : newids::clashes(udisks.disks())) {
        if (loops.contains(c.first) && loops.contains(c.second))
            found << c;
    }
    report(found.size() == 1 && found[0].ids.contains(QStringLiteral("the disk ID")), QStringLiteral("they're seen as copies of each other"),
           found.isEmpty() ? QString() : found[0].ids.join(QStringLiteral(", ")));
    {
        MainWindow window(&udisks);
        window.show();
        QApplication::processEvents();
        bool warned = false;
        for (NoticeBar *bar : window.findChildren<NoticeBar *>(QStringLiteral("clone")))
            warned = warned || (bar->isVisible() && bar->findChildren<QPushButton *>().size() == 2);
        report(warned, QStringLiteral("the main window warns, with a button to fix it"));
    }

    // Writing to a drive takes polkit's yes, which only root gets without being asked.
    if (geteuid() != 0) {
        out << "SKIP  Give New IDs' writing part: run this scenario as root (sudo ... --user --only=newids)" << Qt::endl;
        for (const QString &loop : std::as_const(loops))
            cleanUpLoop(udisks, loop);
        return;
    }
    const Disk *before = udisks.diskByPath(loops[1]);
    const QString oldTable = before->tableId, oldPart = before->volumes[1].partUuid;
    QStringList said;
    Answerer answerer;
    answerer.answer = [&said](QWidget *modal) {
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            said << box->text();
            box->accept();
            return true;
        }
        return false;
    };
    {
        NewIdsDialog dialog(&udisks, loops[1]);
        dialog.show();
        dialog.findChild<QLineEdit *>(QStringLiteral("confirm"))->setText(shortDevice(before->device));
        dialog.start();
        waitUntil([&] { return !dialog.isVisible(); }, 90000);
    }
    QString root, efi;
    waitUntil([&] {
        udisks.refresh();
        const Disk *after = udisks.diskByPath(loops[1]);
        return after && after->tableId != oldTable && after->volumes.size() == 2 && after->volumes[1].uuid != rootUuid;
    }, 15000);
    const Disk *after = udisks.diskByPath(loops[1]);
    const Disk *kept = udisks.diskByPath(loops[0]);
    QStringList clashLeft;
    for (const newids::Clash &c : newids::clashes(udisks.disks())) {
        if (loops.contains(c.first) && loops.contains(c.second))
            clashLeft << c.ids;
    }
    report(after && kept && after->tableId != oldTable && after->volumes[1].partUuid != oldPart && after->volumes[1].uuid != rootUuid
               && after->volumes[0].uuid.compare(efiUuid, Qt::CaseInsensitive) != 0 && kept->tableId == oldTable && clashLeft.isEmpty(),
           QStringLiteral("the copy has its own IDs now, the original keeps its own"), clashLeft.join(QStringLiteral(", ")) + QStringLiteral(" | ") + said.join(QStringLiteral(" / ")));
    // What its Linux says now: read from the copy's image, with the tools that made it.
    QString newFstab, newEntry;
    sh(QStringLiteral("debugfs"), {QStringLiteral("-R"), QStringLiteral("cat /etc/fstab"), copy + QStringLiteral("?offset=%1").arg(43008 * 512)}, &newFstab);
    if (newFstab.trimmed().isEmpty()) {
        // debugfs without image offsets: through a copy of just that file system
        const QString part = dir.filePath(QStringLiteral("root.ext4"));
        sh(QStringLiteral("dd"), {QStringLiteral("if=") + copy, QStringLiteral("of=") + part, QStringLiteral("bs=512"), QStringLiteral("skip=43008"),
                                  QStringLiteral("status=none")});
        sh(QStringLiteral("debugfs"), {QStringLiteral("-R"), QStringLiteral("cat /etc/fstab"), part}, &newFstab);
    }
    sh(QStringLiteral("mtype"), {QStringLiteral("-i"), copy + QStringLiteral("@@1M"), QStringLiteral("::/loader/entries/arch.conf")}, &newEntry);
    report(after && after->volumes[1].uuid != rootUuid && newFstab.contains(after->volumes[1].uuid) && !newFstab.contains(rootUuid)
               && newFstab.contains(after->volumes[0].uuid, Qt::CaseInsensitive) && newEntry.contains(after->volumes[1].partUuid, Qt::CaseInsensitive)
               && !newEntry.contains(rootPart, Qt::CaseInsensitive),
           QStringLiteral("and its Linux's fstab and boot entry name the new IDs"), newFstab.simplified().left(140) + QStringLiteral(" | ") + newEntry.simplified());
    for (const QString &loop : std::as_const(loops))
        cleanUpLoop(udisks, loop);
}

// The online check: asked first, only fingerprints sent, answers shown. A server of our own
// stands in for CIRCL's.
void virusLookUpOnline()
{
    if (QStandardPaths::findExecutable(QStringLiteral("clamscan")).isEmpty() || QStandardPaths::findExecutable(QStringLiteral("sigtool")).isEmpty()) {
        out << "SKIP  Scan for Viruses' online check: ClamAV isn't installed" << Qt::endl;
        return;
    }
    QTemporaryDir dir;
    QString problem;
    if (!makeVirusList({{QByteArray("nothing on this drive matches this"), "Win.Trojan.DiskAnvilTest-5"}}, dir.filePath(QStringLiteral("list")), &problem)) {
        report(false, QStringLiteral("make a test virus list with sigtool"), problem);
        return;
    }
    const QString root = dir.filePath(QStringLiteral("drive"));
    auto put = [&root](const QString &path, const QByteArray &data) {
        QDir().mkpath(QFileInfo(root + QLatin1Char('/') + path).absolutePath());
        QFile f(root + QLatin1Char('/') + path);
        if (f.open(QIODevice::WriteOnly))
            f.write(data);
    };
    auto fixture = [](const char *name) {
        QFile f(QStringLiteral(SOURCE_DIR "/tests/data/registry/") + QLatin1String(name));
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    put(QStringLiteral("Windows/System32/config/SOFTWARE"), fixture("software.hiv"));
    put(QStringLiteral("Windows/System32/config/SYSTEM"), fixture("system.hiv"));
    put(QStringLiteral("Users/tester/NTUSER.DAT"), fixture("ntuser.hiv"));
    const QByteArray knownFile = "a program the list knows";
    put(QStringLiteral("Users/tester/AppData/Local/Temp/xkqzvbnw.exe"), knownFile); // Run: Updater
    put(QStringLiteral("Users/tester/AppData/Roaming/abc.exe"), "a program it doesn't");  // Load
    const QString knownHash = QString::fromLatin1(QCryptographicHash::hash(knownFile, QCryptographicHash::Sha256).toHex());

    QTcpServer server;
    QStringList asked;
    QObject::connect(&server, &QTcpServer::newConnection, [&] {
        QTcpSocket *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
            const QByteArray request = socket->readAll();
            const QString path = QString::fromLatin1(request.split(' ').value(1));
            asked << path;
            const bool known = path.endsWith(knownHash);
            const QByteArray body = known ? QByteArray(R"({"FileName": "xkqzvbnw.exe", "ProductCode": {"ProductName": "Test Product", "ProductVersion": "1.0"}, "source": "NSRL", "hashlookup:trust": 60})")
                                          : QByteArray(R"({"message": "Non existing SHA-256"})");
            socket->write((known ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 404 NOT FOUND\r\n") + QByteArray("Content-Type: application/json\r\nContent-Length: ")
                          + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });
    server.listen(QHostAddress::LocalHost);
    const QString realServer = hashlookup::server;
    hashlookup::server = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());

    UDisks udisks;
    VirusDialog dialog(&udisks);
    dialog.show();
    dialog.scanFolder(root);
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("status"));
    waitUntil([&] { return status->text().startsWith(QLatin1String("Done")); }, 60000);
    auto *lookUp = dialog.findChild<QPushButton *>(QStringLiteral("lookUp"));
    QStringList boxes;
    Answerer answerer;
    answerer.answer = [&](QWidget *modal) {
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            boxes << box->text();
            box->button(QMessageBox::Yes)->click();
            return true;
        }
        return false;
    };
    dialog.lookUpOnline();
    waitUntil([&] { return status->text().startsWith(QLatin1String("Looked up online")); }, 30000);
    auto *list = dialog.findChild<QTreeWidget *>(QStringLiteral("startup"));
    auto *details = dialog.findChild<QLabel *>(QStringLiteral("startupDetails"));
    QString updater, load;
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        list->setCurrentItem(list->topLevelItem(i));
        if (list->topLevelItem(i)->text(0) == QLatin1String("Updater"))
            updater = details->text();
        if (list->topLevelItem(i)->text(0) == QLatin1String("Load"))
            load = details->text();
    }
    bool onlyHashes = asked.size() == 2;
    for (const QString &path : std::as_const(asked))
        onlyHashes = onlyHashes && QRegularExpression(QStringLiteral("^/lookup/sha256/[0-9a-f]{64}$")).match(path).hasMatch();
    report(lookUp && lookUp->isVisible() && boxes.size() == 1 && boxes[0].contains(QLatin1String("CIRCL")) && boxes[0].contains(QLatin1String("fingerprints")),
           QStringLiteral("Scan for Viruses: the online check asks first, and says what's sent and to whom"), boxes.value(0).left(60));
    report(onlyHashes && status->text().contains(QLatin1String("1 known, 1 not known")),
           QStringLiteral("only the two fingerprints are sent"), asked.join(QStringLiteral(", ")).left(160));
    report(updater.contains(QLatin1String("a known file: xkqzvbnw.exe, part of Test Product 1.0 (NSRL)")) && load.contains(QLatin1String("not a file the list knows")),
           QStringLiteral("and what the list said shows with each one"));
    hashlookup::server = realServer;
}

// The Boot tab: an EFI partition whose Windows boot manager a "bootkit" replaced with an unsigned
// one, checked against made-up Secure Boot lists, and Windows' own copy put back.
void virusScanBoot()
{
    using namespace testsign;
    QTemporaryDir dir;
    const Signer root = makeCertificate("DiskAnvil Test Root", nullptr, true);
    const Signer windows = makeCertificate("Microsoft Windows (DiskAnvil test)", &root, false);
    const QByteArray good = sign(makeProgram("Windows' boot manager"), windows, {windows.cert, root.cert});
    const QByteArray bad = makeProgram("a bootkit's boot manager");
    const QString vars = dir.filePath(QStringLiteral("efivars"));
    QDir().mkpath(vars);
    putVariable(vars, QStringLiteral("SecureBoot"), efi::kGlobal, QByteArray(1, '\1'));
    putVariable(vars, QStringLiteral("db"), efi::kSecurity, signatureList(kX509List, {derOf(root.cert)}));
    auto put = [](const QString &path, const QByteArray &data) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly))
            f.write(data);
    };
    const QString esp = dir.filePath(QStringLiteral("esp")), win = dir.filePath(QStringLiteral("windows"));
    put(esp + QStringLiteral("/EFI/Microsoft/Boot/bootmgfw.efi"), bad);
    put(esp + QStringLiteral("/EFI/Boot/bootx64.efi"), good);
    put(win + QStringLiteral("/Windows/Boot/EFI/bootmgfw.efi"), good);
    const QString realVars = efi::varsDir;
    efi::varsDir = vars;
    VirusDialog::askFwupd = false;

    UDisks udisks;
    VirusDialog dialog(&udisks);
    dialog.show();
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("status"));
    dialog.checkEfiPartition(esp, win);
    auto *tabs = dialog.findChild<QTabWidget *>(QStringLiteral("tabs"));
    auto *list = dialog.findChild<QTreeWidget *>(QStringLiteral("boot"));
    QTreeWidgetItem *manager = list->topLevelItemCount() ? list->topLevelItem(0) : nullptr;
    report(tabs->isTabVisible(2) && tabs->tabText(2) == QLatin1String("Boot (1 wrong)") && manager && manager->text(0) == QLatin1String("bootmgfw.efi")
               && manager->checkState(0) == Qt::Checked && list->topLevelItemCount() == 3, // with Secure Boot's own state
           QStringLiteral("Scan for Viruses: an unsigned Windows boot manager is found, ready to be put back"),
           tabs->tabText(2) + (manager ? QStringLiteral(": ") + manager->text(0) + QLatin1Char(' ') + manager->text(2) : QString()));
    if (qEnvironmentVariableIsSet("DISKANVIL_UITEST_SHOTS")) {
        tabs->setCurrentIndex(2);
        dialog.grab().save(qEnvironmentVariable("DISKANVIL_UITEST_SHOTS") + QStringLiteral("/virus-boot.png"));
    }
    Answerer answerer;
    answerer.answer = [&](QWidget *modal) {
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            box->button(QMessageBox::Yes)->click();
            return true;
        }
        return false;
    };
    dialog.putBackTicked();
    waitUntil([&] { return status->text().startsWith(QLatin1String("Put back")); }, 10000);
    QFile now(esp + QStringLiteral("/EFI/Microsoft/Boot/bootmgfw.efi"));
    const QByteArray after = now.open(QIODevice::ReadOnly) ? now.readAll() : QByteArray();
    const QStringList backups = QDir(esp + QStringLiteral("/DiskAnvil backups")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    report(after == good && backups.size() == 1 && QFile::exists(esp + QStringLiteral("/DiskAnvil backups/") + backups.value(0) + QStringLiteral("/EFI_Microsoft_Boot_bootmgfw.efi"))
               && list->topLevelItem(0)->text(2).startsWith(QLatin1String("Windows' own copy is back")),
           QStringLiteral("Windows' own boot manager is put back, the old one kept"), status->text().left(100));
    efi::varsDir = realVars;
    for (const Signer &s : {root, windows})
        release(s);
}

// A drive that isn't mounted (an NTFS loop device standing in for Windows' drive): the window
// mounts it read-only to scan, writable only to delete, and unmounts it again when it closes.
void virusScanOnDrive()
{
    for (const char *tool : {"clamscan", "sigtool", "mkntfs", "ntfscp"}) {
        if (QStandardPaths::findExecutable(QLatin1String(tool)).isEmpty()) {
            out << "SKIP  Scan for Viruses on a drive: " << tool << " isn't installed" << Qt::endl;
            return;
        }
    }
    QTemporaryDir dir;
    const QByteArray trojan = "pretend this is a trojan on a drive\n";
    QString problem;
    if (!makeVirusList({{trojan, "Win.Trojan.DiskAnvilTest-3"}}, dir.filePath(QStringLiteral("list")), &problem)) {
        report(false, QStringLiteral("make a test virus list with sigtool"), problem);
        return;
    }
    const QString image = dir.filePath(QStringLiteral("windows.img"));
    for (const auto &[file, data] : {qMakePair(QStringLiteral("trojan.bin"), trojan), qMakePair(QStringLiteral("clean.bin"), QByteArray("clean\n"))}) {
        QFile f(dir.filePath(file));
        if (f.open(QIODevice::WriteOnly))
            f.write(data);
    }
    sh(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("32M"), image});
    const bool made = sh(QStringLiteral("mkntfs"), {QStringLiteral("-q"), QStringLiteral("-F"), QStringLiteral("-Q"), QStringLiteral("-L"),
                                                    QStringLiteral("VIRUSTEST"), image}) == 0
                   && sh(QStringLiteral("ntfscp"), {QStringLiteral("-q"), image, dir.filePath(QStringLiteral("trojan.bin")), QStringLiteral("/setup.exe")}) == 0
                   && sh(QStringLiteral("ntfscp"), {QStringLiteral("-q"), image, dir.filePath(QStringLiteral("clean.bin")), QStringLiteral("/clean.exe")}) == 0;
    QFile backing(image);
    QString loopPath;
    if (made && backing.open(QIODevice::ReadWrite)) {
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.UDisks2"), QStringLiteral("/org/freedesktop/UDisks2/Manager"),
                                                           QStringLiteral("org.freedesktop.UDisks2.Manager"), QStringLiteral("LoopSetup"));
        call << QVariant::fromValue(QDBusUnixFileDescriptor(backing.handle())) << QVariantMap{{QStringLiteral("auth.no_user_interaction"), true}};
        loopPath = QDBusConnection::systemBus().call(call, QDBus::Block, 30000).arguments().value(0).value<QDBusObjectPath>().path();
        backing.close();
    }
    report(!loopPath.isEmpty(), QStringLiteral("Scan for Viruses: an NTFS loop device stands in for Windows' drive"));
    if (loopPath.isEmpty())
        return;
    UDisks udisks;
    udisks.setInteractive(false);
    QString volumePath;
    waitUntil([&] {
        udisks.refresh();
        if (const Disk *d = udisks.diskByPath(loopPath)) {
            for (const Volume &v : d->volumes) {
                if (v.fsType == QLatin1String("ntfs"))
                    volumePath = v.objectPath;
            }
        }
        return !volumePath.isEmpty();
    }, 15000);
    auto mountOf = [&]() -> QString {
        udisks.refresh();
        if (const Disk *d = udisks.diskByPath(loopPath)) {
            for (const Volume &v : d->volumes) {
                if (v.objectPath == volumePath)
                    return v.mountPoints.value(0);
            }
        }
        return {};
    };
    auto options = [](const QString &mount) {
        QFile f(QStringLiteral("/proc/self/mounts"));
        if (!f.open(QIODevice::ReadOnly))
            return QString();
        for (const QByteArray &line : f.readAll().split('\n')) {
            const QList<QByteArray> fields = line.split(' ');
            if (fields.size() > 3 && QString::fromUtf8(fields[1]).replace(QStringLiteral("\\040"), QStringLiteral(" ")) == mount)
                return QString::fromUtf8(fields[3]);
        }
        return QString();
    };
    {
        VirusDialog dialog(&udisks);
        dialog.show();
        auto *targets = dialog.findChild<QTreeWidget *>(QStringLiteral("targets"));
        QTreeWidgetItem *item = nullptr;
        for (int i = 0; i < targets->topLevelItemCount(); ++i) {
            if (targets->topLevelItem(i)->data(0, Qt::UserRole).toString() == volumePath)
                item = targets->topLevelItem(i);
        }
        report(item != nullptr, QStringLiteral("the drive is in the list to pick from"));
        if (item) {
            targets->setCurrentItem(item);
            findButton(&dialog, QStringLiteral("Scan"))->click();
            auto *status = dialog.findChild<QLabel *>(QStringLiteral("status"));
            waitUntil([&] { return status->text().startsWith(QLatin1String("Done")); }, 60000);
            const QString mount = mountOf();
            auto *hits = dialog.findChild<QTreeWidget *>(QStringLiteral("hits"));
            report(!mount.isEmpty() && options(mount).startsWith(QLatin1String("ro")) && hits->topLevelItemCount() == 1
                       && hits->topLevelItem(0)->text(0) == QLatin1String("/setup.exe"),
                   QStringLiteral("it's mounted read-only to scan, and the trojan is found"),
                   status->text() + QStringLiteral(" | ") + mount + QStringLiteral(" ") + options(mount));

            Answerer answerer;
            answerer.answer = [&](QWidget *modal) {
                if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                    box->button(QMessageBox::Yes)->click();
                    return true;
                }
                return false;
            };
            dialog.deleteTicked();
            waitUntil([&] { return status->text().startsWith(QLatin1String("Deleted")) || status->text().contains(QLatin1String("can't")); },
                      30000);
            const QString writable = mountOf();
            report(!writable.isEmpty() && options(writable).startsWith(QLatin1String("rw")) && !QFile::exists(writable + QStringLiteral("/setup.exe"))
                       && QFile::exists(writable + QStringLiteral("/clean.exe")),
                   QStringLiteral("to delete, it's made writable, and only the trojan goes"), status->text().left(120));
        }
        dialog.reject();
    }
    waitUntil([&] { return mountOf().isEmpty(); }, 15000);
    report(mountOf().isEmpty(), QStringLiteral("closing the window unmounts the drive it mounted"));
    cleanUpLoop(udisks, loopPath);
}

// Windows' thumbnail caches on a drive: their small pictures are listed after a search.
void lostFilesThumbnails()
{
    if (QStandardPaths::findExecutable(QStringLiteral("mkfs.fat")).isEmpty() || QStandardPaths::findExecutable(QStringLiteral("mcopy")).isEmpty()) {
        report(true, QStringLiteral("thumbnails: skipped, mkfs.fat and mtools aren't here"));
        return;
    }
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("windows.img"));
    const QString data = QStringLiteral(SOURCE_DIR "/tests/data/lostfs/types/");
    const auto run = [](const QString &program, const QStringList &args) {
        QProcess p;
        p.start(program, args);
        return p.waitForFinished(30000) && p.exitCode() == 0;
    };
    bool made = run(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("40M"), image})
             && run(QStringLiteral("mkfs.fat"), {QStringLiteral("-F"), QStringLiteral("32"), image});
    for (const char *folder : {"Users", "Users/Sam", "Users/Sam/AppData", "Users/Sam/AppData/Local", "Users/Sam/AppData/Local/Microsoft",
                               "Users/Sam/AppData/Local/Microsoft/Windows", "Users/Sam/AppData/Local/Microsoft/Windows/Explorer", "Old photos"})
        made = made && run(QStringLiteral("mmd"), {QStringLiteral("-i"), image, QStringLiteral("::/") + QString::fromLatin1(folder)});
    made = made && run(QStringLiteral("mcopy"), {QStringLiteral("-i"), image, data + QStringLiteral("thumbcache_256.db"),
                                                 QStringLiteral("::/Users/Sam/AppData/Local/Microsoft/Windows/Explorer/")})
        && run(QStringLiteral("mcopy"), {QStringLiteral("-i"), image, data + QStringLiteral("Thumbs.db"), QStringLiteral("::/Old photos/")});
    if (!made) {
        report(false, QStringLiteral("thumbnails: make the FAT image"));
        return;
    }
    UDisks udisks;
    LostFilesDialog dialog(&udisks, QString());
    dialog.show();
    dialog.scanImage(image);
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("status"));
    waitUntil([&] { return status->text().contains(QLatin1String("small pictures")); }, 60000);
    auto *deletedOnly = dialog.findChild<QCheckBox *>(QStringLiteral("deletedOnly"));
    deletedOnly->setChecked(true);
    auto *view = dialog.findChild<QListView *>(QStringLiteral("files"));
    const auto *model = qobject_cast<const LostFilesModel *>(view->model());
    QStringList names;
    for (int i = 0; i < model->fileCount(); ++i) {
        if (model->file(i).folder == QLatin1String("Small pictures Windows kept"))
            names << model->file(i).name;
    }
    names.sort();
    report(status->text().contains(QLatin1String("Windows kept 6 small pictures")) && names.size() == 6 && names.contains(QStringLiteral("beach (small copy).jpg")),
           QStringLiteral("thumbnails: a search lists the small pictures in Windows' caches on the drive"), names.join(QLatin1Char(',')));
}

// Files locked by ransomware: the window says so and explains what can be done.
void lostFilesRansomware()
{
    if (QStandardPaths::findExecutable(QStringLiteral("mkfs.fat")).isEmpty() || QStandardPaths::findExecutable(QStringLiteral("mcopy")).isEmpty()) {
        report(true, QStringLiteral("ransomware: skipped, mkfs.fat and mtools aren't here"));
        return;
    }
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("locked.img"));
    const auto run = [](const QString &program, const QStringList &args) {
        QProcess p;
        p.start(program, args);
        return p.waitForFinished(30000) && p.exitCode() == 0;
    };
    bool made = run(QStringLiteral("truncate"), {QStringLiteral("-s"), QStringLiteral("40M"), image}) && run(QStringLiteral("mkfs.fat"), {QStringLiteral("-F"), QStringLiteral("32"), image})
             && run(QStringLiteral("mmd"), {QStringLiteral("-i"), image, QStringLiteral("::/Docs")});
    for (int i = 0; i < 4 && made; ++i) {
        const QString name = dir.filePath(QStringLiteral("report%1.docx.locky").arg(i));
        QFile f(name);
        made = f.open(QIODevice::WriteOnly) && f.write(QByteArray(3000, char(0x5A + i))) > 0;
        f.close();
        made = made && run(QStringLiteral("mcopy"), {QStringLiteral("-i"), image, name, QStringLiteral("::/Docs/")});
    }
    const QString note = dir.filePath(QStringLiteral("_Locky_recover_instructions.txt"));
    QFile n(note);
    made = made && n.open(QIODevice::WriteOnly) && n.write("Your files are encrypted.\n") > 0;
    n.close();
    made = made && run(QStringLiteral("mcopy"), {QStringLiteral("-i"), image, note, QStringLiteral("::/Docs/")});
    if (!made) {
        report(false, QStringLiteral("ransomware: make the FAT image"));
        return;
    }
    UDisks udisks;
    LostFilesDialog dialog(&udisks, QString());
    dialog.show();
    dialog.scanImage(image);
    auto *bar = dialog.findChild<QWidget *>(QStringLiteral("ransomBar"));
    auto *text = dialog.findChild<QLabel *>(QStringLiteral("ransom"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("status"));
    waitUntil([&] { return status->text().startsWith(QLatin1String("Done")); }, 60000);
    report(bar->isVisible() && text->text().contains(QLatin1String("Locky")) && text->text().contains(QLatin1String("4 files end in .locky")),
           QStringLiteral("ransomware: the window says the files look locked, and by what"), text->text());
    QStringList boxes;
    Answerer answerer;
    answerer.answer = [&](QWidget *modal) {
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            boxes << box->text();
            box->close();
            return true;
        }
        return false;
    };
    dialog.findChild<QPushButton *>(QStringLiteral("ransomWhatToDo"))->click();
    report(boxes.size() == 1 && boxes[0].contains(QLatin1String("No More Ransom")) && boxes[0].contains(QLatin1String("Don't pay"))
               && boxes[0].contains(QLatin1String("No free decryptor for it is known yet")),
           QStringLiteral("ransomware: What to Do explains, with No More Ransom"), boxes.value(0).left(120));
}

// Keep a copy of what's read: the search reads the drive through into a Rescue Copy image.
void lostFilesReadThrough()
{
    QImage picture(160, 100, QImage::Format_RGB32);
    picture.fill(QColor(0xff, 0x33, 0x55));
    QByteArray png;
    {
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        picture.save(&buffer, "PNG");
    }
    QByteArray data(8 * 1024 * 1024, '\0');
    data.replace(3 * 1024 * 1024, png.size(), png);
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("failing rescue.img"));
    UDisks udisks;
    LostFilesDialog dialog(&udisks, QString());
    dialog.show();
    dialog.scanReadThrough(lost::Source::fromMemory(data), QStringLiteral("Failing drive"), image);
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("status"));
    waitUntil([&] { return status->text().startsWith(QLatin1String("Done")); }, 60000);
    QFile copy(image);
    const QByteArray copied = copy.open(QIODevice::ReadOnly) ? copy.readAll() : QByteArray();
    report(status->text().contains(QLatin1String("What was read")) && status->text().contains(QLatin1String("Rescue Copy can finish"))
               && QFileInfo::exists(image + QStringLiteral(".map")) && copied.size() == data.size() && copied.mid(3 * 1024 * 1024, png.size()) == png,
           QStringLiteral("read through: the search fills a Rescue Copy image and its map, and says so"), status->text().right(120));
}

// btrfs: files deleted since come back out of an older copy of its file tree (btrfs-progs).
void lostFilesBtrfs()
{
    if (!btrfsrestore::available()) {
        report(true, QStringLiteral("btrfs: skipped, btrfs-progs isn't here"));
        return;
    }
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("photos.img"));
    QFile out(image);
    QProcess unpack;
    unpack.start(QStringLiteral("zstd"), {QStringLiteral("-q"), QStringLiteral("-d"), QStringLiteral(SOURCE_DIR "/tests/data/lostfs/btrfs.img.zst"), QStringLiteral("-o"), image});
    if (!unpack.waitForFinished(60000) || unpack.exitCode() != 0) {
        report(false, QStringLiteral("btrfs: unpack the test image"));
        return;
    }
    UDisks udisks;
    LostFilesDialog dialog(&udisks, QString());
    dialog.show();
    dialog.scanImage(image);
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("status"));
    waitUntil([&] { return status->text().startsWith(QLatin1String("Done")); }, 60000);
    auto *bar = dialog.findChild<QWidget *>(QStringLiteral("btrfsBar"));
    report(bar->isVisible(), QStringLiteral("btrfs: the window offers its older copies"));
    const QString target = dir.filePath(QStringLiteral("back"));
    QDir().mkpath(target);
    QStringList listed, said;
    Answerer answerer;
    int step = 0;
    answerer.answer = [&](QWidget *modal) {
        auto *btrfs = qobject_cast<BtrfsDialog *>(modal);
        if (!btrfs)
            return false;
        auto *files = btrfs->findChild<QTreeWidget *>(QStringLiteral("treeFiles"));
        auto *text = btrfs->findChild<QLabel *>(QStringLiteral("btrfsStatus"));
        if (step == 0 && files->topLevelItemCount() > 0 && !btrfs->busy()) {
            for (int i = 0; i < files->topLevelItemCount(); ++i)
                listed << files->topLevelItem(i)->text(0) + QLatin1Char(' ') + files->topLevelItem(i)->text(1);
            btrfs->setTargetForTests(target);
            btrfs->findChild<QPushButton *>(QStringLiteral("copyTicked"))->click();
            step = 1;
        } else if (step == 1 && !btrfs->busy() && text->text().startsWith(QLatin1String("Copied"))) {
            said << text->text();
            btrfs->reject();
            step = 2;
        }
        return true;
    };
    dialog.findChild<QPushButton *>(QStringLiteral("btrfsLook"))->click();
    listed.sort();
    const QStringList restored = QDir(target).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    const QString copy = restored.isEmpty() ? QString() : target + QLatin1Char('/') + restored.first();
    report(listed == QStringList{QStringLiteral("/Notes/ferry.txt gone"), QStringLiteral("/Pictures/2024/beach2.jpg gone"), QStringLiteral("/Pictures/2024/beach4.jpg gone")}
               && said.size() == 1 && said[0].startsWith(QLatin1String("Copied 3 files"))
               && QFileInfo(copy + QStringLiteral("/Notes/ferry.txt")).size() == 850 && QFileInfo(copy + QStringLiteral("/Pictures/2024/beach4.jpg")).size() == 24006,
           QStringLiteral("btrfs: the files deleted since are listed from the older copy, and copied back whole"),
           listed.join(QLatin1Char(',')) + QStringLiteral(" | ") + said.join(QLatin1Char(' ')));

    // The same file system as a partition on a whole disk's image: it's copied out of the image first.
    const QString disk = dir.filePath(QStringLiteral("disk.img"));
    QFile part(image), whole(disk);
    const qint64 at = 1024 * 1024;
    bool made = part.open(QIODevice::ReadOnly) && whole.open(QIODevice::WriteOnly) && whole.resize(at + part.size() + at) && whole.seek(at)
             && whole.write(part.readAll()) == part.size();
    whole.close();
    QProcess table;
    table.start(QStringLiteral("sfdisk"), {QStringLiteral("--quiet"), disk});
    table.write("label: gpt\nstart=2048, size=" + QByteArray::number(part.size() / 512) + ", type=linux\n");
    table.closeWriteChannel();
    made = made && table.waitForFinished(30000) && table.exitCode() == 0;
    if (!made) {
        report(false, QStringLiteral("btrfs: make the disk image"));
        return;
    }
    dialog.scanImage(disk);
    waitUntil([&] { return status->text().startsWith(QLatin1String("Done")); }, 60000);
    listed.clear();
    said.clear();
    step = 0;
    const QString target2 = dir.filePath(QStringLiteral("back2"));
    QDir().mkpath(target2);
    answerer.answer = [&](QWidget *modal) {
        auto *btrfs = qobject_cast<BtrfsDialog *>(modal);
        if (!btrfs)
            return false;
        auto *files = btrfs->findChild<QTreeWidget *>(QStringLiteral("treeFiles"));
        if (step == 0 && files->topLevelItemCount() > 0 && !btrfs->busy()) {
            for (int i = 0; i < files->topLevelItemCount(); ++i)
                listed << files->topLevelItem(i)->text(0);
            step = 1;
            btrfs->reject();
        }
        return true;
    };
    dialog.findChild<QPushButton *>(QStringLiteral("btrfsLook"))->click();
    report(bar->isVisible() && listed.size() == 3, QStringLiteral("btrfs: in a whole disk's image, its partition is copied out and read the same way"),
           listed.join(QLatin1Char(',')));

    // As on DiskAnvil Live: the drive read as root, through a read-only loop device at the
    // partition (tried here on the disk image, when sudo needs no password).
    if (QProcess::execute(QStringLiteral("sudo"), {QStringLiteral("-n"), QStringLiteral("true")}) != 0) {
        report(true, QStringLiteral("btrfs: the DiskAnvil Live way skipped, sudo wants a password here"));
        return;
    }
    QStringList live;
    {
        BtrfsDialog root(disk, QString(), QStringLiteral("disk"), nullptr, true, quint64(at), quint64(part.size()));
        root.show();
        auto *files = root.findChild<QTreeWidget *>(QStringLiteral("treeFiles"));
        waitUntil([&] { return files->topLevelItemCount() >= 3 && !root.busy(); }, 60000);
        for (int i = 0; i < files->topLevelItemCount(); ++i)
            live << files->topLevelItem(i)->text(0);
    }
    QProcess loops;
    loops.start(QStringLiteral("losetup"), {QStringLiteral("-j"), disk});
    loops.waitForFinished(10000);
    report(live.size() == 3 && loops.readAll().trimmed().isEmpty(),
           QStringLiteral("btrfs: the DiskAnvil Live way, read as root through a loop device that's taken away after"), live.join(QLatin1Char(',')));
}

// "What Happened?": from what happened and which drive, to the odds and the right tool.
void whatHappened()
{
    Disk ssd;
    ssd.blockPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/nvme0n1");
    ssd.device = QStringLiteral("/dev/nvme0n1");
    ssd.model = QStringLiteral("Fast SSD");
    ssd.size = 512ull * 1000 * 1000 * 1000;
    ssd.rotationRate = 0;
    ssd.discard = true;
    Disk hdd;
    hdd.blockPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/sdb");
    hdd.device = QStringLiteral("/dev/sdb");
    hdd.model = QStringLiteral("Old HDD");
    hdd.size = 500ull * 1000 * 1000 * 1000;
    hdd.rotationRate = 5400;
    hdd.health.state = Health::State::Failing;
    hdd.health.pendingSectors = 8;
    WhatHappenedDialog dialog({ssd, hdd}, false);
    dialog.setTrimForTests(triage::Trim::Continuous);
    dialog.show();
    QStringList opened;
    QObject::connect(&dialog, &WhatHappenedDialog::openTool, &dialog,
                     [&opened](const QString &tool, const QString &path) { opened << tool + QLatin1Char('@') + path.section(QLatin1Char('/'), -1); });
    auto *drives = dialog.findChild<QListWidget *>(QStringLiteral("drives"));
    auto *odds = dialog.findChild<QLabel *>(QStringLiteral("odds"));
    dialog.findChild<QCommandLinkButton *>(QStringLiteral("deleted"))->click();
    drives->setCurrentRow(0);
    dialog.findChild<QPushButton *>(QStringLiteral("next"))->click();
    const QString ssdOdds = odds->text();
    dialog.findChild<QPushButton *>(QStringLiteral("tool-lost-files"))->click();
    report(ssdOdds.contains(QLatin1String("Poor")) && ssdOdds.contains(QLatin1String("TRIM is on")) && opened == QStringList{QStringLiteral("lost-files@nvme0n1")},
           QStringLiteral("what happened: deleted files on an SSD that trims: poor odds, said why, and Find Lost Files opens on it"),
           opened.join(',') + QStringLiteral(" | ") + ssdOdds.left(80));
    // A failing drive: a Rescue Copy first.
    drives->setCurrentRow(1);
    dialog.findChild<QPushButton *>(QStringLiteral("next"))->click();
    QCoreApplication::processEvents(); // new buttons are shown on the next turn of the event loop
    QStringList buttons;
    for (QPushButton *b : dialog.findChildren<QPushButton *>()) {
        if (b->objectName().startsWith(QLatin1String("tool-")) && b->isVisible())
            buttons << b->objectName().mid(5);
    }
    if (qEnvironmentVariableIsSet("DISKANVIL_UITEST_SHOTS"))
        dialog.grab().save(qEnvironmentVariable("DISKANVIL_UITEST_SHOTS") + QStringLiteral("/what-happened.png"));
    report(drives->item(1)->text().contains(QLatin1String("failing")) && buttons.value(0) == QLatin1String("rescue-copy"),
           QStringLiteral("what happened: on a failing drive, Make a Rescue Copy is the first button"), buttons.join(','));
}

// Repairing through the window: a copy is repaired, and joins the list to be looked at and saved.
void lostFilesRepaired()
{
    QImage image(200, 120, QImage::Format_RGB32);
    image.fill(QColor(0x1f, 0x6f, 0xff));
    QByteArray png;
    {
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
    }
    const qsizetype idat = png.indexOf("IDAT");
    png[idat + 4 + qFromBigEndian<quint32>(png.constData() + idat - 4) + 2] ^= 0x40; // its checksum
    const auto film = [](const char *name) {
        QFile f(QStringLiteral(SOURCE_DIR "/tests/data/lostfs/repair/") + QString::fromLatin1(name));
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    const QByteArray whole = film("h264-pcm.mov");
    const QByteArray broken = whole.left(whole.lastIndexOf("moov") - 4); // the recording stopped
    QTemporaryDir dir;
    const QString reference = dir.filePath(QStringLiteral("working clip.mov"));
    QFile ref(reference);
    if (!ref.open(QIODevice::WriteOnly) || ref.write(film("h264-pcm-ref.mov")) <= 0) {
        report(false, QStringLiteral("write the working clip"));
        return;
    }
    ref.close();
    QByteArray data(8 * 1024 * 1024, '\0');
    data.replace(64 * 1024, png.size(), png);
    data.replace(1024 * 1024, broken.size(), broken);
    const QString imagePath = dir.filePath(QStringLiteral("repair.img"));
    QFile imageFile(imagePath);
    if (!imageFile.open(QIODevice::WriteOnly) || imageFile.write(data) != data.size()) {
        report(false, QStringLiteral("write the test image"));
        return;
    }
    imageFile.close();

    UDisks udisks;
    QSettings().remove(QStringLiteral("lostFiles/order"));
    LostFilesDialog dialog(&udisks, QString());
    dialog.show();
    dialog.scanImage(imagePath);
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("status"));
    waitUntil([&] { return status->text().startsWith(QLatin1String("Done")); }, 60000);
    auto *view = dialog.findChild<QListView *>(QStringLiteral("files"));
    const auto *model = qobject_cast<const LostFilesModel *>(view->model());
    int pngIndex = -1, filmIndex = -1;
    for (int i = 0; i < model->fileCount(); ++i) {
        const QString id = model->file(i).type >= 0 ? filetypes::types()[model->file(i).type].id : QString();
        if (id == QLatin1String("png"))
            pngIndex = i;
        else if (id == QLatin1String("mov") || id == QLatin1String("mp4"))
            filmIndex = i;
    }
    report(pngIndex >= 0 && filmIndex >= 0, QStringLiteral("repair: the damaged picture and the film without its index are found"),
           QStringLiteral("%1 files").arg(model->fileCount()));
    if (pngIndex < 0 || filmIndex < 0)
        return;

    const auto readAll = [&dialog, model](int index) {
        const lost::Found &f = model->file(index);
        lost::FileReader reader(f.from, f);
        return reader.head(qint64(f.size));
    };
    const int before = model->fileCount();
    dialog.repairForTests(pngIndex);
    waitUntil([&] { return !dialog.repairing() && model->fileCount() > before; }, 30000);
    const lost::Found fixedPng = model->file(model->fileCount() - 1);
    report(model->fileCount() == before + 1 && fixedPng.name.endsWith(QLatin1String("(repaired).png")) && fixedPng.folder == QLatin1String("Repaired copies")
               && !QImage::fromData(readAll(model->fileCount() - 1), "PNG").isNull() && status->text().contains(QLatin1String("checksum")),
           QStringLiteral("repair: the picture's repaired copy joins the list, and opens"), fixedPng.name + QStringLiteral(" | ") + status->text());

    // The film asks for a working clip first; it's picked in the file dialog.
    QStringList asked;
    Answerer answerer;
    answerer.answer = [&](QWidget *modal) {
        if (auto *picker = qobject_cast<QFileDialog *>(modal)) {
            asked << picker->windowTitle();
            // As a person would: the file's path typed in, then Open.
            picker->selectFile(reference);
            if (auto *name = picker->findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
                name->setText(reference);
            static_cast<QDialog *>(picker)->accept();
            return true;
        }
        return false;
    };
    const int withPng = model->fileCount();
    dialog.repairForTests(filmIndex);
    waitUntil([&] { return !dialog.repairing() && model->fileCount() > withPng; }, 60000);
    const QByteArray fixedFilm = readAll(model->fileCount() - 1);
    QStringList problems;
    for (const structure::Part &p : structure::parts(model->file(model->fileCount() - 1).type, fixedFilm))
        if (!p.problem.isEmpty())
            problems << p.name;
    report(asked.size() == 1 && asked[0].contains(QLatin1String("Clip That Plays")) && model->fileCount() == withPng + 1 && fixedFilm.contains("moov") && problems.isEmpty()
               && model->file(model->fileCount() - 1).note.contains(QLatin1String("60 frames")),
           QStringLiteral("repair: the film asks for a clip that plays, and its repaired copy with a new index joins the list"),
           asked.join(QLatin1Char('|')) + QStringLiteral(" | ") + model->file(model->fileCount() - 1).note);
}

// Organising what was found: names from inside, sorting, small pictures, copies and files that
// don't open.
void lostFilesOrganised()
{
    // A photo with the camera's date and name in it (EXIF), little-endian.
    const auto le = [](quint32 v, int bytes) {
        QByteArray b(bytes, '\0');
        for (int i = 0; i < bytes; ++i)
            b[i] = char(v >> (8 * i));
        return b;
    };
    QByteArray tiff = QByteArray("II*\0", 4) + le(8, 4);
    tiff += le(3, 2) + le(0x010F, 2) + le(2, 2) + le(5, 4) + le(50, 4) + le(0x0110, 2) + le(2, 2) + le(9, 4) + le(56, 4) + le(0x8769, 2) + le(4, 2)
          + le(1, 4) + le(66, 4) + le(0, 4);
    tiff += QByteArray("Test\0\0", 6) + QByteArray("Test Cam\0\0", 10);
    tiff += le(1, 2) + le(0x9003, 2) + le(2, 2) + le(20, 4) + le(84, 4) + le(0, 4) + QByteArray("2024:07:14 13:22:05\0", 20);
    const auto encoded = [](const QImage &image, const char *format) {
        QByteArray file;
        QBuffer buffer(&file);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, format);
        return file;
    };
    QImage photoImage(400, 300, QImage::Format_RGB32);
    QRandomGenerator rng(11);
    for (int y = 0; y < photoImage.height(); ++y)
        for (int x = 0; x < photoImage.width(); ++x)
            photoImage.setPixel(x, y, qRgb(((x * 3) & 0xFF) ^ int(rng.bounded(96)), (y * 5) & 0xFF, int(rng.bounded(256))));
    QByteArray photo = encoded(photoImage, "JPG");
    const QByteArray app1 = QByteArray("Exif\0\0", 6) + tiff;
    photo.insert(2, QByteArray("\xFF\xE1", 2) + char((app1.size() + 2) >> 8) + char((app1.size() + 2) & 0xFF) + app1);
    QImage noise(300, 300, QImage::Format_RGB32);
    for (int y = 0; y < 300; ++y)
        for (int x = 0; x < 300; ++x)
            noise.setPixel(x, y, rng.generate());
    const QByteArray big = encoded(noise, "PNG");
    QImage iconImage(16, 16, QImage::Format_RGB32);
    iconImage.fill(QColor(0x2b, 0xee, 0x8a));
    const QByteArray icon = encoded(iconImage, "PNG");
    // Damaged inside its picture data (its checksum no longer matches): it doesn't open.
    QByteArray broken = encoded(noise.copy(0, 0, 200, 200), "PNG");
    const qsizetype idat = broken.indexOf("IDAT");
    for (int k = 0; k < 64; ++k)
        broken[idat + 200 + k] = char(k * 37);

    QTemporaryDir dir;
    QByteArray data(16 * 1024 * 1024, '\0');
    quint64 at = 64 * 1024;
    for (const QByteArray &file : {photo, big, icon, photo, broken}) {
        data.replace(qsizetype(at), file.size(), file);
        at = (at + quint64(file.size()) + 64 * 1024) / 512 * 512;
    }
    const QString imagePath = dir.filePath(QStringLiteral("organise.img"));
    QFile imageFile(imagePath);
    if (!imageFile.open(QIODevice::WriteOnly) || imageFile.write(data) != data.size()) {
        report(false, QStringLiteral("write the test image"));
        return;
    }
    imageFile.close();

    UDisks udisks;
    QSettings().remove(QStringLiteral("lostFiles/order"));
    LostFilesDialog dialog(&udisks, QString());
    dialog.show();
    dialog.scanImage(imagePath);
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("status"));
    waitUntil([&] { return status->text().startsWith(QLatin1String("Done")); }, 60000);
    auto *view = dialog.findChild<QListView *>(QStringLiteral("files"));
    QAbstractItemModel *model = view->model();
    const auto names = [model] {
        QStringList out;
        for (int row = 0; row < model->rowCount(); ++row)
            out << model->index(row, 0).data().toString();
        return out;
    };
    report(model->rowCount() == 5 && names().count(QStringLiteral("2024-07-14 13.22.05 Test Cam.jpg")) == 2,
           QStringLiteral("organise: five found, the photos named by when they were taken and the camera"), names().join(QStringLiteral(" | ")));

    auto *sort = dialog.findChild<QComboBox *>(QStringLiteral("sort"));
    sort->setCurrentIndex(sort->findData(int(LostFilter::Order::Size)));
    const QStringList bySize = names();
    sort->setCurrentIndex(sort->findData(int(LostFilter::Order::Date)));
    const QStringList byDate = names();
    sort->setCurrentIndex(sort->findData(int(LostFilter::Order::Name)));
    const QStringList byName = names();
    QStringList sorted = byName;
    std::sort(sorted.begin(), sorted.end(), [](const QString &a, const QString &b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    report(bySize.value(0) == QStringLiteral("Picture 000002.png") && byDate.value(0).startsWith(QLatin1String("2024-07-14"))
               && byDate.value(1).startsWith(QLatin1String("2024-07-14")) && byName == sorted,
           QStringLiteral("organise: by size, by date and by name"), bySize.join(QStringLiteral(" | ")));
    sort->setCurrentIndex(sort->findData(int(LostFilter::Order::Found)));

    auto *small = dialog.findChild<QCheckBox *>(QStringLiteral("hideSmall"));
    small->setChecked(true);
    report(model->rowCount() == 4 && !names().contains(QStringLiteral("Picture 000003.png")), QStringLiteral("organise: small pictures hidden"),
           names().join(QStringLiteral(" | ")));
    small->setChecked(false);

    auto *findCopies = dialog.findChild<QAction *>(QStringLiteral("findCopies"));
    auto *hideCopies = dialog.findChild<QCheckBox *>(QStringLiteral("hideCopies"));
    findCopies->trigger();
    waitUntil([&] { return hideCopies->isVisible(); }, 30000);
    hideCopies->setChecked(true);
    report(hideCopies->text().startsWith(QLatin1String("Hide copies (1,")) && model->rowCount() == 4
               && names().count(QStringLiteral("2024-07-14 13.22.05 Test Cam.jpg")) == 1,
           QStringLiteral("organise: the copy of the photo is found and can be hidden"), hideCopies->text());
    hideCopies->setChecked(false);

    auto *checkOpen = dialog.findChild<QAction *>(QStringLiteral("checkOpen"));
    auto *hideBroken = dialog.findChild<QCheckBox *>(QStringLiteral("hideBroken"));
    checkOpen->trigger();
    waitUntil([&] { return hideBroken->isVisible(); }, 120000);
    hideBroken->setChecked(true);
    report(hideBroken->text() == QLatin1String("Hide ones that don't open (1)") && model->rowCount() == 4,
           QStringLiteral("organise: the damaged picture is found not to open, and can be hidden"), hideBroken->text());
    // Find by Words: the camera's name is in each photo's EXIF, as text.
    hideBroken->setChecked(false);
    const int beforeWords = model->rowCount();
    dialog.findByWordsForTests(QStringLiteral("\"test cam\""));
    waitUntil([&] { return !dialog.searchingWords(); }, 60000);
    report(status->text().startsWith(QLatin1String("\"test cam\" was found in 2 places")) && model->rowCount() == beforeWords + 2,
           QStringLiteral("organise: words remembered are found, and the text around them listed"), status->text());
    if (qEnvironmentVariableIsSet("DISKANVIL_UITEST_SHOTS")) {
        QCoreApplication::processEvents();
        dialog.grab().save(qEnvironmentVariable("DISKANVIL_UITEST_SHOTS") + QStringLiteral("/lostfiles-organised.png"));
    }
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

    // Saving doesn't end it: the files stay, each marked as saved, for saving more.
    auto *pages = dialog.findChild<QStackedWidget *>();
    int marked = 0;
    for (int row = 0; row < view->model()->rowCount(); ++row)
        marked += !view->model()->index(row, 0).data(Qt::UserRole + 2).toString().isEmpty();
    report(pages && pages->currentIndex() == 1 && view->isVisible() && view->model()->rowCount() == 3 && marked == 3,
           QStringLiteral("after saving, the files are still there, each marked as saved"), QStringLiteral("%1 marked").arg(marked));
    // Enter used to press Close, and everything found was gone.
    view->setFocus();
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(view, &enter);
    QCoreApplication::processEvents();
    report(dialog.isVisible(), QStringLiteral("Enter doesn't close the window"));
    // And again, somewhere else.
    const QString saveAgain = dir.filePath(QStringLiteral("saved again"));
    QDir().mkpath(saveAgain);
    {
        Answerer answerer;
        answerer.answer = [](QWidget *modal) {
            if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                box->button(QMessageBox::No) ? box->button(QMessageBox::No)->click() : box->accept();
                return true;
            }
            return false;
        };
        dialog.saveTo(saveAgain);
        waitUntil([&] { return saveStatus->text().startsWith(QLatin1String("Saved")) && saveStatus->text().contains(saveAgain); }, 30000);
    }
    int savedAgain = 0;
    bool sums = false;
    QDirIterator it2(saveAgain, QDir::Files, QDirIterator::Subdirectories);
    while (it2.hasNext()) {
        const QString file = it2.next();
        sums = sums || file.endsWith(QLatin1String("/SHA256SUMS"));
        savedAgain += !file.endsWith(QLatin1String(".txt")) && !file.endsWith(QLatin1String("/SHA256SUMS"));
    }
    report(savedAgain == 3 && sums && view->model()->rowCount() == 3, QStringLiteral("a second save works, from the same results, with SHA256SUMS"),
           saveStatus->text().left(80));
    auto *panes = dialog.findChild<QSplitter *>(QStringLiteral("panes"));
    report(panes && panes->widget(0)->maximumWidth() > 4000 && panes->widget(0)->width() > 0,
           QStringLiteral("the kinds and folders pane isn't capped, so long names fit when it's made wider"));
    const QRect room = dialog.screen()->availableGeometry();
    report(room.contains(dialog.frameGeometry()), QStringLiteral("the window fits on its screen"),
           QStringLiteral("%1x%2 on %3x%4").arg(dialog.width()).arg(dialog.height()).arg(room.width()).arg(room.height()));
    // DISKANVIL_SHOTS=<folder>: a picture of the window as it is now, to look at by eye.
    if (const QString shots = qEnvironmentVariable("DISKANVIL_SHOTS"); !shots.isEmpty()) {
        view->setCurrentIndex(view->model()->index(0, 0));
        waitUntil([&] { return !preview->pixmap().isNull(); }, 10000);
        dialog.grab().save(QDir(shots).filePath(QStringLiteral("lostfiles-%1x%2.png").arg(room.width()).arg(room.height())));
    }

    // The same, as if the files were on the drive a folder in the home folder is on (/tmp can be
    // in memory): saving there is refused.
    QTemporaryDir onDisk(QDir::homePath() + QStringLiteral("/.cache/diskanvil-uitest-XXXXXX"));
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
    // Double-click: the viewer, a program of its own (in its sandbox where there's one), says it's ready.
    Q_EMIT view->doubleClicked(view->model()->index(0, 0));
    const int first = qobject_cast<const QAbstractItemModel *>(view->model())->index(0, 0).data(Qt::UserRole).toInt();
    waitUntil([&] { return dialog.viewerShown() == first; }, 20000);
    report(dialog.viewerReady() && dialog.viewerShown() == first, QStringLiteral("double-click opens the viewer, a program of its own, and it shows that file"),
           QStringLiteral("shown %1, wanted %2").arg(dialog.viewerShown()).arg(first));
    // Its changed copy (from Inside, Change Bytes): saved with the changes, never onto the drive searched.
    viewer::Message changed;
    changed.type = viewer::Message::Edited;
    changed.edits = {{2, 0x00}, {3, 0x7F}};
    boxes.clear();
    dialog.setChangedCopyTarget(onDisk.filePath(QStringLiteral("changed.bin")));
    dialog.hearFromViewer(viewer::encode(changed));
    const bool refused = boxes.size() == 1 && boxes[0].contains(QLatin1String("drive being searched")) && !QFileInfo::exists(onDisk.filePath(QStringLiteral("changed.bin")));
    // Somewhere surely not on that drive: memory (TMPDIR can be on it, as when staging a release).
    QTemporaryDir elsewhere(QFileInfo(QStringLiteral("/dev/shm")).isWritable() ? QStringLiteral("/dev/shm/diskanvil-uitest-XXXXXX") : QDir::tempPath() + QStringLiteral("/diskanvil-uitest-XXXXXX"));
    dialog.setChangedCopyTarget(elsewhere.filePath(QStringLiteral("changed.bin")));
    dialog.hearFromViewer(viewer::encode(changed));
    QFile changedFile(elsewhere.filePath(QStringLiteral("changed.bin")));
    const QByteArray changedBytes = changedFile.open(QIODevice::ReadOnly) ? changedFile.readAll() : QByteArray();
    report(refused && changedBytes.size() > 4 && changedBytes[2] == '\0' && changedBytes[3] == '\x7F',
           QStringLiteral("the viewer's changed copy is saved with its changes, and never onto the drive searched"),
           boxes.join(QStringLiteral(" | ")).left(100));
    // Enhance profiles the viewer asks to keep: it can't reach the home folder, so they're kept here, checked.
    QTemporaryDir kept;
    dialog.setProfilesFolder(kept.path());
    const auto ask = [&](viewer::ProfileDo what, const QString &profileName, const QByteArray &file, const QStringList &files = {}) {
        viewer::Message m;
        m.type = viewer::Message::Profile;
        m.profileDo = what;
        m.profileName = profileName;
        m.profile = file;
        m.dropped = files;
        dialog.hearFromViewer(viewer::encode(m));
    };
    enhance::Profile gate;
    gate.name = QStringLiteral("Gate camera");
    gate.settings.shadows = 40;
    ask(viewer::ProfileDo::Save, gate.name, enhance::profileToFile(gate));
    ask(viewer::ProfileDo::Save, QStringLiteral("bad"), QByteArray("{not a profile"));
    const bool keptOne = enhance::savedProfiles(kept.path()).size() == 1;
    // Files dropped on the viewer: a good one added, a bad one refused.
    enhance::Profile given;
    given.name = QStringLiteral("From a friend");
    given.settings.sharpen = 45;
    QFile good(elsewhere.filePath(QStringLiteral("friend.anvil-enhance"))), badFile(elsewhere.filePath(QStringLiteral("bad.anvil-enhance")));
    if (good.open(QIODevice::WriteOnly))
        good.write(enhance::profileToFile(given));
    good.close();
    if (badFile.open(QIODevice::WriteOnly))
        badFile.write("MZ\x90\x00");
    badFile.close();
    ask(viewer::ProfileDo::Dropped, {}, {}, {good.fileName(), badFile.fileName()});
    QStringList names;
    for (const enhance::Profile &p : enhance::savedProfiles(kept.path()))
        names << p.name;
    report(keptOne && names == QStringList{QStringLiteral("From a friend"), QStringLiteral("Gate camera")},
           QStringLiteral("enhance profiles the viewer saves, and good ones dropped on it, are kept; what isn't one is refused"), names.join(QStringLiteral(", ")));
    dialog.setChangedCopyTarget(elsewhere.filePath(QStringLiteral("shared.anvil-enhance")));
    ask(viewer::ProfileDo::Export, given.name, enhance::profileToFile(given));
    enhance::Profile shared;
    QString sharedWhy;
    QFile sharedFile(elsewhere.filePath(QStringLiteral("shared.anvil-enhance")));
    const bool exported = sharedFile.open(QIODevice::ReadOnly) && enhance::profileFromFile(sharedFile.readAll(), &shared, &sharedWhy) && shared.settings.sharpen == 45;
    ask(viewer::ProfileDo::Remove, QStringLiteral("Gate camera"), {});
    report(exported && enhance::savedProfiles(kept.path()).size() == 1, QStringLiteral("enhance profiles: one saved as a file to pass on, and one removed"));
    dialog.setChangedCopyTarget(QString());
    // Closing with files found asks first, and No keeps them.
    boxes.clear();
    answerer.answer = [&](QWidget *modal) {
        if (auto *box = qobject_cast<QMessageBox *>(modal)) {
            boxes << box->text();
            box->button(QMessageBox::No)->click();
            return true;
        }
        return false;
    };
    dialog.close();
    QCoreApplication::processEvents();
    report(dialog.isVisible() && boxes.size() == 1 && boxes[0].startsWith(QLatin1String("Close Find Lost Files?")),
           QStringLiteral("closing with files found asks first, and No keeps them"), boxes.join(QStringLiteral(" | ")).left(100));
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
        check(dialog, QStringLiteral("Make a DiskAnvil Live USB"));
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

// In DiskAnvil Live the few things that only change the running system are off and say why;
// the USB tools and everything else stay on.
void rescueMode()
{
    const QString realConf = rescue::rescueConfPath;
    QTemporaryDir dir;
    rescue::rescueConfPath = dir.filePath(QStringLiteral("diskanvil-live.conf"));
    UDisks udisks;
    const QStringList offInRescue = {QStringLiteral("Mount at Startup"), QStringLiteral("Disk Cleanup"), QStringLiteral("Btrfs Snapshots")};
    const QStringList alwaysOn = {QStringLiteral("Write Image to USB"), QStringLiteral("Make a Windows USB"),
                                  QStringLiteral("Make a DiskAnvil Live USB"), QStringLiteral("Check a USB Stick"),
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

// `only`: just the scenarios with these names (--only=viruses,home), for when one is being worked on.
int userScenarios(const QStringList &only)
{
    QTemporaryDir home;
    qputenv("XDG_DATA_HOME", QFile::encodeName(home.filePath(QStringLiteral("data"))));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(home.filePath(QStringLiteral("config"))));
    const QVector<QPair<const char *, void (*)()>> scenarios = {
        {"output", outputWindow},      {"addons", addonMenus},          {"themes", themes},
        {"maker", maker},              {"banner", banner},              {"jobs", jobBars},
        {"rescueusb", rescueUsb},      {"livelayout", rescueUsbLayout},      {"writeblock", writeBlockMode},      {"oldname", oldNameMoveOver},      {"remotehelp", remoteHelpWindow},      {"liveonpc", liveOnPcWindow},      {"netboot", netBootWindow},      {"stickcheck", stickCheck},      {"sdcard", sdCardFormat},        {"refresh", slowAreaRefresh},    {"sectorsize", sectorSizeWindow}, {"veracrypt", veraCryptWindow}, {"productkey", productKeyWindow}, {"drivers", driversWindow}, {"savefiles", saveFilesWindow}, {"bootmenu", bootMenuWindow}, {"linuxboot", linuxBootWindow}, {"clonezilla", clonezillaRestore}, {"testboot", testBootWindow}, {"certificate", certificateWindow}, {"report", driveReport}, {"evidence", evidenceWindow}, {"timeline", timelineWindow}, {"places", placesWindow}, {"activity", activityWindow}, {"enhance", enhanceWindow}, {"film", filmTools}, {"isocopy", isoCopy},
        {"windowsusb", windowsUsb},    {"rescuemode", rescueMode},      {"enterkey", enterKey},
        {"home", homeScreen},          {"lostfiles", lostFilesThroughWindow}, {"lostorganise", lostFilesOrganised}, {"lostrepair", lostFilesRepaired}, {"lostthumbs", lostFilesThumbnails}, {"lostransom", lostFilesRansomware}, {"lostthrough", lostFilesReadThrough}, {"lostbtrfs", lostFilesBtrfs}, {"whathappened", whatHappened}, {"viewer", viewerWindow}, {"lostfs", lostFilesFromFileSystem},
        {"lostrecent", lostFilesDeletedRecently}, {"lostcopies", lostFilesFromShadowCopies}, {"lostvdisk", lostFilesFromVirtualDisk},
        {"previews", previewsThroughHelper}, {"viruses", virusScanThroughWindow}, {"virusdrive", virusScanOnDrive}, {"viruswindows", virusScanWindowsDrive}, {"virusboot", virusScanBoot}, {"viruslookup", virusLookUpOnline}, {"newids", newIdsThroughWindow},
    };
    for (const auto &[name, run] : scenarios) {
        if (only.isEmpty() || only.contains(QLatin1String(name)))
            run();
    }
    out << (failures ? QStringLiteral("%1 step(s) failed").arg(failures) : QStringLiteral("All steps passed")) << Qt::endl;
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char *argv[])
{
    // Find Lost Files starts this program as its preview helper, as it would DiskAnvil itself.
    if (argc == 2 && QByteArray(argv[1]) == "--preview-helper") {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QGuiApplication helper(argc, argv);
        return preview::runHelper();
    }
    QApplication app(argc, argv);
    wof::setInteractive(false); // never a password window in the middle of the tests
    // ...and as its viewer.
    if (argc == 2 && QByteArray(argv[1]) == "--viewer")
        return viewer::run();
    installTranslations();
    // Qt's own file dialogs, which the tests can see and answer (the desktop's own aren't widgets).
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QStringList only;
    for (const QString &arg : app.arguments()) {
        if (arg.startsWith(QLatin1String("--only=")))
            only = arg.mid(7).split(QLatin1Char(','));
    }
    // As the user: the add-on, menu and theme parts, which need no test disks.
    if (app.arguments().contains(QStringLiteral("--user")))
        return userScenarios(only);
    if (geteuid() != 0) {
        out << "Run as root: it creates a loop device (or use --user for the parts that don't)." << Qt::endl;
        return 2;
    }
    // As root, the ones that make their own loop device can run by themselves.
    if (!only.isEmpty()) {
        const QVector<QPair<const char *, void (*)()>> alone = {{"stickcheck", stickCheck}, {"sdcard", sdCardFormat}, {"refresh", slowAreaRefresh}, {"stopwipe", stopWipe}, {"luksheader", luksHeaderThroughWindow}, {"nas", nasDrives}, {"tryrepair", tryRepairFirst}, {"clonezilla", clonezillaRestore}, {"testboot", testBootWindow}, {"wipecert", wipeCertificate}, {"evidence", evidenceWindow}, {"writeblock", writeBlockMode}};
        for (const auto &[name, run] : alone) {
            if (only.contains(QLatin1String(name)))
                run();
        }
        out << (failures ? QStringLiteral("%1 step(s) failed").arg(failures) : QStringLiteral("All steps passed")) << Qt::endl;
        return failures ? 1 : 0;
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
            // Repair, Try the Repair First… or Not Now: Repair.
            if (QPushButton *repair = findButton(box, QStringLiteral("Repair")))
                repair->click();
            else
                box->close();
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
    luksHeaderThroughWindow();
    tryRepairFirst();
    clonezillaRestore();
    testBootWindow();
    wipeCertificate();
    evidenceWindow();
    inspector();
    recoverThroughWindow();
    scanThroughWindow();
    raidBanner();
    nasDrives();
    stickCheck();
    sdCardFormat();
    slowAreaRefresh();

    out << (failures ? QStringLiteral("%1 step(s) failed").arg(failures) : QStringLiteral("All steps passed")) << Qt::endl;
    return failures ? 1 : 0;
}
