// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Renders the dialogs to PNGs: QT_QPA_PLATFORM=offscreen diskforge-preview <dir>

#include "../src/lostfiles/preview.h"
#include "../src/app/about.h"
#include "../src/addons/addonform.h"
#include "../src/addons/addonmaker.h"
#include "../src/addons/addonoutput.h"
#include "../src/addons/addonprompt.h"
#include "../src/addons/addons.h"
#include "../src/addons/addonsdialog.h"
#include "../src/health/blockmapwidget.h"
#include "../src/backup/copydialogs.h"
#include "../src/backup/imagebackup.h"
#include "../src/drives/inspectdialog.h"
#include "../src/drives/partrecover.h"
#include "../src/drives/recoverdialog.h"
#include "../src/usb/stickcheckdialog.h"
#include "../src/app/homewindow.h"
#include "../src/usb/windowsusbdialog.h"
#include "../src/drives/powerbox.h"
#include "../src/app/mainwindow.h"
#include "../src/backup/rescuecopy.h"
#include "../src/tools/usagedialog.h"
#include "../src/tools/systemtools.h"
#include "../src/drives/erasedialog.h"
#include "../src/app/dialogs.h"
#include "../src/app/diskmap.h"
#include "../src/app/theme.h"
#include "../src/app/thememaker.h"
#include "../src/app/tools.h"
#include "../src/drives/typedialog.h"
#include "../src/app/translations.h"
#include "../src/drives/udisks.h"

#include <QApplication>
#include <QGuiApplication>
#include <QDir>
#include <QIcon>
#include <QCheckBox>
#include <QTabWidget>
#include <QLineEdit>
#include <QSpinBox>
#include <QTextStream>
#include <QFile>
#include <QProcess>
#include <QRadioButton>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QElapsedTimer>

#include <fcntl.h>
#include <QProgressBar>
#include <QLabel>

void previewTools(UDisks &udisks, const QDir &out);
void previewHome(UDisks &udisks, const QDir &out);
void previewCopyTools(UDisks &udisks, const QDir &out);

namespace {

void save(QWidget &widget, const QString &path)
{
    widget.show();
    QApplication::processEvents();
    widget.grab().save(path);
    QTextStream(stdout) << path << Qt::endl;
    widget.hide();
}

} // namespace

int main(int argc, char *argv[])
{
    // Find Lost Files starts this program as its preview helper, as it would DiskForge itself.
    if (argc == 2 && QByteArray(argv[1]) == "--preview-helper") {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QGuiApplication helper(argc, argv);
        return preview::runHelper();
    }
    QApplication app(argc, argv);
    // Without a desktop there's no icon theme; DISKFORGE_PREVIEW_ICONS=breeze-dark (say) picks one,
    // for screenshots that look like the app does on a desktop.
    if (qEnvironmentVariableIsSet("DISKFORGE_PREVIEW_ICONS")) {
        QIcon::setThemeSearchPaths(QIcon::themeSearchPaths() << QStringLiteral("/usr/share/icons") << QStringLiteral("/usr/local/share/icons"));
        QIcon::setThemeName(qEnvironmentVariable("DISKFORGE_PREVIEW_ICONS"));
    }
    installTranslations();
    QApplication::setApplicationVersion(QStringLiteral(APP_VERSION));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/data/" APP_ID ".svg")));
    const QDir out(app.arguments().value(1, QStringLiteral(".")));
    UDisks udisks;

    AboutDialog about(udisks.daemonVersion());
    save(about, out.filePath(QStringLiteral("about.png")));
    if (auto *tabs = about.findChild<QTabWidget *>()) {
        tabs->setCurrentIndex(3);
        save(about, out.filePath(QStringLiteral("about-components.png")));
    }
    HelpWindow help;
    save(help, out.filePath(QStringLiteral("handbook.png")));
    Addons addons;
    addons.load();
    AddonsDialog addonsDialog(&addons);
    save(addonsDialog, out.filePath(QStringLiteral("addons.png")));
    ThemeMaker themeMaker(&addons);
    save(themeMaker, out.filePath(QStringLiteral("theme-maker.png")));
    previewHome(udisks, out);
    {
        // The main window (with the warning banner when a drive here has one), then in each
        // built-in theme.
        MainWindow window(&udisks);
        window.resize(1200, 760);
        save(window, out.filePath(QStringLiteral("main.png")));
        // A wipe running on the second drive (faked), with its Stop bar on top.
        if (udisks.disks().size() > 1) {
            const Disk &d = udisks.disks()[1];
            Job wipe;
            wipe.path = QStringLiteral("/preview/jobs/wipe");
            wipe.operation = QStringLiteral("format-erase");
            wipe.cancelable = true;
            wipe.objects = {d.blockPath};
            wipe.progress = 0.34;
            wipe.progressValid = true;
            wipe.rate = 49 * 1000 * 1000;
            wipe.bytes = d.size;
            udisks.setJobsForTest({wipe});
            udisks.refresh();
            save(window, out.filePath(QStringLiteral("main-stop.png")));
            udisks.setJobsForTest({});
            udisks.refresh();
        }
        {
            // The inspector, on an image with a damaged main header (the backup is still good).
            QTemporaryDir tmp;
            const QString image = tmp.filePath(QStringLiteral("inspect.img"));
            QFile f(image);
            if (f.open(QIODevice::WriteOnly) && f.resize(64 * 1024 * 1024)) {
                f.close();
                QProcess sfdisk;
                sfdisk.start(QStringLiteral("sfdisk"), {QStringLiteral("--quiet"), image});
                sfdisk.waitForStarted();
                sfdisk.write("label: gpt\nsize=16MiB, type=uefi, name=\"EFI\"\nsize=32MiB, type=linux, name=\"root\"\ntype=swap\n");
                sfdisk.closeWriteChannel();
                sfdisk.waitForFinished();
                if (f.open(QIODevice::ReadWrite) && f.seek(512 + 16)) {
                    f.write("\x01", 1); // the header's checksum no longer fits
                    f.close();
                }
                TableInspectorDialog inspector(::open(QFile::encodeName(image).constData(), O_RDONLY | O_CLOEXEC), QStringLiteral("inspect.img"));
                save(inspector, out.filePath(QStringLiteral("inspector.png")));
            }
        }
        for (const Disk &d : udisks.disks()) {
            if (PowerBox::applies(d)) {
                PowerBox power(&udisks, d);
                power.resize(600, power.sizeHint().height());
                save(power, out.filePath(QStringLiteral("power.png")));
                break;
            }
        }
        for (const Disk &d : udisks.disks()) {
            if (!d.isSystem && !d.volumes.isEmpty() && d.tableType == QLatin1String("gpt")) {
                PartitionTypeDialog typeDialog(d, d.volumes.first());
                save(typeDialog, out.filePath(QStringLiteral("type-flags.png")));
                break;
            }
        }
        {
            // Locks on encrypted partitions, on a made-up drive: one locked, one unlocked.
            Disk fake;
            fake.device = QStringLiteral("/dev/sdx");
            fake.blockPath = QStringLiteral("/preview/sdx");
            fake.model = QStringLiteral("Example Drive");
            fake.size = 100ULL * 1000 * 1000 * 1000;
            fake.tableType = QStringLiteral("gpt");
            Volume locked;
            locked.objectPath = QStringLiteral("/preview/sdx1");
            locked.device = QStringLiteral("/dev/sdx1");
            locked.offset = 1024 * 1024;
            locked.size = 40ULL * 1000 * 1000 * 1000;
            locked.fsType = QStringLiteral("crypto_LUKS");
            locked.encrypted = true;
            Volume open = locked;
            open.objectPath = QStringLiteral("/preview/sdx2");
            open.device = QStringLiteral("/dev/sdx2");
            open.offset = locked.offset + locked.size;
            open.size = 50ULL * 1000 * 1000 * 1000;
            open.cleartextPath = QStringLiteral("/preview/dm-0");
            open.cleartextDevice = QStringLiteral("/dev/dm-0");
            open.cleartextFsType = QStringLiteral("ext4");
            open.cleartextHasFilesystem = true;
            open.label = QStringLiteral("Home");
            fake.volumes = {locked, open};
            DiskMap map;
            map.setDisks({fake});
            map.resize(900, map.sizeHint().height());
            save(map, out.filePath(QStringLiteral("map-locks.png")));

            // An LVM volume group and a RAID array, as rows of their own.
            Disk group;
            group.isLvm = true;
            group.device = QStringLiteral("/dev/vault");
            group.blockPath = QStringLiteral("/preview/vault");
            group.model = QStringLiteral("LVM volume group vault");
            group.size = 120ULL * 1000 * 1000 * 1000;
            Volume lvData;
            lvData.objectPath = QStringLiteral("/preview/vault/data");
            lvData.device = QStringLiteral("/dev/vault/data");
            lvData.isLv = true;
            lvData.lvActive = true;
            lvData.partName = QStringLiteral("data");
            lvData.number = 1;
            lvData.size = 60ULL * 1000 * 1000 * 1000;
            lvData.fsType = QStringLiteral("ext4");
            lvData.hasFilesystem = true;
            Volume lvOld = lvData;
            lvOld.objectPath = QStringLiteral("/preview/vault/old");
            lvOld.device = QStringLiteral("/dev/vault/old");
            lvOld.lvActive = false;
            lvOld.hasFilesystem = false;
            lvOld.fsType.clear();
            lvOld.partName = QStringLiteral("old");
            lvOld.number = 2;
            lvOld.offset = lvData.size;
            lvOld.size = 20ULL * 1000 * 1000 * 1000;
            group.volumes = {lvData, lvOld};
            Disk array;
            array.isRaid = true;
            array.device = QStringLiteral("/dev/md/mirror");
            array.blockPath = QStringLiteral("/preview/mirror");
            array.model = QStringLiteral("RAID 1 array mirror");
            array.raidLevel = QStringLiteral("raid1");
            array.raidDevices = 2;
            array.raidSync = QStringLiteral("check");
            array.raidSyncDone = 0.12;
            array.size = 500ULL * 1000 * 1000 * 1000;
            array.health.state = Health::State::Healthy;
            DiskMap storage;
            storage.setDisks({group, array});
            storage.resize(900, storage.sizeHint().height());
            save(storage, out.filePath(QStringLiteral("map-lvm-raid.png")));

            // Recover Partitions on the same made-up drive, after its second partition was
            // deleted: the layout from before is offered.
            fake.health.key = QStringLiteral("Example-Drive-Preview");
            for (Volume &v : fake.volumes) {
                v.encrypted = false;
                v.cleartextPath.clear();
                v.number = int(&v - fake.volumes.data()) + 1;
                v.partType = QStringLiteral("0fc63daf-8483-4772-8e79-3d69d8477de4");
                v.fsType = QStringLiteral("ext4");
            }
            fake.volumes[0].label = QStringLiteral("System");
            recover::remember(fake.health.key, recover::fromDisk(fake));
            fake.volumes.removeLast();
            RecoverDialog recoverDialog(&udisks, fake, gpt::Report());
            save(recoverDialog, out.filePath(QStringLiteral("recover.png")));
        }
        for (const char *id : {"classic", "deadshadow", "diskforge-live", "high-contrast"}) {
            Theme::instance().use(QLatin1String(id), addons);
            save(window, out.filePath(QStringLiteral("main-%1.png").arg(QLatin1String(id))));
        }
        Theme::instance().use(QStringLiteral("system"), addons);
    }
    {
        // The Add-on Maker, editing the backup example (its form fields and all).
        Addon backupExample = Addons::parse(QStringLiteral(SOURCE_DIR "/examples/addons/backup-rsync/addon.json"));
        AddonMaker maker(&addons, &backupExample, [](const Addon &, const AddonAction &) {});
        save(maker, out.filePath(QStringLiteral("addon-maker.png")));
        if (auto *tabs = maker.findChild<QTabWidget *>()) {
            tabs->setCurrentIndex(1);
            save(maker, out.filePath(QStringLiteral("addon-maker-where.png")));
        }
    }

    // The add-on questions open their own dialogs: grab each one while it's open.
    auto grabDialog = [](const QString &path) {
        QTimer::singleShot(100, [path] {
            if (QWidget *w = QApplication::activeModalWidget()) {
                w->grab().save(path);
                QTextStream(stdout) << path << Qt::endl;
                w->close();
            }
        });
    };
    const QString examples = QStringLiteral(SOURCE_DIR "/examples/addons/");
    const Addon smart = Addons::parse(examples + QStringLiteral("smart-report/addon.json"));
    const Addon sizes = Addons::parse(examples + QStringLiteral("folder-sizes/addon.json"));
    Addon backup = Addons::parse(examples + QStringLiteral("backup-rsync/addon.json"));
    backup.outside = true;
    grabDialog(out.filePath(QStringLiteral("addon-install.png")));
    askInstallAddon(nullptr, smart);
    grabDialog(out.filePath(QStringLiteral("addon-run-admin.png")));
    askRunAddon(nullptr, smart, smart.actions.value(0), {QStringLiteral("sudo"), QStringLiteral("smartctl"), QStringLiteral("-x"), QStringLiteral("/dev/sdb")}, nullptr);
    grabDialog(out.filePath(QStringLiteral("addon-run-outside.png")));
    askRunAddon(nullptr, backup, backup.actions.value(0),
                {QStringLiteral("rsync"), QStringLiteral("-a"), QStringLiteral("--info=progress2"), QStringLiteral("--mkpath"),
                 QStringLiteral("/run/media/me/My Stuff/"), QDir::homePath() + QStringLiteral("/Backups/My Stuff/")}, nullptr);
    {
        const Addon full = Addons::parseData(R"json({"id":"backup","name":"Back Up","actions":[{"label":"Back Up to a Folder","output":"window",
            "command":["rsync","-a","{ask:check}","{mountpoint}/","{ask:dest}/"],
            "ask":[{"id":"dest","type":"folder","label":"Back up to","default":"{home}/Backups"},
                   {"id":"check","type":"check","label":"Compare file contents (slower)","on":"--checksum","off":""},
                   {"id":"mode","type":"choice","label":"When a file exists","choices":[{"label":"Replace it","value":"--inplace"},{"label":"Keep both","value":"--backup"}]},
                   {"id":"limit","type":"number","label":"Speed limit (MB/s, 0 = none)","min":0,"max":1000}]}]})json", QString());
        QMap<QString, QString> values;
        for (const AddonField &f : full.actions.value(0).ask)
            values.insert(f.id, QString(f.defaultValue).replace(QStringLiteral("{home}"), QDir::homePath()));
        AddonFormDialog form(QStringLiteral("Back Up to a Folder"), QStringLiteral("\"Back Up to a Folder\" from the add-on \"Back Up\" needs a few things first:"),
                             full.actions.value(0).ask, values, QStringLiteral("Continue"));
        save(form, out.filePath(QStringLiteral("addon-form.png")));
        AddonOutputWindow output(QStringLiteral("Show Folder Sizes"),
                                 {QStringLiteral("sh"), QStringLiteral("-c"), QStringLiteral("printf '4.0K\\t/run/media/me/STICK/notes\\n1.2G\\t/run/media/me/STICK/photos\\n\\033[1;31m3.4G\\033[0m\\t/run/media/me/STICK/videos\\n'; exit 0")});
        output.setAttribute(Qt::WA_DeleteOnClose, false);
        QElapsedTimer wait;
        wait.start();
        while (output.isRunning() && wait.elapsed() < 3000)
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(200);
        QApplication::processEvents();
        save(output, out.filePath(QStringLiteral("addon-output.png")));
    }
    grabDialog(out.filePath(QStringLiteral("addon-run-lookonly.png")));
    askRunAddon(nullptr, sizes, sizes.actions.value(0),
                {QStringLiteral("sh"), QStringLiteral("-c"), sizes.actions.value(0).command.value(2), QStringLiteral("sh"), QStringLiteral("/")}, nullptr);
    previewTools(udisks, out);

    // prefer Ventoy so the warning shows up
    const Disk *disk = nullptr;
    int number = -1;
    for (int i = 0; i < udisks.disks().size(); ++i) {
        const Disk &d = udisks.disks()[i];
        if (!d.isSystem && !d.volumes.isEmpty() && (!disk || d.isVentoy)) {
            disk = &d;
            number = i;
        }
    }
    if (!disk)
        return 1;

    FormatDialog format(*disk, disk->volumes.first(), udisks.filesystems());
    save(format, out.filePath(QStringLiteral("format.png")));

    for (const Disk &d : udisks.disks()) {
        if (d.isSystem)
            continue;
        for (const Span &s : diskSpans(d)) {
            if (s.isFree()) {
                NewPartitionDialog dialog(d, s, udisks.filesystems());
                save(dialog, out.filePath(QStringLiteral("new-partition.png")));
                goto table;
            }
        }
    }
table:
    PartitionTableDialog table(*disk, number);
    save(table, out.filePath(QStringLiteral("table.png")));
    if (auto *confirm = table.findChild<QLineEdit *>()) {
        confirm->setText(disk->device.section(QLatin1Char('/'), -1));
        save(table, out.filePath(QStringLiteral("table-confirmed.png")));
    }

    for (const Disk &d : udisks.disks()) {
        for (const Volume &v : d.volumes) {
            const ResizeLimits limits = udisks.resizeLimits(v);
            if (limits.possible) {
                ResizeDialog dialog(d, v, limits, udisks.resizeNeedsRemount(v, true), udisks.resizeNeedsRemount(v, false));
                save(dialog, out.filePath(QStringLiteral("resize.png")));
                if (auto *size = dialog.findChild<QSpinBox *>()) {
                    size->setValue(size->minimum() + (size->maximum() - size->minimum()) / 3);
                    save(dialog, out.filePath(QStringLiteral("resize-shrink.png")));
                }
                return 0;
            }
        }
    }
    QTextStream(stdout) << "no resizable volume to preview" << Qt::endl;
    return 0;
}

// Second pass: the tools dialogs. Called from main before the resize preview returns.
void previewTools(UDisks &udisks, const QDir &out)
{
    for (const Disk &d : udisks.disks()) {
        // fwupd answers in the background; wait for it (when it's running) before the picture.
        auto settle = [](QWidget &dialog) {
            QElapsedTimer waited;
            waited.start();
            auto asking = [&dialog] {
                for (QLabel *l : dialog.findChildren<QLabel *>()) {
                    if (l->text().contains(QLatin1String("Asking fwupd")))
                        return true;
                }
                return false;
            };
            do
                QApplication::processEvents(QEventLoop::AllEvents, 50);
            while (asking() && waited.elapsed() < 15000);
        };
        if (d.health.nvme) {
            HealthDialog nvme(&udisks, d.blockPath);
            settle(nvme);
            save(nvme, out.filePath(QStringLiteral("health-nvme.png")));
        }
        if (d.health.state == Health::State::Warning || d.health.state == Health::State::Failing) {
            HealthDialog health(&udisks, d.blockPath);
            settle(health);
            save(health, out.filePath(QStringLiteral("health.png")));
            BadSectorsDialog scan(&udisks, d);
            save(scan, out.filePath(QStringLiteral("badsectors.png")));
            // Same dialog partway through a scan: mostly fine, a few slow spots, one bad.
            if (auto *map = scan.findChild<BlockMapWidget *>()) {
                QVector<ReadSample> samples;
                const quint64 step = d.size / 1000;
                for (quint64 off = 0; off < d.size * 7 / 10; off += step)
                    samples.append({off, quint32(step), off % (step * 97) == 0 ? 400u : 12u, off / step != 311});
                map->addSamples(samples);
                save(scan, out.filePath(QStringLiteral("badsectors-map.png")));
            }
            break;
        }
    }
    for (const Disk &d : udisks.disks()) {
        if (d.isSystem || d.isLoop || d.volumes.isEmpty())
            continue;
        BenchmarkDialog bench(&udisks, d);
        save(bench, out.filePath(QStringLiteral("benchmark.png")));
        WipeDialog wipe(d, 2);
        save(wipe, out.filePath(QStringLiteral("wipe.png")));
        WriteImageDialog write(&udisks, QString());
        save(write, out.filePath(QStringLiteral("write-image.png")));
        // A compressed image, the way Raspberry Pi hands them out.
        QTemporaryDir packed;
        QFile raw(packed.filePath(QStringLiteral("raspios-lite-arm64.img")));
        if (raw.open(QIODevice::WriteOnly)) {
            QByteArray sector(512, '\0');
            sector[510] = char(0x55);
            sector[511] = char(0xAA);
            raw.write(sector + QByteArray(2 * 1024 * 1024, 'x'));
            raw.close();
            QProcess::execute(QStringLiteral("xz"), {QStringLiteral("-q"), raw.fileName()});
        }
        if (auto *image = write.findChild<QLineEdit *>(QStringLiteral("image")))
            image->setText(raw.fileName() + QStringLiteral(".xz"));
        if (auto *checksum = write.findChild<QLineEdit *>(QStringLiteral("checksum")))
            checksum->setText(QStringLiteral("sha256:0123abcd"));
        save(write, out.filePath(QStringLiteral("write-image-xz.png")));
        // A Linux ISO with its files copied and persistence, the way an Ubuntu one would be.
        const QString tree = packed.filePath(QStringLiteral("ubuntu"));
        for (const char *file : {"EFI/BOOT/BOOTX64.EFI", "casper/vmlinuz", "casper/filesystem.squashfs", "boot/grub/grub.cfg"}) {
            QDir().mkpath(QFileInfo(tree + QLatin1Char('/') + QLatin1String(file)).path());
            QFile f(tree + QLatin1Char('/') + QLatin1String(file));
            if (f.open(QIODevice::WriteOnly))
                f.write(QByteArray(64 * 1024, 'u'));
        }
        const QString iso = packed.filePath(QStringLiteral("ubuntu-24.04.3-desktop-amd64.iso"));
        QProcess::execute(QStringLiteral("xorriso"), {QStringLiteral("-as"), QStringLiteral("mkisofs"), QStringLiteral("-quiet"), QStringLiteral("-J"), QStringLiteral("-R"),
                                                      QStringLiteral("-V"), QStringLiteral("Ubuntu 24.04.3 LTS amd64"), QStringLiteral("-o"), iso, tree});
        if (auto *image = write.findChild<QLineEdit *>(QStringLiteral("image")))
            image->setText(iso);
        if (auto *checksum = write.findChild<QLineEdit *>(QStringLiteral("checksum")))
            checksum->clear();
        if (auto *copy = write.findChild<QRadioButton *>(QStringLiteral("copyMode")))
            copy->click();
        if (auto *persist = write.findChild<QCheckBox *>(QStringLiteral("persist")))
            persist->setChecked(true);
        save(write, out.filePath(QStringLiteral("write-image-copy.png")));
        StickCheckDialog stickCheck(&udisks, QString());
        save(stickCheck, out.filePath(QStringLiteral("check-stick.png")));
        stickcheck::Result fake;
        fake.completed = true;
        fake.fake = true;
        fake.wraps = true;
        fake.claimed = 64ULL * 1000 * 1000 * 1000;
        fake.real = 7ULL * 1024 * 1024 * 1024 + 512ULL * 1024 * 1024;
        stickCheck.showResultForPreview(fake);
        save(stickCheck, out.filePath(QStringLiteral("check-stick-fake.png")));
        FormatDialog format(d, d.volumes.first(), udisks.filesystems());
        if (auto *box = format.findChild<QCheckBox *>())
            box->setChecked(true);
        save(format, out.filePath(QStringLiteral("format-encrypted.png")));
        break;
    }
    previewCopyTools(udisks, out);

    // Make a Windows USB, with Microsoft's ISO when there's one in Downloads (UDisks mounts it
    // without a password; it's closed again afterwards).
    {
        WindowsUsbDialog windows(&udisks, QString());
        windows.show();
        auto *isoInfo = windows.findChild<QLabel *>(QStringLiteral("isoInfo"));
        QElapsedTimer waited;
        waited.start();
        while (!WindowsUsbDialog::findIso().isEmpty() && isoInfo && !isoInfo->text().contains(QLatin1String("build")) && waited.elapsed() < 30000)
            QApplication::processEvents(QEventLoop::AllEvents, 100);
        save(windows, out.filePath(QStringLiteral("windows-usb.png")));
    }
    // The dialog closes the ISO when it goes; give that a moment before quitting.
    QElapsedTimer closing;
    closing.start();
    while (closing.elapsed() < 3000)
        QApplication::processEvents(QEventLoop::AllEvents, 100);

    // Secure erase: a frozen ATA drive and an NVMe drive (the system one, so Erase stays off).
    for (const Disk &d : udisks.disks()) {
        if (d.ataEraseMinutes > 0 || d.ataEnhancedEraseMinutes > 0) {
            SecureEraseDialog erase(&udisks, d.blockPath, 1);
            save(erase, out.filePath(QStringLiteral("secure-erase-ata.png")));
            // Unplug and Replug's steps, shown without touching the drive.
            for (int step : {1, 2, 3}) {
                SecureEraseDialog replug(&udisks, d.blockPath, 1);
                replug.showReplugStepForPreview(step);
                save(replug, out.filePath(QStringLiteral("secure-erase-replug-%1.png").arg(step)));
            }
        } else if (d.nvmeNamespace) {
            SecureEraseDialog erase(&udisks, d.blockPath, 0);
            save(erase, out.filePath(QStringLiteral("secure-erase-nvme.png")));
        }
    }

    // Disk usage of this source tree (small and always there), once the scan is done.
    UsageDialog usage(QStringLiteral(SOURCE_DIR), QStringLiteral("diskforge"));
    usage.show();
    if (auto *busy = usage.findChild<QProgressBar *>()) {
        QElapsedTimer waited;
        waited.start();
        while (busy->isVisible() && waited.elapsed() < 30000)
            QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    save(usage, out.filePath(QStringLiteral("usage.png")));

    OptimizeDialog optimize(&udisks);
    save(optimize, out.filePath(QStringLiteral("optimize.png")));
    SnapshotsDialog snapshots;
    save(snapshots, out.filePath(QStringLiteral("snapshots.png")));
    // Cleanup works out its sizes in the background first.
    CleanupDialog clean(&udisks);
    clean.show();
    QElapsedTimer waited;
    waited.start();
    auto measuring = [&clean] {
        for (QLabel *l : clean.findChildren<QLabel *>()) {
            if (l->text() == QStringLiteral("…"))
                return true;
        }
        return false;
    };
    while (measuring() && waited.elapsed() < 60000)
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    save(clean, out.filePath(QStringLiteral("cleanup.png")));
}

// Clone, Back Up, Restore and Rescue Copy, with sample files in a temporary folder.
void previewCopyTools(UDisks &udisks, const QDir &out)
{
    const Disk *source = nullptr;
    for (const Disk &d : udisks.disks()) {
        if (!d.isSystem && !d.isLoop && !d.volumes.isEmpty() && (!source || d.rotationRate > 0))
            source = &d;
    }
    if (!source)
        return;
    const Disk disk = *source;
    const Volume volume = disk.volumes.first();
    QTemporaryDir tmp;

    CloneDialog clone(&udisks, disk.blockPath);
    save(clone, out.filePath(QStringLiteral("clone.png")));

    BackupDialog backup(&udisks, volume.objectPath);
    if (auto *file = backup.findChild<QLineEdit *>())
        file->setText(tmp.filePath(QStringLiteral("SATA500-2026-10-08.img.zst")));
    save(backup, out.filePath(QStringLiteral("backup.png")));

    // A description file is all Restore reads until it starts.
    const QString image = tmp.filePath(QStringLiteral("SATA500-2026-10-08.img.zst"));
    BackupInfo info = imagebackup::infoFor(disk, &volume);
    info.size = volume.size / 2;
    info.sha256 = QString(64, QLatin1Char('a'));
    QFile json(imagebackup::infoPath(image));
    if (json.open(QIODevice::WriteOnly))
        json.write(QJsonDocument(info.toJson()).toJson());
    json.close();
    {
        QFile empty(image);
        if (!empty.open(QIODevice::WriteOnly))
            return;
    }
    RestoreDialog restore(&udisks, volume.objectPath);
    if (auto *file = restore.findChild<QLineEdit *>())
        file->setText(image);
    save(restore, out.filePath(QStringLiteral("restore.png")));

    // A rescue that was stopped partway: the first third done, a few bad spots, some to retry.
    RescueMap map(disk.size);
    const quint64 sector = 512;
    map.set(0, disk.size / 3 / sector * sector, RescueMap::Finished);
    for (int i = 1; i < 6; ++i) {
        const quint64 at = disk.size / 3 / 6 * i / sector * sector;
        map.set(at, 64 * 1024 * 1024, i % 2 ? RescueMap::NonTrimmed : RescueMap::Bad);
    }
    const QString rescued = tmp.filePath(QStringLiteral("HGST.img"));
    QString error;
    map.save(rescued + QStringLiteral(".map"), &error);
    {
        QFile empty(rescued);
        if (!empty.open(QIODevice::WriteOnly))
            return;
    }
    RescueDialog rescue(&udisks, disk.blockPath);
    if (auto *file = rescue.findChild<QLineEdit *>())
        file->setText(rescued);
    save(rescue, out.filePath(QStringLiteral("rescue.png")));
}

// DiskForge Live's home screen, with DiskForge Live's own icons (from rescue/icons, the logo added the way
// make-image.sh does it) and its programs, then the same tiles as the Quick Fixes window.
void previewHome(UDisks &udisks, const QDir &out)
{
    QTemporaryDir dir;
    const QString theme = dir.filePath(QStringLiteral("icons/DiskForge-Live"));
    QDir().mkpath(dir.filePath(QStringLiteral("icons")));
    QProcess::execute(QStringLiteral("cp"), {QStringLiteral("-a"), QStringLiteral(SOURCE_DIR "/rescue/icons/DiskForge-Live"), dir.filePath(QStringLiteral("icons"))});
    QFile::copy(QStringLiteral(SOURCE_DIR "/rescue/art/diskforge-live-icon.svg"), theme + QStringLiteral("/scalable/apps/diskforge-live.svg"));
    const QStringList searchPaths = QIcon::themeSearchPaths();
    const QString themeName = QIcon::themeName();
    QIcon::setThemeSearchPaths(QStringList{dir.filePath(QStringLiteral("icons"))} + searchPaths);
    QIcon::setThemeName(QStringLiteral("DiskForge-Live"));

    // The stick's launchers, plus stand-ins for the programs Debian brings.
    const QString apps = dir.filePath(QStringLiteral("applications"));
    QDir().mkpath(apps);
    for (const QString &f : QDir(QStringLiteral(SOURCE_DIR "/rescue/files/usr/share/applications")).entryList({QStringLiteral("*.desktop")}))
        QFile::copy(QStringLiteral(SOURCE_DIR "/rescue/files/usr/share/applications/") + f, apps + QLatin1Char('/') + f);
    for (const char *program : {"pcmanfm-qt", "firefox-esr", "qterminal", "qps", "featherpad"}) {
        QFile f(apps + QLatin1Char('/') + QLatin1String(program) + QStringLiteral(".desktop"));
        if (f.open(QIODevice::WriteOnly))
            f.write(QByteArray("[Desktop Entry]\nType=Application\nName=x\nExec=") + program + "\n");
    }
    HomeWindow::applicationDirs = {apps};
    {
        HomeWindow home(&udisks, HomeWindow::Mode::Desktop);
        home.resize(1280, 800);
        save(home, out.filePath(QStringLiteral("home.png")));
    }
    {
        HomeWindow fixes(&udisks, HomeWindow::Mode::Window);
        save(fixes, out.filePath(QStringLiteral("quick-fixes.png")));
    }
    HomeWindow::applicationDirs.clear();
    QIcon::setThemeSearchPaths(searchPaths);
    QIcon::setThemeName(themeName);
}
