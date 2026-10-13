// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mainwindow.h"

#include "homewindow.h"
#include "lostfilesdialog.h"

#include "about.h"
#include "addonform.h"
#include "addonoutput.h"
#include "addonprompt.h"
#include "addonsdialog.h"
#include "bootentriesdialog.h"
#include "catalogdialog.h"
#include "certificates.h"
#include "closedsets.h"
#include "writeblock.h"
#include "remotehelpdialog.h"
#include "netbootdialog.h"
#include "liveonpcdialog.h"
#include "clonezilladialog.h"
#include "copydialogs.h"
#include "erasedialog.h"
#include "evidencedialog.h"
#include "snapper.h"
#include "systemtools.h"
#include "usagedialog.h"
#include "applog.h"
#include "blockio.h"
#include "drivepower.h"
#include "luksheader.h"
#include "newids.h"
#include "newidsdialog.h"
#include "virusdialog.h"
#include "dialogs.h"
#include "tools.h"
#include "typedialog.h"
#include "diskmap.h"
#include "format.h"
#include "health.h"
#include "hostprocess.h"
#include "inspectdialog.h"
#include "recoverdialog.h"
#include "rescuestick.h"
#include "savefilesdialog.h"
#include "whathappened.h"
#include "rescueusbdialog.h"
#include "stickcheckdialog.h"
#include "windowsusbdialog.h"
#include "jobui.h"
#include "linuxbootdialog.h"
#include "noticebar.h"
#include "powerbox.h"
#include "productkeydialog.h"
#include "windowspicker.h"
#include "activitydialog.h"
#include "lookcloserdialog.h"
#include "driversdialog.h"
#include "theme.h"
#include "testbootdialog.h"
#include "thememaker.h"
#include "tryrepairdialog.h"
#include "winredialog.h"
#include "updates.h"
#include "usbinfo.h"
#include "udisks.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QCloseEvent>
#include <QFileDialog>
#include <QHeaderView>
#include <QProgressBar>
#include <QSignalBlocker>
#include <QKeySequence>
#include <QShortcut>
#include <QInputDialog>
#include <QLineEdit>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>
#include <QTreeWidget>
#include <QUrl>
#include <QSet>
#include <QDateTime>
#include <QTimer>
#include <QSaveFile>
#include <QSettings>
#include <QVBoxLayout>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

namespace {

enum Column { ColVolume, ColDevice, ColFileSystem, ColStatus, ColCapacity, ColFree, ColPercentFree, ColumnCount };

QIcon themeIcon(const char *name, const char *fallback)
{
    return QIcon::fromTheme(QLatin1String(name), QIcon::fromTheme(QLatin1String(fallback)));
}

QWidget *legendItem(const QPixmap &swatch, const QString &text, QLabel **swatchLabel = nullptr)
{
    auto *item = new QWidget;
    auto *layout = new QHBoxLayout(item);
    layout->setContentsMargins(6, 0, 6, 0);
    layout->setSpacing(4);
    auto *icon = new QLabel;
    icon->setPixmap(swatch);
    if (swatchLabel)
        *swatchLabel = icon;
    layout->addWidget(icon);
    layout->addWidget(new QLabel(text));
    return item;
}

QPixmap colorSwatch(const QColor &color)
{
    QPixmap pixmap(12, 12);
    pixmap.fill(color);
    return pixmap;
}

} // namespace

MainWindow::MainWindow(UDisks *udisks, QWidget *parent)
    : QMainWindow(parent)
    , m_udisks(udisks)
    , m_table(new QTreeWidget)
    , m_map(new DiskMap)
{
    setWindowTitle(tr("DiskAnvil"));

    m_table->setColumnCount(ColumnCount);
    m_table->setHeaderLabels({tr("Volume"), tr("Device"), tr("File System"), tr("Status"),
                              tr("Capacity"), tr("Free Space"), tr("% Free")});
    m_table->setRootIsDecorated(false);
    m_table->setUniformRowHeights(true);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->header()->setStretchLastSection(false);
    m_table->header()->setSectionResizeMode(ColStatus, QHeaderView::Stretch);

    auto *scroll = new QScrollArea;
    scroll->setWidget(m_map);
    scroll->setWidgetResizable(true);

    auto *splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(m_table);
    splitter->addWidget(scroll);
    splitter->setChildrenCollapsible(false);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({300, 500});

    m_notices = new QWidget;
    m_notices->setObjectName(QStringLiteral("notices"));
    auto *noticeLayout = new QVBoxLayout(m_notices);
    noticeLayout->setContentsMargins(4, 4, 4, 0);
    noticeLayout->setSpacing(4);
    m_notices->setVisible(false);
    auto *central = new QWidget;
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(m_notices);
    centralLayout->addWidget(splitter, 1);
    setCentralWidget(central);

    m_addons.load();
    Theme::instance().restore(m_addons);
    createActions();

    m_progress = new QProgressBar;
    m_progress->setMaximumWidth(180);
    m_progress->setTextVisible(false);
    m_progress->setVisible(false);
    statusBar()->addPermanentWidget(m_progress);
    QLabel *freeSwatch = nullptr, *partitionSwatch = nullptr;
    statusBar()->addPermanentWidget(legendItem({}, tr("Unallocated"), &freeSwatch));
    statusBar()->addPermanentWidget(legendItem({}, tr("Partition"), &partitionSwatch));
    auto paintLegend = [freeSwatch, partitionSwatch] {
        freeSwatch->setPixmap(colorSwatch(Theme::instance().color(Theme::Role::Free)));
        partitionSwatch->setPixmap(colorSwatch(Theme::instance().color(Theme::Role::Partition)));
    };
    paintLegend();
    connect(&Theme::instance(), &Theme::changed, this, paintLegend);
    statusBar()->addPermanentWidget(legendItem(QIcon::fromTheme(QStringLiteral("object-locked")).pixmap(12, 12),
                                               tr("System disk (read-only)")));

    connect(m_udisks, &UDisks::changed, this, &MainWindow::rebuild);
    connect(m_udisks, &UDisks::operationFinished, this, [this](bool ok, const QString &message) {
        updateActions();
        if (ok) {
            statusBar()->showMessage(message, 8000);
        } else if (m_udisks->wasStopped()) {
            statusBar()->showMessage(message, 15000); // stopped on purpose: not an error
        } else {
            statusBar()->clearMessage();
            QMessageBox::warning(this, windowTitle(), message);
        }
    });
    // A finished erasure is kept, for its certificate, and a bar offers one now.
    connect(m_udisks, &UDisks::erased, this, [this](const erasure::Record &record) {
        erasure::remember(record);
        const erasure::Record kept = erasure::all().value(0);
        for (NoticeBar *old : m_notices->findChildren<NoticeBar *>(QStringLiteral("erased"), Qt::FindDirectChildrenOnly)) {
            old->hide();
            old->deleteLater();
        }
        auto *bar = new NoticeBar(NoticeBar::Level::Info, tr("%1 (%2) has been erased. A certificate of erasure can be saved for it, now or "
                                                             "later (Tools, Erasure Certificates).")
                                                              .arg(kept.model, kept.serial.isEmpty() ? kept.device : kept.serial));
        bar->setObjectName(QStringLiteral("erased"));
        QPushButton *make = bar->addButton(tr("Make a Certificate…"));
        connect(make, &QPushButton::clicked, this, [this, kept, bar] {
            QTimer::singleShot(0, this, [this, kept, bar] {
                if (CertificateDialog(m_udisks, kept, this).exec() == QDialog::Accepted) {
                    bar->hide();
                    bar->deleteLater();
                }
            });
        });
        QPushButton *dismiss = bar->addButton(tr("Dismiss"));
        connect(dismiss, &QPushButton::clicked, bar, [bar] {
            bar->hide();
            bar->deleteLater();
        });
        m_notices->layout()->addWidget(bar);
        bar->show();
        showNoticesIfAny();
    });
    connect(m_udisks, &UDisks::jobStopFailed, this, [this](const QString &message) {
        QMessageBox::warning(this, windowTitle(), message);
    });
    connect(m_udisks, &UDisks::checkFinished, this, [this](const QString &objectPath, bool clean) {
        const Volume *v = volumeByPath(objectPath);
        if (clean || !v)
            return;
        // On DiskAnvil Live the repair can be tried on a stand-in first, to see what it does.
        const QString fs = v->effectiveFsType();
        QMessageBox ask(QMessageBox::Question, tr("Errors Found"), tr("%1 has file system errors. Repair them now?").arg(volumeTitle(*v)),
                        QMessageBox::NoButton, this);
        QAbstractButton *repair = ask.addButton(tr("Repair"), QMessageBox::AcceptRole);
        QAbstractButton *tryFirst = TryRepairDialog::possible(fs) ? ask.addButton(tr("Try the Repair First…"), QMessageBox::ActionRole) : nullptr;
        ask.addButton(tr("Not Now"), QMessageBox::RejectRole);
        ask.exec();
        if (tryFirst && ask.clickedButton() == tryFirst) {
            TryRepairDialog trying(v->encrypted ? v->cleartextDevice : v->device, fs, volumeTitle(*v), this);
            if (trying.exec() != QDialog::Accepted)
                return;
        } else if (ask.clickedButton() != repair) {
            return;
        }
        if (const Volume *again = volumeByPath(objectPath); !again || !failingDriveOk(*again, true))
            return;
        if (const Volume *fresh = volumeByPath(objectPath))
            m_udisks->repair(*fresh);
        else
            gone();
    });
    connect(m_table, &QTreeWidget::itemSelectionChanged, this, &MainWindow::onTableSelection);
    connect(m_table, &QTreeWidget::itemDoubleClicked, this, &MainWindow::activate);
    connect(m_table, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (m_table->itemAt(pos))
            showContextMenu(m_table->viewport()->mapToGlobal(pos));
    });
    connect(m_map, &DiskMap::selectionChanged, this, [this] {
        syncTableToMap();
        updateActions();
    });
    connect(m_map, &DiskMap::contextMenuRequested, this, &MainWindow::showContextMenu);
    connect(m_map, &DiskMap::activated, this, &MainWindow::activate);
    connect(m_map, &DiskMap::lockClicked, this, [this](const QString &path) {
        // Queued: Unlock asks for the passphrase, and the click is still being handled.
        QTimer::singleShot(0, this, [this, path] {
            const Volume *v = volumeByPath(path);
            if (!v || !v->encrypted)
                return;
            QAction *action = v->cleartextPath.isEmpty() ? m_unlock : m_lock;
            if (action->isEnabled())
                action->trigger();
            else
                statusBar()->showMessage(action == m_lock ? tr("%1 can't be locked right now.").arg(shortDevice(v->device))
                                                          : tr("%1 can't be unlocked right now.").arg(shortDevice(v->device)), 8000);
        });
    });

    rebuild();
    refreshAddons();
    statusBar()->showMessage(tr("Tip: right-click any drive or partition to see what you can do with it"), 20000);
}

void MainWindow::createActions()
{
    m_refresh = new QAction(themeIcon("view-refresh", "reload"), tr("&Refresh"), this);
    m_refresh->setShortcut(QKeySequence::Refresh);
    connect(m_refresh, &QAction::triggered, m_udisks, &UDisks::refresh);

    m_open = new QAction(themeIcon("document-open-folder", "folder-open"), tr("&Open in File Manager"), this);
    connect(m_open, &QAction::triggered, this, &MainWindow::openInFileManager);

    m_mount = new QAction(themeIcon("media-mount", "drive-harddisk"), tr("&Mount"), this);
    connect(m_mount, &QAction::triggered, this, [this] {
        if (const Volume *v = selectedVolume()) {
            statusBar()->showMessage(tr("Mounting %1…").arg(shortDevice(v->device)));
            m_udisks->mount(*v);
        }
    });

    m_unmount = new QAction(themeIcon("media-eject", "media-eject"), tr("&Unmount"), this);
    connect(m_unmount, &QAction::triggered, this, [this] {
        if (const Volume *v = selectedVolume()) {
            statusBar()->showMessage(tr("Unmounting %1…").arg(shortDevice(v->device)));
            m_udisks->unmount(*v);
        }
    });

    m_copy = new QAction(themeIcon("edit-copy", "edit-copy"), tr("&Copy Device Path"), this);
    connect(m_copy, &QAction::triggered, this, &MainWindow::copyDevicePath);

    m_properties = new QAction(themeIcon("document-properties", "configure"), tr("&Properties"), this);
    m_properties->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Return));
    connect(m_properties, &QAction::triggered, this, &MainWindow::showProperties);

    m_newPartition = new QAction(themeIcon("list-add", "document-new"), tr("&New Partition…"), this);
    connect(m_newPartition, &QAction::triggered, this, &MainWindow::newPartition);

    m_format = new QAction(themeIcon("tools-wizard", "edit-clear"), tr("&Format…"), this);
    connect(m_format, &QAction::triggered, this, &MainWindow::formatVolume);

    m_rename = new QAction(themeIcon("edit-rename", "document-edit"), tr("Change &Label…"), this);
    m_rename->setShortcut(QKeySequence(Qt::Key_F2));
    connect(m_rename, &QAction::triggered, this, &MainWindow::changeLabel);

    m_inspect = new QAction(themeIcon("document-preview", "edit-find"), tr("&Inspect Partition Table…"), this);
    connect(m_inspect, &QAction::triggered, this, [this] {
        if (const Disk *d = selectedDisk())
            TableInspectorDialog(m_udisks, *d, this).exec();
    });

    m_raidCheck = new QAction(themeIcon("tools-check-spelling", "edit-find"), tr("Check RAID &Array"), this);
    m_raidCheck->setToolTip(tr("Reads the whole array and compares its copies. It can be stopped any time."));
    connect(m_raidCheck, &QAction::triggered, this, [this] {
        if (const Disk *d = selectedDisk(); d && d->isRaid)
            m_udisks->raidSyncAction(*d, QStringLiteral("check"));
    });

    // Find Lost Files: from the selected drive or partition, or picked in the window.
    m_findFiles = new QAction(themeIcon("diskanvil-live-recover", "edit-find"), tr("Find &Lost Files…"), this);
    connect(m_findFiles, &QAction::triggered, this, [this] {
        const Volume *v = selectedVolume();
        const Disk *d = selectedDisk();
        LostFilesDialog(m_udisks, v ? v->objectPath : d ? d->blockPath : QString(), toolParent()).exec();
    });
    m_findFilesImage = new QAction(themeIcon("diskanvil-live-recover", "edit-find"), tr("Find Lost &Files in a Disk Image…"), this);
    connect(m_findFilesImage, &QAction::triggered, this, [this] {
        const QString file = QFileDialog::getOpenFileName(
            this, tr("Find Lost Files in a Disk Image"), QDir::homePath(),
            tr("Disk images (*.img *.iso *.raw *.dd *.bin *.vdi *.vmdk *.vhd *.vhdx *.qcow2);;Virtual machines' disks (*.vdi *.vmdk *.vhd *.vhdx "
               "*.qcow2);;All files (*)"));
        if (file.isEmpty())
            return;
        LostFilesDialog dialog(m_udisks, QString(), this);
        dialog.scanImage(file);
        dialog.exec();
    });
    m_recover = new QAction(themeIcon("edit-undo", "document-revert"), tr("Re&cover Partitions…"), this);
    connect(m_recover, &QAction::triggered, this, [this] {
        if (const Disk *d = selectedDisk())
            RecoverDialog(m_udisks, *d, this).exec();
    });

    m_typeFlags = new QAction(themeIcon("document-properties", "document-properties"), tr("Partition &Type and Flags…"), this);
    connect(m_typeFlags, &QAction::triggered, this, [this] {
        const Disk *d = selectedDisk();
        const Volume *v = selectedVolume();
        if (!d || !v)
            return;
        const QString path = v->objectPath;
        PartitionTypeDialog dialog(*d, *v, this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        if (const Volume *fresh = volumeByPath(path)) // the list may have been rebuilt meanwhile
            m_udisks->setPartitionTypeAndFlags(*fresh, dialog.typeChanged() ? dialog.type() : QString(), dialog.flagsChanged(), dialog.flags());
    });

    m_resize = new QAction(themeIcon("transform-scale", "zoom-fit-best"), tr("&Resize…"), this);
    connect(m_resize, &QAction::triggered, this, &MainWindow::resizeVolume);

    m_delete = new QAction(themeIcon("edit-delete", "list-remove"), tr("&Delete Partition…"), this);
    m_delete->setShortcut(QKeySequence::Delete);
    connect(m_delete, &QAction::triggered, this, &MainWindow::deletePartition);

    m_newTable = new QAction(themeIcon("document-new", "list-add"), tr("New Partition &Table…"), this);
    connect(m_newTable, &QAction::triggered, this, &MainWindow::newPartitionTable);

    m_safelyRemove = new QAction(themeIcon("media-eject", "media-eject"), tr("&Safely Remove"), this);
    connect(m_safelyRemove, &QAction::triggered, this, [this] {
        if (const Disk *d = selectedDisk()) {
            statusBar()->showMessage(tr("Getting %1 ready to unplug…").arg(shortDevice(d->device)));
            m_udisks->powerOff(*d);
        }
    });

    m_check = new QAction(themeIcon("checkmark", "dialog-ok-apply"), tr("Chec&k for Errors…"), this);
    connect(m_check, &QAction::triggered, this, [this] {
        const Volume *v = selectedVolume();
        if (!v)
            return;
        const QString path = v->objectPath;
        if (!failingDriveOk(*v, false))
            return;
#ifndef Q_OS_WIN // Windows' chkdsk checks it where it is
        if (!v->mounts().isEmpty()
            && !askPlain(this, tr("Check for Errors"), tr("%1 has to be unmounted while it's checked. Continue?").arg(volumeTitle(*v))))
            return;
#endif
        const Volume *fresh = volumeByPath(path);
        if (!fresh)
            return gone();
        statusBar()->showMessage(tr("Checking %1…").arg(shortDevice(fresh->device)));
        m_udisks->check(*fresh);
    });

    m_startup = new QAction(tr("Mount at &Startup"), this);
    m_startup->setCheckable(true);
    connect(m_startup, &QAction::triggered, this, [this](bool on) {
        if (const Volume *v = selectedVolume())
            m_udisks->setMountAtStartup(*v, on);
        updateActions();
    });

    m_unlock = new QAction(themeIcon("object-unlocked", "unlock"), tr("U&nlock…"), this);
    connect(m_unlock, &QAction::triggered, this, [this] {
        const Volume *v = selectedVolume();
        if (!v)
            return;
        const QString path = v->objectPath;
        // What UDisks takes for VeraCrypt (or can't tell from it): VeraCrypt's own questions.
        if (v->fsType == QLatin1String("crypto_unknown") || v->fsType == QLatin1String("crypto_TCRYPT")) {
            VeraCryptDialog dialog(shortDevice(v->device), this);
            if (dialog.exec() != QDialog::Accepted)
                return;
            if (const Volume *fresh = volumeByPath(path))
                m_udisks->unlock(*fresh, dialog.password(), dialog.options());
            else
                gone();
            return;
        }
        bool ok = false;
        // The label comes from the drive: shown as plain text. BitLocker takes its password or its
        // recovery key, which people rarely know where to find.
        const QString prompt = v->fsType == QLatin1String("BitLocker")
                                   ? tr("%1 is locked with BitLocker. Its password, or its recovery key: 48 digits in eight groups, "
                                        "kept in the Microsoft account of whoever set up the PC (aka.ms/myrecoverykey), or on a "
                                        "printout or USB stick from back then. A company PC's IT department has it.")
                                         .arg(volumeTitle(*v))
                                   : tr("Passphrase for %1:").arg(volumeTitle(*v));
        // Its own dialog, so the explanation wraps instead of stretching it off the screen.
        QInputDialog ask(this);
        ask.setWindowTitle(tr("Unlock %1").arg(shortDevice(v->device)));
        ask.setLabelText(prompt);
        ask.setTextEchoMode(QLineEdit::Password);
        for (QLabel *label : ask.findChildren<QLabel *>()) {
            label->setWordWrap(true);
            label->setTextFormat(Qt::PlainText);
        }
        ask.resize(480, ask.sizeHint().height());
        ok = ask.exec() == QDialog::Accepted;
        const QString pass = ask.textValue();
        if (!ok || pass.isEmpty())
            return;
        if (const Volume *fresh = volumeByPath(path))
            m_udisks->unlock(*fresh, pass);
        else
            gone();
    });

    m_lock = new QAction(themeIcon("object-locked", "lock"), tr("&Lock"), this);
    connect(m_lock, &QAction::triggered, this, [this] {
        if (const Volume *v = selectedVolume())
            m_udisks->lock(*v);
    });

#ifdef Q_OS_WIN
    m_changePass = new QAction(themeIcon("document-encrypt", "dialog-password"), tr("Change BitLocker &Password…"), this);
#else
    m_changePass = new QAction(themeIcon("document-encrypt", "dialog-password"), tr("Change &Passphrase…"), this);
#endif
    connect(m_changePass, &QAction::triggered, this, [this] {
        const Volume *v = selectedVolume();
        if (!v)
            return;
        const QString path = v->objectPath;
        ChangePassphraseDialog dialog(shortDevice(v->device), this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        if (const Volume *fresh = volumeByPath(path))
            m_udisks->changePassphrase(*fresh, dialog.oldPassphrase(), dialog.newPassphrase());
        else
            gone();
    });

    m_luksBackup = new QAction(themeIcon("document-save", "document-save"), tr("Back Up Encryption &Header…"), this);
    m_luksBackup->setObjectName(QStringLiteral("luksBackup"));
    m_luksBackup->setToolTip(tr("A copy of the LUKS header, which holds the drive's keys: if the header is ever damaged, the copy "
                                "is the only way back in"));
    connect(m_luksBackup, &QAction::triggered, this, &MainWindow::backupLuksHeader);
    m_luksRestore = new QAction(themeIcon("document-revert", "edit-undo"), tr("Put Back Encryption Header…"), this);
    m_luksRestore->setObjectName(QStringLiteral("luksRestore"));
    connect(m_luksRestore, &QAction::triggered, this, &MainWindow::restoreLuksHeader);
#ifdef Q_OS_WIN
    m_luksBackup->setVisible(false); // LUKS is Linux's: DiskAnvil Live does it
    m_luksRestore->setVisible(false);
#endif

    m_openImage = new QAction(themeIcon("document-open", "document-open"), tr("&Open Disk Image…"), this);
    m_openImage->setShortcut(QKeySequence::Open);
    connect(m_openImage, &QAction::triggered, this, [this] {
#ifdef Q_OS_WIN
        // What Windows itself opens as a drive.
        const QString file = QFileDialog::getOpenFileName(this, tr("Open Disk Image"), QDir::homePath(),
                                                          tr("Virtual disks (*.vhd *.vhdx);;All files (*)"));
#else
        const QString file = QFileDialog::getOpenFileName(this, tr("Open Disk Image"), QDir::homePath(),
                                                          tr("Disk images (*.iso *.img *.raw);;All files (*)"));
#endif
        if (!file.isEmpty())
            m_udisks->openImage(file);
    });

    m_detachImage = new QAction(themeIcon("media-eject", "media-eject"), tr("&Close Disk Image"), this);
    connect(m_detachImage, &QAction::triggered, this, [this] {
        if (const Disk *d = selectedDisk())
            m_udisks->detachImage(*d);
    });

    m_writeImage = new QAction(themeIcon("media-flash", "document-save"), tr("&Write Image to USB…"), this);
    connect(m_writeImage, &QAction::triggered, this, [this] {
        const Disk *d = selectedDisk();
        WriteImageDialog(m_udisks, d ? d->blockPath : QString(), toolParent()).exec();
    });

    m_liveOnPc = new QAction(themeIcon("system-reboot", "drive-harddisk"), tr("Put DiskAnvil Live on This &PC…"), this);
    m_liveOnPc->setObjectName(QStringLiteral("liveOnPc"));
    m_liveOnPc->setToolTip(tr("DiskAnvil Live on a partition of this PC's drive, in its boot menu: no stick needed"));
    connect(m_liveOnPc, &QAction::triggered, this, [this] { LiveOnPcDialog(m_udisks, toolParent()).exec(); });
    m_rescueUsb = new QAction(themeIcon("tools-media-optical-burn", "media-flash"), tr("Make a DiskAnvil &Live USB…"), this);
    connect(m_rescueUsb, &QAction::triggered, this, [this] {
        const Disk *d = selectedDisk();
        RescueUsbDialog(m_udisks, d ? d->blockPath : QString(), toolParent()).exec();
    });

    m_windowsUsb = new QAction(themeIcon("windows", "media-flash"), tr("Make a W&indows USB…"), this);
    connect(m_windowsUsb, &QAction::triggered, this, [this] {
        const Disk *d = selectedDisk();
        WindowsUsbDialog(m_udisks, d ? d->blockPath : QString(), toolParent()).exec();
    });

    m_wipe = new QAction(themeIcon("edit-clear-all", "edit-clear"), tr("&Wipe Disk…"), this);
    connect(m_wipe, &QAction::triggered, this, [this] {
        const Disk *d = selectedDisk();
        if (!d)
            return;
        const QString path = d->blockPath;
        WipeDialog dialog(*d, selectedDiskNumber(), this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        const Disk *fresh = m_udisks->diskByPath(path);
        if (!fresh)
            return gone();
        statusBar()->showMessage(tr("Wiping %1…").arg(shortDevice(fresh->device)));
        m_udisks->wipe(*fresh);
    });

    m_sdCard = new QAction(themeIcon("media-flash-sd-mmc", "media-flash"), tr("Format as an SD &Card…"), this);
    m_sdCard->setObjectName(QStringLiteral("sdCard"));
    m_sdCard->setToolTip(tr("Formats a memory card the way the SD Card Association specifies, as cameras and phones expect"));
    connect(m_sdCard, &QAction::triggered, this, [this] {
        if (const Disk *d = selectedDisk())
            SdCardDialog(m_udisks, *d, selectedDiskNumber(), this).exec();
        updateActions();
    });

    m_secureErase = new QAction(themeIcon("edit-delete-shred", "edit-delete"), tr("Secure &Erase…"), this);
    connect(m_secureErase, &QAction::triggered, this, [this] {
        const Disk *d = selectedDisk();
        if (!d)
            return;
#ifdef Q_OS_WIN
        // Windows doesn't pass a drive's own erase command through, and PCs lock it while
        // Windows runs (DiskAnvil Live gets around that). Wipe Disk works here.
        QMessageBox box(QMessageBox::Information, tr("Secure Erase"),
                        tr("Secure Erase has the drive erase itself, spare areas and all, which only the drive can reach. "
                           "Windows doesn't let programs send it that command, and most PCs lock it while Windows runs.\n\n"
                           "DiskAnvil Live does it: start the PC from a DiskAnvil Live USB. Wipe Disk works here, writing zeros "
                           "over everything, which is as good for a hard drive."),
                        QMessageBox::Close, this);
        QPushButton *live = box.addButton(tr("Make a DiskAnvil Live USB…"), QMessageBox::ActionRole);
        QPushButton *wipe = box.addButton(tr("Wipe Disk…"), QMessageBox::ActionRole);
        wipe->setEnabled(m_wipe->isEnabled());
        box.exec();
        if (box.clickedButton() == live)
            m_rescueUsb->trigger();
        else if (box.clickedButton() == wipe)
            m_wipe->trigger();
        return;
#endif
        const QString path = d->blockPath;
        SecureEraseDialog dialog(m_udisks, path, selectedDiskNumber(), this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        // Unplug and Replug can bring the drive back under another name.
        const Disk *fresh = m_udisks->diskByPath(dialog.blockPath());
        if (!fresh)
            return gone();
        statusBar()->showMessage(tr("Erasing %1… the drive is doing it, so there's no progress until it's done.").arg(shortDevice(fresh->device)));
        // Old controllers can lose a drive during a long erase and not notice it's back. On
        // DiskAnvil Live, ask them to look again once it's done.
        if (rescue::canRescanDrives()) {
            auto conn = std::make_shared<QMetaObject::Connection>();
            *conn = connect(m_udisks, &UDisks::operationFinished, this, [this, conn](bool, const QString &) {
                disconnect(*conn);
                rescue::rescanDrives({QStringLiteral("scan")}, this);
            });
        }
        m_udisks->secureErase(*fresh, dialog.method(), dialog.sectorSize());
    });

    m_health = new QAction(themeIcon("dialog-information", "help-about"), tr("Disk &Health…"), this);
    connect(m_health, &QAction::triggered, this, [this] {
        if (const Disk *d = selectedDisk())
            HealthDialog(m_udisks, d->blockPath, this).exec();
    });

    m_badSectors = new QAction(themeIcon("tools-check-spelling", "edit-find"), tr("Scan for Bad &Sectors…"), this);
    connect(m_badSectors, &QAction::triggered, this, [this] {
        if (const Disk *d = selectedDisk())
            BadSectorsDialog(m_udisks, *d, this).exec();
    });

    m_checkStick = new QAction(themeIcon("drive-removable-media-usb", "drive-removable-media"), tr("Check a USB Stic&k…"), this);
    connect(m_checkStick, &QAction::triggered, this, [this] {
        const Disk *d = selectedDisk();
        StickCheckDialog(m_udisks, d ? d->blockPath : QString(), toolParent()).exec();
    });

    m_benchmark = new QAction(themeIcon("speedometer", "chronometer"), tr("&Benchmark…"), this);
    connect(m_benchmark, &QAction::triggered, this, [this] {
        if (const Disk *d = selectedDisk())
            BenchmarkDialog(m_udisks, *d, this).exec();
    });

    m_clone = new QAction(themeIcon("edit-copy", "edit-copy"), tr("&Clone Drive…"), this);
    connect(m_clone, &QAction::triggered, this, [this] {
        if (const Disk *d = selectedDisk())
            CloneDialog(m_udisks, d->blockPath, this).exec();
    });

    m_newIds = new QAction(themeIcon("view-refresh", "view-refresh"), tr("Give New &IDs…"), this);
    m_newIds->setToolTip(tr("Give a copy of a drive its own IDs, so it can be used next to the drive it was copied from"));
    connect(m_newIds, &QAction::triggered, this, [this] {
        if (const Disk *d = selectedDisk())
            NewIdsDialog(m_udisks, d->blockPath, this).exec();
    });

    // A selected partition is backed up or restored on its own; otherwise the whole drive.
    auto backupTarget = [this]() -> QString {
        if (const Volume *v = selectedVolume())
            return v->objectPath;
        const Disk *d = selectedDisk();
        return d ? d->blockPath : QString();
    };
    m_backup = new QAction(themeIcon("document-save-as", "document-save"), tr("&Back Up…"), this);
    connect(m_backup, &QAction::triggered, this, [this, backupTarget] {
        const QString path = backupTarget();
        if (!path.isEmpty())
            BackupDialog(m_udisks, path, this).exec();
    });
    m_restore = new QAction(themeIcon("document-revert", "edit-undo"), tr("R&estore Backup…"), this);
    connect(m_restore, &QAction::triggered, this, [this, backupTarget] {
        const QString path = backupTarget();
        if (!path.isEmpty())
            RestoreDialog(m_udisks, path, this).exec();
    });

    m_rescue = new QAction(themeIcon("tools-media-optical-copy", "edit-copy"), tr("Rescue &Copy…"), this);
    connect(m_rescue, &QAction::triggered, this, [this] {
        if (const Disk *d = selectedDisk())
            RescueDialog(m_udisks, d->blockPath, this).exec();
    });

    m_evidence = new QAction(themeIcon("document-save-as", "document-save"), tr("Make an E&vidence Image…"), this);
    m_evidence->setObjectName(QStringLiteral("evidence"));
    m_evidence->setToolTip(tr("Every sector, read-only, into an evidence file (.E01) with the case's details and hashes, and a case report"));
    connect(m_evidence, &QAction::triggered, this, [this] {
        if (const Disk *d = selectedDisk())
            EvidenceDialog(m_udisks, *d, selectedVolume(), this).exec();
    });

    m_certificates = new QAction(themeIcon("document-print", "document-save"), tr("Erasure &Certificates…"), this);
    m_certificates->setObjectName(QStringLiteral("certificates"));
    m_certificates->setToolTip(tr("Certificates of erasure for the drives wiped or erased here"));
    connect(m_certificates, &QAction::triggered, this, [this] { CertificatesDialog(m_udisks, this).exec(); });

    m_testBoot = new QAction(themeIcon("computer", "system-run"), tr("&Try Starting It…"), this);
    m_testBoot->setObjectName(QStringLiteral("testBoot"));
    m_testBoot->setToolTip(tr("Starts the drive in a virtual machine, to see that it starts (a clone, say): nothing on it changes"));
    connect(m_testBoot, &QAction::triggered, this, [this] {
        if (const Disk *d = selectedDisk())
            TestBootDialog(m_udisks, d, QString(), this).exec();
    });
    m_testBootImage = new QAction(themeIcon("computer", "system-run"), tr("Try &Starting a Disk Image…"), this);
    connect(m_testBootImage, &QAction::triggered, this, [this] {
        const QString image = QFileDialog::getOpenFileName(this, tr("Try Starting a Disk Image"), QDir::homePath(),
                                                           tr("Disk images (*.img *.raw *.vhd *.vhdx *.vmdk *.qcow2 *.vdi);;All files (*)"));
        if (!image.isEmpty())
            TestBootDialog(m_udisks, nullptr, image, this).exec();
    });

    m_clonezilla = new QAction(themeIcon("document-revert", "edit-undo"), tr("Restore from &Clonezilla…"), this);
    m_clonezilla->setObjectName(QStringLiteral("clonezilla"));
    m_clonezilla->setToolTip(tr("Puts one of a Clonezilla backup's partition images back on this partition"));
    connect(m_clonezilla, &QAction::triggered, this, [this] {
        if (const Volume *v = selectedVolume())
            ClonezillaDialog(m_udisks, *v, this).exec();
        updateActions();
    });

    m_winRe = new QAction(themeIcon("tools-wizard", "system-run"), tr("Make Room for Windows &Recovery…"), this);
    m_winRe->setObjectName(QStringLiteral("winRe"));
    m_winRe->setToolTip(tr("Windows Update failing with 0x80070643: its recovery partition is too small. Microsoft's fix, done for you"));
    connect(m_winRe, &QAction::triggered, this, [this] { WinReDialog(m_udisks, this).exec(); });

    m_linuxBoot = new QAction(themeIcon("tools-wizard", "system-run"), tr("Repair &Linux Boot…"), this);
    m_linuxBoot->setObjectName(QStringLiteral("linuxBoot"));
    m_linuxBoot->setToolTip(tr("A Linux that doesn't start because its boot loader is gone or broken: its own tools put it back"));
    connect(m_linuxBoot, &QAction::triggered, this, [this] { LinuxBootDialog(m_udisks, this).exec(); });

    m_bootMenu = new QAction(themeIcon("system-reboot", "system-run"), tr("&Boot Menu Entries…"), this);
    m_bootMenu->setObjectName(QStringLiteral("bootMenu"));
    m_bootMenu->setToolTip(tr("The PC's own boot menu: put a system's entry back first, add one, or start one just once; and a report "
                              "of how the PC starts"));
    connect(m_bootMenu, &QAction::triggered, this, [this] { BootEntriesDialog(m_udisks, this).exec(); });

    m_saveFiles = new QAction(themeIcon("document-save-all", "document-save"), tr("&Save My Files…"), this);
    m_saveFiles->setObjectName(QStringLiteral("saveFiles"));
    m_saveFiles->setToolTip(tr("Each person's Desktop, Documents, Pictures and more, and their bookmarks, copied off a drive: before "
                               "reinstalling, or when a PC won't start"));
    connect(m_saveFiles, &QAction::triggered, this, [this] { SaveFilesDialog(m_udisks, this).exec(); });

    m_drivers = new QAction(themeIcon("preferences-system", "system-run"), tr("Windows &Drivers and Services…"), this);
    m_drivers->setObjectName(QStringLiteral("drivers"));
    m_drivers->setToolTip(tr("What starts with a Windows, newest first: when it stops starting after an install, switch the newest "
                             "driver off"));
    connect(m_drivers, &QAction::triggered, this, [this] {
        WindowsPicker picker(m_udisks, true, tr("Which Windows? One on a drive is mounted so its drivers can be switched off."), this);
        if (picker.exec() == QDialog::Accepted)
            DriversDialog(picker.root(), this).exec();
    });

    m_activity = new QAction(themeIcon("view-history", "document-open-recent"), tr("What This PC &Did…"), this);
    m_activity->setObjectName(QStringLiteral("activity"));
    m_activity->setToolTip(tr("What system is on a drive and what it was used for, from its own records: USB drives plugged in, programs "
                              "run, files opened, searches, websites, networks joined, sign-ins, programs installed, accounts"));
    connect(m_activity, &QAction::triggered, this, [this] {
        WindowsPicker picker(m_udisks, false, tr("Which system? One on a drive is mounted read-only to read its records."), this, true);
        if (picker.exec() == QDialog::Accepted)
            ActivityDialog(picker.root(), this).exec();
    });

    m_lookCloser = new QAction(themeIcon("diskanvil-live-lookcloser", "zoom-in"), tr("&Look Closer at Pictures and Films…"), this);
    m_lookCloser->setObjectName(QStringLiteral("lookCloser"));
    m_lookCloser->setToolTip(tr("Any picture or film in the viewer, with its tools: Enhance and its profiles, Pick a Spot, Straighten, "
                                "Stack Frames, any speed"));
    connect(m_lookCloser, &QAction::triggered, this, [this] {
        // Its own window, so it can stay open next to this one. Opened on its own (a tile on
        // DiskAnvil Live's home screen, --open), it's all there is: DiskAnvil closes with it.
        if (!isVisible()) {
            LookCloserDialog(this).exec();
            return;
        }
        auto *look = new LookCloserDialog(this);
        look->setAttribute(Qt::WA_DeleteOnClose);
        look->show();
    });

    m_productKey = new QAction(themeIcon("dialog-password", "document-encrypt"), tr("Windows Product &Key…"), this);
    m_productKey->setObjectName(QStringLiteral("productKey"));
    m_productKey->setToolTip(tr("The key in this PC's firmware, and the ones Windows installs here use: to have before reinstalling"));
    connect(m_productKey, &QAction::triggered, this, [this] { ProductKeyDialog(m_udisks, this).exec(); });

    m_openSets = new QAction(themeIcon("drive-multidisk", "drive-harddisk"), tr("Open RAID, NAS and Dynamic Disks…"), this);
    m_openSets->setObjectName(QStringLiteral("openSets"));
    m_openSets->setToolTip(tr("Drives that belong together (a NAS's, a RAID array's, Windows' dynamic disks), opened read-only"));
    connect(m_openSets, &QAction::triggered, this, &MainWindow::openClosedSets);

    m_remoteHelp = new QAction(themeIcon("preferences-desktop-remote-desktop", "network-connect"), tr("Let Someone &Help…"), this);
    m_remoteHelp->setObjectName(QStringLiteral("remoteHelp"));
    m_remoteHelp->setToolTip(tr("Someone you trust, on the same network, sees this screen and can use it, or works in a terminal"));
    connect(m_remoteHelp, &QAction::triggered, this, [this] { RemoteHelpDialog(toolParent()).exec(); });

    m_netBoot = new QAction(themeIcon("network-server", "network-wired"), tr("Start Other PCs from This One…"), this);
    m_netBoot->setObjectName(QStringLiteral("netBoot"));
    m_netBoot->setToolTip(tr("Other PCs on the same network start DiskAnvil Live from this one, with nothing installed on them"));
    connect(m_netBoot, &QAction::triggered, this, [this] { NetBootDialog(toolParent()).exec(); });

    m_writeBlock = new QAction(themeIcon("object-locked", "document-encrypt"), tr("Write-Protect Every Drive"), this);
    m_writeBlock->setObjectName(QStringLiteral("writeBlock"));
    m_writeBlock->setCheckable(true);
    m_writeBlock->setToolTip(writeblock::available()
                                 ? tr("Every drive but this stick read-only, and those plugged in later too: nothing done here can change "
                                      "them (for evidence, or a drive that mustn't be touched)")
#ifdef Q_OS_WIN
                                 : tr("Windows writes to a drive as soon as it's plugged in, before any program can stop it: start the PC "
                                      "from DiskAnvil Live, \"Drives write-protected\", for that"));
#else
                                 : tr("Only DiskAnvil Live can: a running system's own drive can't be made read-only. Its boot menu has "
                                      "\"Drives write-protected\" too."));
#endif
    m_writeBlock->setEnabled(writeblock::available());
    connect(m_writeBlock, &QAction::triggered, this, [this](bool on) { setWriteBlock({on ? QStringLiteral("on") : QStringLiteral("off")}); });

    m_whatHappened = new QAction(themeIcon("help-hint", "help-contents"), tr("&What Happened?…"), this);
    m_whatHappened->setToolTip(tr("Lost files? Start here: what happened, the odds, what not to do, and which tool to use"));
    connect(m_whatHappened, &QAction::triggered, this, [this] {
        WhatHappenedDialog dialog(m_udisks->disks(), rescue::runningInRescue(), this);
        connect(&dialog, &WhatHappenedDialog::openTool, this, [this, &dialog](const QString &tool, const QString &blockPath) {
            // The drive picked there is the one picked here, then the tool is opened as from the menu.
            const QVector<Disk> &disks = m_udisks->disks();
            for (int i = 0; i < disks.size(); ++i) {
                if (disks[i].blockPath == blockPath)
                    m_map->selectDisk(i);
            }
            const QHash<QString, QAction *> actions = {{QStringLiteral("lost-files"), m_findFiles},  {QStringLiteral("rescue-copy"), m_rescue},
                                                       {QStringLiteral("health"), m_health},         {QStringLiteral("recover-partitions"), m_recover},
                                                       {QStringLiteral("rescue-usb"), m_rescueUsb},  {QStringLiteral("virus-scan"), m_virusScan},
                                                       {QStringLiteral("drivers"), m_drivers}, {QStringLiteral("activity"), m_activity},       {QStringLiteral("save-files"), m_saveFiles},
                                                       {QStringLiteral("boot-menu"), m_bootMenu}};
            if (QAction *action = actions.value(tool)) {
                updateActions();
                dialog.accept();
                action->trigger();
            }
        });
        dialog.exec();
    });

    m_usage = new QAction(themeIcon("view-statistics", "drive-harddisk"), tr("Disk &Usage…"), this);
    connect(m_usage, &QAction::triggered, this, [this] {
        const Volume *v = selectedVolume();
        if (v && !v->mounts().isEmpty())
            UsageDialog(v->mounts().first(), volumeTitle(*v), this).exec();
    });
    m_optimize = new QAction(themeIcon("speedometer", "system-run"), tr("&Optimize Drives…"), this);
    connect(m_optimize, &QAction::triggered, this, [this] { OptimizeDialog(m_udisks, this).exec(); });
    m_cleanup = new QAction(themeIcon("edit-clear-history", "edit-clear"), tr("Disk &Cleanup…"), this);
    connect(m_cleanup, &QAction::triggered, this, [this] { CleanupDialog(m_udisks, this).exec(); });
    m_virusScan = new QAction(themeIcon("diskanvil-live-virus", "security-high"), tr("Scan for &Viruses…"), this);
    m_virusScan->setToolTip(tr("Look for known malware on a drive or in a folder"));
    connect(m_virusScan, &QAction::triggered, this, [this] { VirusDialog(m_udisks, toolParent()).exec(); });
    m_snapshots = new QAction(themeIcon("camera-photo", "document-open-recent"), tr("Btrfs &Snapshots…"), this);
    connect(m_snapshots, &QAction::triggered, this, [this] { SnapshotsDialog(this).exec(); });

    auto *quit = new QAction(themeIcon("application-exit", "window-close"), tr("&Quit"), this);
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close); // closeEvent asks about running add-ons

    // Stops what can safely be stopped halfway: a wipe, a check, a self-test.
    m_stop = new QAction(themeIcon("process-stop", "media-playback-stop"), tr("&Stop"), this);
    m_stop->setToolTip(tr("Stop the wipe, check or self-test that's running"));
    m_stop->setEnabled(false);
    connect(m_stop, &QAction::triggered, this, [this] {
        // The selected drive's first, then any.
        const Disk *selected = selectedDisk();
        auto touches = [](const Job &j, const Disk &d) {
            if (j.objects.contains(d.blockPath) || j.objects.contains(d.drivePath))
                return true;
            for (const Volume &v : d.volumes) {
                if (j.objects.contains(v.objectPath) || (!v.cleartextPath.isEmpty() && j.objects.contains(v.cleartextPath)))
                    return true;
            }
            return false;
        };
        QString chosen;
        for (const Job &j : m_udisks->jobs()) {
            if (!jobShown(j) || !jobCantStop(j).isEmpty())
                continue;
            const bool mine = selected && touches(j, *selected);
            if (chosen.isEmpty() || mine)
                chosen = j.path;
            if (mine)
                break;
        }
        if (!chosen.isEmpty()) {
            stopJob(chosen);
            return;
        }
        for (const Disk &d : m_udisks->disks()) {
            if (d.isRaid && (d.raidSync == QLatin1String("check") || d.raidSync == QLatin1String("repair"))
                && (!selected || selected->blockPath == d.blockPath)) {
                m_udisks->raidSyncAction(d, QStringLiteral("idle"));
                return;
            }
            if (d.health.selftestStatus == QLatin1String("inprogress") && (!selected || selected->blockPath == d.blockPath)) {
                m_udisks->smartSelftestAbort(d);
                return;
            }
        }
    });
    m_jobRecheck = new QTimer(this);
    m_jobRecheck->setSingleShot(true);
    connect(m_jobRecheck, &QTimer::timeout, this, [this] {
        updateJobBars();
        showNoticesIfAny();
    });

    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addActions({m_openImage, m_findFilesImage, m_testBootImage, m_writeImage, m_windowsUsb, m_rescueUsb, m_liveOnPc});
    file->addSeparator();
    file->addAction(m_refresh);
    file->addSeparator();
    file->addAction(quit);

    QMenu *action = menuBar()->addMenu(tr("&Action"));
    action->addAction(m_stop);
    action->addSeparator();
    action->addActions({m_open, m_mount, m_unmount, m_safelyRemove});
    action->addSeparator();
    action->addActions({m_unlock, m_lock, m_changePass, m_luksBackup, m_luksRestore});
    action->addSeparator();
    action->addActions({m_newPartition, m_format, m_resize, m_rename, m_typeFlags, m_check, m_startup, m_delete});
    action->addSeparator();
    action->addActions({m_newTable, m_inspect, m_recover, m_findFiles, m_wipe, m_sdCard, m_secureErase, m_detachImage});
    action->addSeparator();
    action->addActions({m_health, m_badSectors, m_checkStick, m_benchmark, m_raidCheck});
    action->addSeparator();
    action->addActions({m_backup, m_restore, m_clonezilla, m_evidence, m_clone, m_newIds, m_rescue});
    action->addSeparator();
    action->addActions({m_copy, m_properties});

    QMenu *view = menuBar()->addMenu(tr("&View"));
    QMenu *themes = view->addMenu(themeIcon("preferences-desktop-theme", "preferences-desktop-color"), tr("&Theme"));
    themes->setToolTipsVisible(true);
    connect(themes, &QMenu::aboutToShow, this, [this, themes] {
        themes->clear();
        auto *group = new QActionGroup(themes);
        for (const Theme::Choice &c : Theme::choices(m_addons)) {
            QAction *item = themes->addAction(QString(c.name).replace(QLatin1Char('&'), QStringLiteral("&&")));
            item->setCheckable(true);
            item->setChecked(c.id == Theme::instance().currentId());
            item->setEnabled(c.usable);
            item->setToolTip(c.reason);
            group->addAction(item);
            const QString id = c.id;
            connect(item, &QAction::triggered, this, [this, id] { Theme::instance().select(id, m_addons); });
        }
        themes->addSeparator();
        themes->addAction(tr("Make a Theme…"), this, [this] {
            ThemeMaker(&m_addons, this).exec();
            refreshAddons();
        });
        themes->addAction(tr("Get More…"), this, [this] {
            CatalogDialog(&m_addons, this).exec();
            m_addons.load();
            refreshAddons();
        });
    });

    QMenu *tools = menuBar()->addMenu(tr("&Tools"));
    tools->setToolTipsVisible(true);
    connect(tools, &QMenu::aboutToShow, this, [this, tools] {
        tools->clear();
        tools->addAction(m_whatHappened);
        tools->addAction(m_saveFiles);
        tools->addAction(m_openSets);
        {
            const QSignalBlocker block(m_writeBlock);
            m_writeBlock->setChecked(writeblock::isOn());
        }
        tools->addAction(m_writeBlock);
        tools->addAction(m_remoteHelp);
        tools->addAction(m_netBoot);
        tools->addAction(m_bootMenu);
        tools->addAction(m_linuxBoot);
        tools->addAction(m_drivers);
        tools->addAction(m_activity);
        tools->addAction(m_lookCloser);
        tools->addAction(m_productKey);
        tools->addAction(m_certificates);
        tools->addSeparator();
#ifdef Q_OS_WIN
        // Quick Fixes' tools, and add-ons (Linux commands), aren't in the Windows version yet.
        tools->addActions({m_saveFiles, m_virusScan, m_bootMenu, m_drivers, m_activity, m_lookCloser, m_winRe, m_productKey, m_certificates, m_usage, m_cleanup, m_optimize, m_snapshots});
        return;
#endif
        tools->addAction(themeIcon("tools-wizard", "system-run"), tr("&Quick Fixes…"), this, [this] {
            // Its own window, so it can stay open next to this one.
            auto *fixes = new HomeWindow(m_udisks, HomeWindow::Mode::Window);
            fixes->setAttribute(Qt::WA_DeleteOnClose);
            fixes->show();
        });
        tools->addSeparator();
        tools->addActions({m_virusScan, m_usage, m_cleanup, m_optimize, m_snapshots});
        tools->addSection(tr("Add-ons"));
        // Every add-on action is listed. One that doesn't fit the selection is greyed out,
        // with the reason where a shortcut would go (and as its tooltip).
        int listed = 0;
        for (const Addon &a : m_addons.all()) {
            if (!a.enabled || !a.error.isEmpty())
                continue;
            for (const AddonAction &act : a.actions) {
                const QString id = a.id, label = act.label;
                const QString why = addonReason(id, label);
                const QString keys = Addons::shortcut(Addons::actionKey(a, act));
                QString text = QString(label).replace(QLatin1Char('&'), QStringLiteral("&&"));
                if (!why.isEmpty() || !keys.isEmpty())
                    text += QLatin1Char('\t') + (why.isEmpty() ? QKeySequence(keys, QKeySequence::PortableText).toString(QKeySequence::NativeText) : why);
                QAction *item = tools->addAction(QIcon::fromTheme(act.icon, QIcon::fromTheme(QStringLiteral("application-x-addon"))), text,
                                                 this, [this, id, label] { runAddon(id, label); });
                item->setEnabled(why.isEmpty());
                item->setToolTip(why.isEmpty() ? a.name : why);
                ++listed;
            }
        }
        if (listed == 0)
            tools->addAction(tr("No add-ons installed"))->setEnabled(false);
        tools->addSeparator();
        tools->addAction(themeIcon("preferences-plugin", "application-x-addon"), tr("&Add-ons…"), this, [this] {
            AddonsDialog(&m_addons, takenShortcuts(), [this](const Addon &a, const AddonAction &act) { runAddonWith(a, act, true); }, this).exec();
            refreshAddons();
        });
    });

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    QAction *handbook = help->addAction(themeIcon("help-contents", "help-browser"), tr("DiskAnvil &Help"), this, [this] {
        if (!m_help)
            m_help = new HelpWindow(this);
        m_help->show();
        m_help->raise();
        m_help->activateWindow();
    });
    handbook->setShortcut(QKeySequence::HelpContents);
    help->addSeparator();
    help->addAction(themeIcon("update-none", "system-software-update"), tr("Check for &Updates…"), this, &MainWindow::checkForUpdates);
    help->addAction(themeIcon("tools-report-bug", "dialog-warning"), tr("Report a &Bug…"), this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral(APP_HOMEPAGE "/issues/new/choose")));
    });
    help->addAction(themeIcon("internet-services", "applications-internet"), tr("Project &Website"), this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral(APP_HOMEPAGE)));
    });
    help->addAction(themeIcon("help-donate", "emblem-favorite"), tr("&Support DiskAnvil (Ko-fi)"), this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral(APP_SUPPORT_URL)));
    });
    help->addSeparator();
    help->addAction(QApplication::windowIcon(), tr("&About DiskAnvil"), this, [this] {
        AboutDialog(m_udisks->daemonVersion(), this).exec();
    });
    help->addAction(themeIcon("qtcreator", "help-about"), tr("About &Qt"), qApp, &QApplication::aboutQt);

#ifdef Q_OS_WIN
    // Linux's own things (LUKS, fstab, RAID, Btrfs), and what the Windows version can't do yet.
    for (QAction *a : {m_startup, m_raidCheck, m_snapshots})
        a->setVisible(false);
#endif

    m_toolbar = addToolBar(tr("Main"));
    m_toolbar->setObjectName(QStringLiteral("mainToolbar"));
    m_toolbar->setMovable(false);
    m_toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_toolbar->addActions({m_refresh, m_stop});
    m_toolbar->addSeparator();
    m_toolbar->addActions({m_open, m_mount, m_unmount, m_safelyRemove});
    m_toolbar->addSeparator();
    m_toolbar->addActions({m_writeImage, m_properties});
}

void MainWindow::rebuild()
{
    // Remember each drive's partition layout, so Recover Partitions can put it back. Not
    // while something is being changed: the steps in between aren't worth keeping.
    if (!m_udisks->isBusy() && m_udisks->jobs().isEmpty()) {
        for (const Disk &d : m_udisks->disks()) {
            if (!d.isLoop && !d.health.key.isEmpty() && !d.tableType.isEmpty())
                recover::remember(d.health.key, recover::fromDisk(d));
        }
    }
    m_map->setDisks(m_udisks->disks());
    fillTable();
    syncTableToMap();
    updateActions();
    if (!m_udisks->isAvailable())
        statusBar()->showMessage(tr("UDisks2 is not available: %1").arg(m_udisks->lastError()));
    updateProgress();
    updateNotices();
    updateCloneNotices();
    updateClosedNotices();
    updateWriteBlockNotice();
}

void MainWindow::setWriteBlock(const QStringList &args)
{
    const QStringList command = writeblock::command(args);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QProcess run;
    run.setProcessChannelMode(QProcess::MergedChannels);
    run.start(command.first(), command.mid(1));
    const bool ok = run.waitForFinished(60000) && run.exitStatus() == QProcess::NormalExit && run.exitCode() == 0;
    QApplication::restoreOverrideCursor();
    const QString said = QString::fromLocal8Bit(run.readAll()).trimmed();
    qCInfo(lcOps).noquote() << "Write protection:" << args.join(QLatin1Char(' ')) << (ok ? "done" : "failed") << said;
    if (!ok)
        QMessageBox::warning(this, tr("Write-Protect Every Drive"),
                             tr("That didn't work.\n\n%1").arg(said.isEmpty() ? run.errorString() : said.right(1500)));
    // The kernel says so at once; UDisks a moment later.
    m_udisks->refresh();
    updateWriteBlockNotice();
    QTimer::singleShot(1500, m_udisks, &UDisks::refresh);
}

// A bar for as long as the drives are write-protected, with which ones aren't.
void MainWindow::updateWriteBlockNotice()
{
    const bool on = writeblock::isOn();
    const QStringList allowed = writeblock::allowed();
    const QString key = on ? QStringLiteral("on ") + allowed.join(QLatin1Char(' ')) : QString();
    if (key == m_writeBlockKey)
        return;
    m_writeBlockKey = key;
    for (NoticeBar *old : m_notices->findChildren<NoticeBar *>(QStringLiteral("writeBlock"), Qt::FindDirectChildrenOnly)) {
        old->hide();
        old->deleteLater();
    }
    if (!on)
        return showNoticesIfAny();
    QString text = tr("Every drive but this stick is write-protected: nothing done here can change them, drives plugged in later too.");
    if (!allowed.isEmpty())
        text += QLatin1Char(' ') + tr("Let through, to save to: %1.").arg(allowed.join(QStringLiteral(", ")));
    auto *bar = new NoticeBar(NoticeBar::Level::Warning, text);
    bar->setObjectName(QStringLiteral("writeBlock"));
    QPushButton *allow = bar->addButton(tr("Let One Be Written…"));
    allow->setObjectName(QStringLiteral("writeBlockAllow"));
    connect(allow, &QPushButton::clicked, this, [this, allow] {
        QMenu menu(this);
        for (const Disk &d : m_udisks->disks()) {
            const QString name = shortDevice(d.device);
            if (d.isLoop || !d.readOnly || writeblock::allowed().contains(name))
                continue;
            menu.addAction(tr("%1 (%2, %3)").arg(name, d.model.isEmpty() ? tr("drive") : d.model, formatSize(d.size)), this,
                           [this, name] { setWriteBlock({QStringLiteral("allow"), name}); });
        }
        if (menu.isEmpty())
            menu.addAction(tr("No write-protected drives"))->setEnabled(false);
        menu.exec(allow->mapToGlobal(QPoint(0, allow->height())));
    });
    QPushButton *off = bar->addButton(tr("Switch Off"));
    connect(off, &QPushButton::clicked, this, [this] { QTimer::singleShot(0, this, [this] { setWriteBlock({QStringLiteral("off")}); }); });
    m_notices->layout()->addWidget(bar);
    bar->show();
    showNoticesIfAny();
}

#ifdef Q_OS_WIN
void MainWindow::updateClosedNotices()
{
}

void MainWindow::openClosedSets()
{
    // Windows opens its own dynamic disks; Linux RAID (a NAS's drives) it can't read at all.
    QMessageBox::information(this, tr("Open RAID, NAS and Dynamic Disks"),
                             tr("Windows opens its own dynamic disks by itself (Disk Management). Drives from a NAS or a Linux RAID "
                                "array it can't read: DiskAnvil Live opens them, read-only."));
}
#else
namespace {

bool dynamicVolumesOpen()
{
    return !QDir(QStringLiteral("/dev/mapper")).entryList({QStringLiteral("ldm_vol_*")}, QDir::System).isEmpty();
}

} // namespace

// A bar while drives that belong together are here but not opened: a NAS's, a RAID array's,
// Windows' dynamic disks.
void MainWindow::updateClosedNotices()
{
    const QVector<closedsets::Set> sets = closedsets::find(m_udisks->disks(), dynamicVolumesOpen());
    QStringList key;
    for (const closedsets::Set &s : sets)
        key << closedsets::describe(s);
    if (key == m_closedNoticeKey)
        return;
    m_closedNoticeKey = key;
    for (NoticeBar *old : m_notices->findChildren<NoticeBar *>(QStringLiteral("closed"), Qt::FindDirectChildrenOnly)) {
        old->hide();
        old->deleteLater();
    }
    if (sets.isEmpty())
        return showNoticesIfAny();
    auto *bar = new NoticeBar(NoticeBar::Level::Info, tr("These belong together and aren't opened: %1. Opened read-only, their files can "
                                                         "be copied off, and nothing on them changes.")
                                                          .arg(key.join(QStringLiteral("; "))));
    bar->setObjectName(QStringLiteral("closed"));
    QPushButton *open = bar->addButton(tr("Open Read-Only…"));
    connect(open, &QPushButton::clicked, this, [this] { QTimer::singleShot(0, this, &MainWindow::openClosedSets); });
    m_notices->layout()->addWidget(bar);
    bar->show();
    showNoticesIfAny();
}

void MainWindow::openClosedSets()
{
    const QString title = tr("Open RAID, NAS and Dynamic Disks");
    const QVector<closedsets::Set> sets = closedsets::find(m_udisks->disks(), dynamicVolumesOpen());
    if (sets.isEmpty()) {
        QMessageBox::information(this, title, tr("Nothing to open: no RAID, NAS or Windows dynamic disks are here that aren't open already."));
        return;
    }
    const QStringList missing = closedsets::missing(sets, [](const QString &program) { return host::programExists(program); });
    if (!missing.isEmpty()) {
        QMessageBox::information(this, title,
                                 tr("Opening these takes %1, which isn't on this computer. Install it, or start from DiskAnvil Live, "
                                    "which has everything.")
                                     .arg(missing.join(QStringLiteral(", "))));
        return;
    }
    QStringList list;
    for (const closedsets::Set &s : sets)
        list << QStringLiteral("• ") + closedsets::describe(s);
    if (QMessageBox::question(this, title,
                              tr("Open these, read-only?\n\n%1\n\nNothing is written to them: an array that was left unclean isn't "
                                 "rebuilt, and Windows' dynamic volumes aren't changed. It takes the admin password.")
                                  .arg(list.join(QLatin1Char('\n'))))
        != QMessageBox::Yes)
        return;
    // As root: DiskAnvil Live's sudo, or the admin password through pkexec.
    const QString script = closedsets::openScript(sets);
    const QStringList command = rescue::runningInRescue() ? QStringList{QStringLiteral("sudo"), QStringLiteral("-n"), QStringLiteral("sh"), QStringLiteral("-c"), script}
                              : ::geteuid() == 0          ? QStringList{QStringLiteral("sh"), QStringLiteral("-c"), script}
                                                          : host::command({QStringLiteral("pkexec"), QStringLiteral("sh"), QStringLiteral("-c"), script});
    statusBar()->showMessage(tr("Opening…"));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QProcess run;
    run.setProcessChannelMode(QProcess::MergedChannels);
    run.start(command.first(), command.mid(1));
    run.waitForFinished(300000);
    QApplication::restoreOverrideCursor();
    statusBar()->clearMessage();
    const QString said = QString::fromLocal8Bit(run.readAll()).trimmed();
    qCInfo(lcOps).noquote() << "Open closed sets:" << script << "->" << said;
    m_udisks->refresh();
    const QVector<closedsets::Set> left = closedsets::find(m_udisks->disks(), dynamicVolumesOpen());
    if (left.size() < sets.size())
        QMessageBox::information(this, title, tr("Opened. They're in the list of drives now (read-only): mount a volume to copy files off."));
    else
        QMessageBox::warning(this, title, tr("They didn't open.\n\n%1").arg(said.isEmpty() ? tr("No reason was given.") : said.right(1500)));
}
#endif

// A bar for each pair of drives that are copies of each other (the same IDs), until it's
// dismissed: Linux may mount the wrong one, and Windows switches one off.
void MainWindow::updateCloneNotices()
{
    const QVector<Disk> &disks = m_udisks->disks();
    QSettings settings(QStringLiteral("diskanvil"), QStringLiteral("diskanvil"));
    QVector<newids::Clash> shown;
    QStringList key;
    for (const newids::Clash &c : newids::clashes(disks)) {
        const QString id = newids::clashKey(c, disks);
        if (settings.value(QStringLiteral("dismissed/clone-") + id).toBool())
            continue;
        shown << c;
        key << id + c.first + c.second;
    }
    if (key == m_cloneNoticeKey)
        return;
    m_cloneNoticeKey = key;
    for (NoticeBar *old : m_notices->findChildren<NoticeBar *>(QStringLiteral("clone"), Qt::FindDirectChildrenOnly)) {
        old->hide();
        old->deleteLater();
    }
    for (const newids::Clash &c : std::as_const(shown).mid(0, 2)) {
        const Disk *a = m_udisks->diskByPath(c.first), *b = m_udisks->diskByPath(c.second);
        if (!a || !b)
            continue;
        // The one to give new IDs: never the system drive; a USB drive before an inner one.
        const Disk *copy = b->isSystem || (!a->isSystem && a->bus == QLatin1String("usb") && b->bus != QLatin1String("usb")) ? a : b;
        auto name = [](const Disk *d) { return QStringLiteral("%1 (%2)").arg(d->model.isEmpty() ? tr("A drive") : d->model, shortDevice(d->device)); };
#ifdef Q_OS_WIN
        auto *bar = new NoticeBar(NoticeBar::Level::Warning,
                                  tr("%1 and %2 are copies of each other: they share %3. Windows switches one of them off.")
                                      .arg(name(a), name(b), c.ids.join(QStringLiteral(", "))));
#else
        auto *bar = new NoticeBar(NoticeBar::Level::Warning,
                                  tr("%1 and %2 are copies of each other: they share %3. Linux may use the wrong one at the next start, "
                                     "and Windows switches one of them off.")
                                      .arg(name(a), name(b), c.ids.join(QStringLiteral(", "))));
#endif
        bar->setObjectName(QStringLiteral("clone"));
        const QString device = copy->device;
        if (!copy->isSystem) {
            QPushButton *fix = bar->addButton(tr("Give %1 New IDs…").arg(shortDevice(copy->device)));
            connect(fix, &QPushButton::clicked, this, [this, device] { runOnDrive(device, m_newIds); });
        }
        QPushButton *dismiss = bar->addButton(tr("Dismiss"));
        const QString id = newids::clashKey(c, disks);
        connect(dismiss, &QPushButton::clicked, this, [this, id] {
            QTimer::singleShot(0, this, [this, id] {
                QSettings(QStringLiteral("diskanvil"), QStringLiteral("diskanvil")).setValue(QStringLiteral("dismissed/clone-") + id, true);
                m_cloneNoticeKey.clear();
                updateCloneNotices();
            });
        });
        m_notices->layout()->addWidget(bar);
        bar->show();
    }
    showNoticesIfAny();
}

// Select a drive and run one of the window's actions on it, the same as from the menu (so
// it's only run when the action is available for it). Queued, so a bar can be rebuilt
// while its button's dialog is open.
void MainWindow::runOnDrive(const QString &device, QAction *action)
{
    QTimer::singleShot(0, this, [this, device, action] {
        if (!selectDevice(device))
            return;
        if (action->isEnabled())
            action->trigger();
        else
            statusBar()->showMessage(tr("%1 can't be used on %2 right now.").arg(action->text().remove(QLatin1Char('&')), shortDevice(device)), 8000);
    });
}

// A bar for each drive whose health is a warning or worse, failing ones first, until it's
// dismissed. It comes back when it gets worse. At most three, then a count.
void MainWindow::updateNotices()
{
    updateJobBars();
    QSettings settings(QStringLiteral("diskanvil"), QStringLiteral("diskanvil"));
    QVector<const Disk *> drives;
    for (const Disk &d : m_udisks->disks()) {
        if (d.health.state != Health::State::Warning && d.health.state != Health::State::Failing)
            continue;
        if (!health::worseThan(d.health, settings.value(QStringLiteral("dismissed/") + d.health.key).toStringList()))
            continue;
        drives << &d;
    }
    std::stable_sort(drives.begin(), drives.end(), [](const Disk *a, const Disk *b) { return a->health.state > b->health.state; });

    QStringList key;
    for (const Disk *d : std::as_const(drives))
        key << d->blockPath + QLatin1Char(' ') + d->model + QLatin1Char(' ') + d->health.summary + QLatin1Char(' ')
                + health::signature(d->health).join(QLatin1Char(','));
    if (key == m_noticeKey) {
        showNoticesIfAny();
        return;
    }
    m_noticeKey = key;

    for (NoticeBar *old : m_notices->findChildren<NoticeBar *>(QStringLiteral("health"), Qt::FindDirectChildrenOnly)) {
        old->hide();
        old->deleteLater();
    }
    const int shown = qMin<int>(drives.size(), 3);
    for (int i = 0; i < shown; ++i) {
        const Disk &d = *drives[i];
        const Health &h = d.health;
        const bool failing = h.state == Health::State::Failing;
        const QString name = QStringLiteral("%1 (%2)").arg(d.model.isEmpty() ? tr("A drive") : d.model, shortDevice(d.device));
        auto *bar = new NoticeBar(failing ? NoticeBar::Level::Danger : NoticeBar::Level::Warning,
                                  failing ? tr("%1 is failing. Copy what you want to keep to another drive now.").arg(name)
                                          : tr("%1: %2.").arg(name, h.summary));
        bar->setObjectName(QStringLiteral("health"));
        // Spots it can't read stop a normal backup; Rescue Copy works around them.
        const bool unreadable = health::hasUnreadableSpots(h);
        const QString device = d.device;
        if (!d.isSystem) {
            QPushButton *copy = bar->addButton(unreadable && !d.isLoop ? tr("Rescue Copy…") : tr("Back Up…"));
            QAction *action = unreadable && !d.isLoop ? m_rescue : m_backup;
            copy->setToolTip(action == m_rescue ? tr("Copy the whole drive to another one, working around the spots it can't read.")
                                                : tr("Save an image of the drive."));
            connect(copy, &QPushButton::clicked, this, [this, device, action] { runOnDrive(device, action); });
        }
        QPushButton *details = bar->addButton(tr("Details…"));
        connect(details, &QPushButton::clicked, this, [this, device] { runOnDrive(device, m_health); });
        QPushButton *dismiss = bar->addButton(tr("Dismiss"));
        dismiss->setToolTip(tr("Hide this until it gets worse."));
        const QString blockPath = d.blockPath, healthKey = h.key;
        const QStringList signature = health::signature(h);
        connect(dismiss, &QPushButton::clicked, this, [this, blockPath, healthKey, signature] {
            QTimer::singleShot(0, this, [this, blockPath, healthKey, signature] {
                QSettings(QStringLiteral("diskanvil"), QStringLiteral("diskanvil")).setValue(QStringLiteral("dismissed/") + healthKey, signature);
                // Counters that only matter when they grow start again from here.
                if (const Disk *disk = m_udisks->diskByPath(blockPath))
                    health::acknowledge(healthKey, m_udisks->smartAttributes(*disk));
                updateNotices();
            });
        });
        m_notices->layout()->addWidget(bar);
        bar->show();
    }
    if (drives.size() > shown) {
        auto *more = new NoticeBar(NoticeBar::Level::Info,
                                   tr("%n more drive(s) need a look; their health is in the Status column.", nullptr, int(drives.size() - shown)));
        more->setObjectName(QStringLiteral("health"));
        m_notices->layout()->addWidget(more);
        more->show();
    }
    showNoticesIfAny();
}

void MainWindow::showNoticesIfAny()
{
    bool any = false;
    for (NoticeBar *bar : m_notices->findChildren<NoticeBar *>(Qt::FindDirectChildrenOnly))
        any = any || !bar->isHidden();
    m_notices->setVisible(any);
}

// "sda (HGST HTS545050A7E380)" or "sda1 (SATA500)": what a job works on.
QString MainWindow::jobTarget(const Job &job, bool *wholeDrive) const
{
    *wholeDrive = false;
    for (const Disk &d : m_udisks->disks()) {
        if (job.objects.contains(d.blockPath) || job.objects.contains(d.drivePath)) {
            *wholeDrive = true;
            return d.model.isEmpty() ? shortDevice(d.device) : tr("%1 (%2)").arg(shortDevice(d.device), d.model);
        }
        for (const Volume &v : d.volumes) {
            if (job.objects.contains(v.objectPath) || (!v.cleartextPath.isEmpty() && job.objects.contains(v.cleartextPath)))
                return v.label.isEmpty() ? shortDevice(v.device) : tr("%1 (%2)").arg(shortDevice(v.device), v.label);
        }
    }
    return tr("a drive");
}

// A bar on top for each long job, updated in place (so a click on Stop never lands on a
// bar that's being rebuilt). Quick jobs come and go without one.
void MainWindow::updateJobBars()
{
    const quint64 now = quint64(QDateTime::currentMSecsSinceEpoch()) * 1000;
    const quint64 settle = 2000000; // microseconds
    QSet<QString> seen;
    bool stoppable = false;
    qint64 recheck = -1;
    int index = 0;
    auto place = [this, &index, &seen](const QString &key, NoticeBar::Level level, const QString &text,
                                       const QString &buttonText, const std::function<void()> &onStop) {
        seen << key;
        NoticeBar *bar = m_jobBars.value(key);
        if (!bar) {
            bar = new NoticeBar(level, text);
            bar->setObjectName(QStringLiteral("job"));
            if (!buttonText.isEmpty()) {
                QPushButton *stop = bar->addButton(buttonText);
                // Queued: the question it asks runs a nested event loop.
                connect(stop, &QPushButton::clicked, this, [this, onStop] { QTimer::singleShot(0, this, onStop); });
            }
            m_jobBars.insert(key, bar);
            static_cast<QVBoxLayout *>(m_notices->layout())->insertWidget(index, bar);
            bar->show(); // now, not on the next pass, so showNoticesIfAny() counts it
        } else {
            bar->setText(text);
        }
        ++index;
    };
    for (const Job &j : m_udisks->jobs()) {
        if (!jobShown(j))
            continue;
        if (j.started && now < j.started + settle && !m_jobBars.contains(j.path)) {
            const qint64 wait = qint64(j.started + settle - now) / 1000 + 50;
            recheck = recheck < 0 ? wait : qMin(recheck, wait);
            continue;
        }
        bool whole = false;
        const QString name = jobTarget(j, &whole);
        const QString cantStop = jobCantStop(j), progress = jobProgress(j, now);
        QString text = progress.isEmpty() ? tr("%1 %2…").arg(jobVerb(j), name) : tr("%1 %2: %3.").arg(jobVerb(j), name, progress);
        if (!cantStop.isEmpty())
            text += QLatin1Char(' ') + cantStop;
        stoppable = stoppable || cantStop.isEmpty();
        const QString path = j.path;
        place(path, jobSelfErasing(j) ? NoticeBar::Level::Warning : NoticeBar::Level::Info, text,
              cantStop.isEmpty() ? jobStopButton(j) : QString(), [this, path] { stopJob(path); });
    }
    // RAID arrays checking or rebuilding: a check can be stopped, a rebuild is best left to finish.
    for (const Disk &d : m_udisks->disks()) {
        if (!d.isRaid || d.raidSync.isEmpty() || d.raidSync == QLatin1String("idle") || d.raidSync == QLatin1String("frozen"))
            continue;
        const bool checking = d.raidSync == QLatin1String("check") || d.raidSync == QLatin1String("repair");
        stoppable = stoppable || checking;
        const QString blockPath = d.blockPath;
        QString text = tr("%1 %2: %3%").arg(checking ? tr("Checking") : tr("Rebuilding"), shortDevice(d.device)).arg(int(d.raidSyncDone * 100));
        if (d.raidSyncRate)
            text += QStringLiteral(", ") + tr("%1/s").arg(formatSize(d.raidSyncRate));
        if (d.raidSyncLeftUs)
            text += QStringLiteral(", ") + tr("%1 left").arg(durationText(double(d.raidSyncLeftUs) / 1e6));
        text += QLatin1Char('.');
        if (!checking)
            text += QLatin1Char(' ') + tr("It's safest to let it finish; the array works meanwhile.");
        place(QStringLiteral("raid:") + d.blockPath, NoticeBar::Level::Info, text, checking ? tr("Stop Checking") : QString(), [this, blockPath] {
            if (const Disk *disk = m_udisks->diskByPath(blockPath))
                m_udisks->raidSyncAction(*disk, QStringLiteral("idle"));
        });
    }
    for (const Disk &d : m_udisks->disks()) {
        if (d.health.selftestStatus != QLatin1String("inprogress"))
            continue;
        stoppable = true;
        const QString name = d.model.isEmpty() ? shortDevice(d.device) : tr("%1 (%2)").arg(shortDevice(d.device), d.model);
        const QString blockPath = d.blockPath;
        place(QStringLiteral("selftest:") + d.blockPath, NoticeBar::Level::Info,
              d.health.selftestPercentRemaining >= 0 ? tr("Self-test on %1: %2% to go.").arg(name).arg(d.health.selftestPercentRemaining)
                                                     : tr("Self-test running on %1.").arg(name),
              tr("Stop Test"), [this, blockPath] {
                  if (const Disk *disk = m_udisks->diskByPath(blockPath))
                      m_udisks->smartSelftestAbort(*disk);
              });
    }
    for (auto it = m_jobBars.begin(); it != m_jobBars.end();) {
        if (seen.contains(it.key())) {
            ++it;
            continue;
        }
        it.value()->hide();
        it.value()->deleteLater();
        it = m_jobBars.erase(it);
    }
    m_stop->setEnabled(stoppable);
    if (recheck >= 0)
        m_jobRecheck->start(int(qMin<qint64>(recheck, 5000)));
}

void MainWindow::stopJob(const QString &jobPath)
{
    auto find = [this, &jobPath]() -> const Job * {
        for (const Job &j : m_udisks->jobs()) {
            if (j.path == jobPath)
                return &j;
        }
        return nullptr;
    };
    const Job *job = find();
    if (!job || !jobCantStop(*job).isEmpty())
        return;
    bool whole = false;
    const QString name = jobTarget(*job, &whole);
    const Job asked = *job; // the job list is rebuilt while the question is open
    QMessageBox box(QMessageBox::Question, tr("Stop"), jobStopQuestion(asked, name, whole), QMessageBox::NoButton, this);
    box.setTextFormat(Qt::PlainText); // names come from the drive
    QPushButton *stop = box.addButton(jobStopButton(asked), QMessageBox::DestructiveRole);
    QPushButton *keep = box.addButton(tr("Keep Going"), QMessageBox::RejectRole);
    box.setDefaultButton(keep);
    box.setEscapeButton(keep);
    box.exec();
    if (box.clickedButton() != stop)
        return;
    const Job *still = find();
    if (!still) {
        statusBar()->showMessage(tr("It had already finished."), 8000);
        return;
    }
    m_udisks->cancelJob(jobPath, jobStoppedMessage(*still, name));
}

bool MainWindow::selectDevice(const QString &device)
{
    for (int i = 0; i < m_map->disks().size(); ++i) {
        if (m_map->disks()[i].device == device) {
            m_map->selectDisk(i);
            syncTableToMap();
            updateActions();
            return true;
        }
    }
    for (const Disk &d : m_map->disks()) {
        for (const Volume &v : d.volumes) {
            if (v.device == device) {
                m_map->selectVolume(v.objectPath);
                syncTableToMap();
                updateActions();
                return true;
            }
        }
    }
    return false;
}

void MainWindow::fillTable()
{
    m_syncing = true;
    m_table->clear();
    for (const Disk &d : m_map->disks()) {
        for (const Volume &v : d.volumes) {
            if (v.isContainer)
                continue;
            auto *item = new QTreeWidgetItem(m_table);
            item->setData(ColVolume, Qt::UserRole, v.objectPath);
            item->setText(ColVolume, !v.label.isEmpty() ? v.label : !v.partName.isEmpty() ? v.partName : shortDevice(v.device));
            item->setIcon(ColVolume, d.isSystem ? themeIcon("drive-harddisk-root", "drive-harddisk")
                                     : (d.removable || d.bus == QLatin1String("usb")) ? themeIcon("drive-removable-media-usb", "drive-harddisk")
                                                                                     : themeIcon("drive-harddisk", "drive-harddisk"));
            item->setText(ColDevice, v.device);
            item->setText(ColFileSystem, v.fsType);
            item->setText(ColStatus, volumeStatus(v));
            item->setText(ColCapacity, formatSize(v.size));
            if (v.fsTotal) {
                item->setText(ColFree, formatSize(v.fsFree));
                item->setText(ColPercentFree, QStringLiteral("%1 %").arg(qRound(100.0 * double(v.fsFree) / double(v.fsTotal))));
            }
            for (int col : {ColCapacity, ColFree, ColPercentFree})
                item->setTextAlignment(col, Qt::AlignRight | Qt::AlignVCenter);
        }
    }
    for (int col = 0; col < ColumnCount; ++col) {
        if (col != ColStatus)
            m_table->resizeColumnToContents(col);
    }
    m_syncing = false;
}

void MainWindow::syncTableToMap()
{
    m_syncing = true;
    const Volume *v = selectedVolume();
    m_table->clearSelection();
    for (int i = 0; v && i < m_table->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_table->topLevelItem(i);
        if (item->data(ColVolume, Qt::UserRole).toString() == v->objectPath) {
            m_table->setCurrentItem(item);
            m_table->scrollToItem(item);
        }
    }
    m_syncing = false;
}

void MainWindow::onTableSelection()
{
    if (m_syncing)
        return;
    const QList<QTreeWidgetItem *> items = m_table->selectedItems();
    if (!items.isEmpty())
        m_map->selectVolume(items.first()->data(ColVolume, Qt::UserRole).toString());
    updateActions();
}

const Disk *MainWindow::selectedDisk() const
{
    const DiskMap::Selection sel = m_map->selection();
    return sel.kind == DiskMap::Selection::Kind::None ? nullptr : &m_map->disks()[sel.disk];
}

const Volume *MainWindow::selectedVolume() const
{
    const DiskMap::Selection sel = m_map->selection();
    return sel.kind == DiskMap::Selection::Kind::Volume ? &m_map->disks()[sel.disk].volumes[sel.volume] : nullptr;
}

void MainWindow::updateActions()
{
    for (QAction *pin : std::as_const(m_pinned)) {
        const QStringList ref = pin->data().toStringList(); // id and label; empty for the separator
        if (ref.size() != 2)
            continue;
        const QString why = addonReason(ref[0], ref[1]);
        pin->setEnabled(why.isEmpty());
        pin->setToolTip(why.isEmpty() ? QString(pin->text()).replace(QStringLiteral("&&"), QStringLiteral("&")) : why);
    }
    const Disk *d = selectedDisk();
    const Volume *v = selectedVolume();
    const auto kind = m_map->selection().kind;
    const bool mounted = v && !v->mounts().isEmpty();
    const bool busy = m_udisks->isBusy();
    // A read-only drive (a write-protected one, or an image opened read-only) can't be changed.
    const bool changeable = d && !d->isSystem && !d->readOnly && !busy;
    // LVM groups: only what works on a logical volume's own device for now (mount, check,
    // label...); changes to the group and its volumes come later.
    const bool lvm = d && d->isLvm;
    const bool tableChangeable = changeable && !lvm && !d->isDynamicVolume;
    const FsType *fs = v ? m_udisks->filesystem(v->effectiveFsType()) : nullptr;

    m_open->setEnabled(mounted);
    m_mount->setEnabled(v && v->canMount() && !mounted && !busy);
    m_unmount->setEnabled(v && v->canMount() && mounted && !v->isSystem && !busy);
    m_safelyRemove->setEnabled(d && !d->isSystem && !d->isLoop && d->canPowerOff && !busy);
    m_unlock->setEnabled(v && v->encrypted && v->cleartextPath.isEmpty() && !busy);
    m_lock->setEnabled(v && v->encrypted && !v->cleartextPath.isEmpty() && !v->isSystem && !busy);
#ifdef Q_OS_WIN
    // BitLocker's password changes while it's unlocked; Windows' own drive uses the TPM.
    m_changePass->setEnabled(changeable && v && v->encrypted && !v->cleartextPath.isEmpty() && !v->isSystem);
#else
    // UDisks changes LUKS passphrases; a BitLocker password only Windows can change.
    const bool bitLocker = v && v->fsType == QLatin1String("BitLocker");
    m_changePass->setEnabled(changeable && v && v->encrypted && !bitLocker);
#endif
    const bool luks = v && v->fsType == QLatin1String("crypto_LUKS");
    // A LUKS partition whose header is wrecked doesn't show as LUKS any more: that's when the
    // copy is needed, so it can go back on a partition nothing is recognized on too.
    const bool unknown = v && v->fsType.isEmpty() && !v->isContainer && v->number > 0;
    m_luksBackup->setEnabled(luks && !busy);
    m_luksRestore->setEnabled((luks || unknown) && changeable && !v->isSystem);
    m_newPartition->setEnabled(tableChangeable && kind == DiskMap::Selection::Kind::Free && !d->tableType.isEmpty());
    m_format->setEnabled(tableChangeable && v && !v->isContainer);
    m_rename->setEnabled(changeable && v && v->canMount());
    m_inspect->setEnabled(d && !lvm && d->size > 0 && !busy); // read-only: the system disk too
    m_recover->setEnabled(d && !lvm && d->size > 0 && !d->isSystem && !busy);
    m_raidCheck->setEnabled(d && d->isRaid && d->raidRunning && d->raidDegraded == 0 && !busy
                            && (d->raidSync.isEmpty() || d->raidSync == QLatin1String("idle")));
    m_typeFlags->setEnabled(tableChangeable && v && !v->isContainer
                            && (d->tableType == QLatin1String("gpt") || d->tableType == QLatin1String("dos")));
    m_check->setEnabled(changeable && v && v->canMount() && fs && fs->canCheck);
    m_startup->setEnabled(changeable && v && v->hasFilesystem && !v->encrypted && !v->uuid.isEmpty());
    {
        const QSignalBlocker block(m_startup);
        m_startup->setChecked(v && !v->fstab.isEmpty());
    }
    m_delete->setEnabled(tableChangeable && v && v->number > 0);
    const ResizeLimits limits = v ? m_udisks->resizeLimits(*v) : ResizeLimits{};
    m_resize->setEnabled(tableChangeable && limits.possible);
    m_newTable->setEnabled(tableChangeable);
    m_wipe->setEnabled(tableChangeable);
    m_sdCard->setEnabled(tableChangeable && (d->sdCard || d->removable || d->bus == QLatin1String("usb") || d->isLoop));
#ifdef Q_OS_WIN
    m_secureErase->setEnabled(tableChangeable && !d->isLoop); // it says what Windows can't do, and what can
#else
    m_secureErase->setEnabled(tableChangeable && !d->isLoop && (d->ataEraseMinutes > 0 || d->ataEnhancedEraseMinutes > 0 || d->nvmeNamespace));
#endif
    m_detachImage->setEnabled(d && d->isLoop && !busy);
    m_openImage->setEnabled(!busy);
    m_writeImage->setEnabled(!busy);
    m_rescueUsb->setEnabled(!busy);
    m_liveOnPc->setEnabled(!busy);
    m_windowsUsb->setEnabled(!busy);
    m_findFiles->setEnabled(!busy);
    m_findFilesImage->setEnabled(!busy);
    m_health->setEnabled(d && !d->isLoop && d->health.state != Health::State::Unknown);
    m_benchmark->setEnabled(d && !lvm && !busy);
    m_badSectors->setEnabled(d && !lvm && !d->isLoop && !busy);
    m_checkStick->setEnabled(!busy);
    m_copy->setEnabled(d && kind != DiskMap::Selection::Kind::Free);
    // Clone and Rescue read the whole drive and refuse the running system; Back Up works on
    // anything that can be unmounted (or isn't mounted); Restore writes, so not the system disk.
    m_clone->setEnabled(d && !lvm && !d->isSystem && !busy && d->size > 0);
    m_newIds->setEnabled(d && !lvm && !d->isRaid && !d->isSystem && !d->readOnly && !busy && d->size > 0);
    m_rescue->setEnabled(d && !lvm && !d->isSystem && !d->isLoop && !busy);
    const bool partition = v && !v->isContainer;
    bool anyMounted = false;
    if (d) {
        for (const Volume &x : d->volumes)
            anyMounted = anyMounted || ((!v || &x == v) && !x.mounts().isEmpty());
    }
    m_backup->setEnabled(d && !busy && kind != DiskMap::Selection::Kind::Free && (!v || partition) && !(lvm && (!v || !v->lvActive))
                         && !(d->isSystem && (!v || v->isSystem || anyMounted)));
    m_restore->setEnabled(tableChangeable && kind != DiskMap::Selection::Kind::Free && (!v || partition));
    m_backup->setText(partition ? tr("&Back Up Partition…") : tr("&Back Up Drive…"));
    m_usage->setEnabled(mounted);
    m_snapshots->setEnabled(!snapper::mountedSubvolumes().isEmpty());
    m_restore->setText(partition ? tr("R&estore Partition Backup…") : tr("R&estore Drive Backup…"));
    m_clonezilla->setEnabled(tableChangeable && partition && !v->isSystem);
    m_testBoot->setEnabled(d && !d->isRaid && !busy);
    m_evidence->setEnabled(d && kind != DiskMap::Selection::Kind::Free && !busy && !(v ? v->isSystem : d->isSystem));
    m_properties->setEnabled(d != nullptr);

    const QString why = d && d->isSystem ? tr("Disk holds the running system (%1)").arg(d->systemReason)
                      : d && d->readOnly ? (writeblock::isOn() ? tr("This drive is write-protected (Tools > Write-Protect Every Drive)")
                                                               : tr("This drive is read-only"))
                      : busy             ? tr("Wait for the current operation to finish")
                                         : QString();
    for (QAction *a : {m_unmount, m_newPartition, m_format, m_rename, m_delete, m_newTable, m_wipe, m_safelyRemove,
                       m_check, m_startup, m_lock, m_changePass, m_clone, m_restore, m_rescue, m_backup, m_secureErase})
        a->setToolTip(why.isEmpty() || a->isEnabled() ? a->text().remove(QLatin1Char('&')) : why);
    m_resize->setToolTip(m_resize->isEnabled() ? tr("Resize") : !why.isEmpty() ? why : limits.reason);
    if (d && !m_health->isEnabled())
        m_health->setToolTip(tr("This drive doesn't report health data"));
    if (v && v->encrypted && !m_startup->isEnabled() && why.isEmpty())
        m_startup->setToolTip(tr("Encrypted drives can't mount at startup yet"));
#ifndef Q_OS_WIN
    if (bitLocker && why.isEmpty())
        m_changePass->setToolTip(tr("Only Windows can change a BitLocker password"));
#endif
    // In DiskAnvil Live these would only change the system in memory, which starts fresh.
    if (rescue::runningInRescue()) {
        for (QAction *a : {m_startup, m_cleanup, m_snapshots}) {
            a->setEnabled(false);
            a->setToolTip(rescue::notInRescueReason());
            a->setStatusTip(rescue::notInRescueReason());
        }
    }
    updateProgress();
}

QWidget *MainWindow::toolParent()
{
    // Opened on its own (--open), this window stays hidden, and a dialog that belongs to a
    // hidden window gets no taskbar button: then the tool is a window of its own.
    return m_toolOnly ? nullptr : this;
}

bool MainWindow::failingDriveOk(const Volume &volume, bool repairing)
{
    const Disk *disk = m_udisks->diskOf(volume);
    if (!disk || !health::hasUnreadableSpots(disk->health))
        return true;
    // Checking reads all of it; repairing writes to it, and can drop what it can't read.
    QMessageBox box(QMessageBox::Warning, repairing ? tr("Repair a Failing Drive?") : tr("Check a Failing Drive?"),
                    repairing ? tr("%1 is on a drive with spots it can't read. Repairing writes to it, and can throw away files it can't "
                                   "read: on a failing drive that can lose them for good. Make a Rescue Copy first, and repair the copy.")
                                    .arg(volumeTitle(volume))
                              : tr("%1 is on a drive with spots it can't read. Checking reads all of it, which can make a failing drive worse. "
                                   "Make a Rescue Copy first, and check the copy.")
                                    .arg(volumeTitle(volume)),
                    QMessageBox::NoButton, this);
    QPushButton *rescue = box.addButton(tr("Make a Rescue Copy…"), QMessageBox::AcceptRole);
    QPushButton *anyway = box.addButton(repairing ? tr("Repair Anyway") : tr("Check Anyway"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(rescue);
    box.setObjectName(QStringLiteral("failingDrive"));
    box.exec();
    if (box.clickedButton() == rescue) {
        RescueDialog(m_udisks, disk->blockPath, this).exec();
        return false;
    }
    return box.clickedButton() == anyway;
}

bool MainWindow::openTool(const QString &name)
{
    const QHash<QString, QAction *> tools = {{QStringLiteral("what-happened"), m_whatHappened},
                                             {QStringLiteral("write-image"), m_writeImage},
                                             {QStringLiteral("windows-usb"), m_windowsUsb},
                                             {QStringLiteral("rescue-usb"), m_rescueUsb},
                                             {QStringLiteral("live-on-pc"), m_liveOnPc},
                                             {QStringLiteral("check-stick"), m_checkStick},
                                             {QStringLiteral("lost-files"), m_findFiles},
                                             {QStringLiteral("virus-scan"), m_virusScan},
                                             {QStringLiteral("save-files"), m_saveFiles},
                                             {QStringLiteral("remote-help"), m_remoteHelp},
                                             {QStringLiteral("net-boot"), m_netBoot},
                                             {QStringLiteral("product-key"), m_productKey},
                                             {QStringLiteral("drivers"), m_drivers}, {QStringLiteral("activity"), m_activity},
                                             {QStringLiteral("look-closer"), m_lookCloser},
                                             {QStringLiteral("boot-menu"), m_bootMenu},
                                             {QStringLiteral("linux-boot"), m_linuxBoot},
                                             {QStringLiteral("make-room"), m_winRe},
                                             {QStringLiteral("certificates"), m_certificates},
                                             {QStringLiteral("open-sets"), m_openSets}};
    QAction *action = tools.value(name);
    if (!action)
        return false;
    m_toolOnly = true;
    action->trigger();
    return true;
}

void MainWindow::updateProgress()
{
    const QVector<Job> &jobs = m_udisks->jobs();
    const bool working = m_udisks->isBusy() || !jobs.isEmpty();
    m_progress->setVisible(working);
    if (!working)
        return;
    for (const Job &j : jobs) {
        if (j.progressValid) {
            m_progress->setRange(0, 1000);
            m_progress->setValue(int(j.progress * 1000));
            m_progress->setToolTip(j.rate ? tr("%1/s").arg(formatSize(j.rate)) : QString());
            return;
        }
    }
    m_progress->setRange(0, 0); // no percentage from UDisks2: show that it's busy
}

void MainWindow::buildContextMenu(QMenu *menu)
{
    const Disk *d = selectedDisk();
    if (!d)
        return;
    const Volume *v = selectedVolume();
    const DiskMap::Selection sel = m_map->selection();
    // Only what can be done right now, so the menu reads as a list of options.
    auto add = [menu](std::initializer_list<QAction *> actions) {
        for (QAction *a : actions) {
            if (a->isEnabled())
                menu->addAction(a);
        }
    };

    if (v) {
        menu->addSection(volumeTitle(*v));
        add({m_open, m_usage, m_mount, m_unmount, m_unlock, m_lock});
        menu->addSeparator();
        add({m_format, m_resize, m_rename, m_typeFlags, m_check, m_startup, m_changePass, m_delete});
        if (v->fsType == QLatin1String("crypto_LUKS"))
            add({m_luksBackup, m_luksRestore});
        else if (m_luksRestore->isEnabled())
            add({m_luksRestore});
        menu->addSeparator();
        add({m_backup, m_restore, m_clonezilla, m_evidence});
        if (v->effectiveFsType() == QLatin1String("btrfs"))
            add({m_snapshots});
    } else if (sel.kind == DiskMap::Selection::Kind::Free) {
        menu->addSection(tr("Unallocated space, %1").arg(formatSize(sel.size)));
        add({m_newPartition});
    }

    menu->addSection(tr("Drive: %1").arg(d->model.isEmpty() ? shortDevice(d->device) : d->model));
    add({m_safelyRemove, m_detachImage, m_health, m_badSectors, m_benchmark, m_testBoot});
    if (!v)
        add({m_backup, m_restore, m_evidence});
    add({m_clone, m_newIds, m_rescue, m_findFiles});
    if (!d->isSystem && (d->removable || d->bus == QLatin1String("usb")))
        add({m_writeImage, m_windowsUsb, m_rescueUsb, m_checkStick});
    add({m_newTable, m_wipe, m_secureErase});
    if (m_sdCard->isEnabled())
        add({m_sdCard});

    const auto addonActions = m_addons.actionsFor(*d, v, sel.kind == DiskMap::Selection::Kind::Free);
    if (!addonActions.isEmpty()) {
        menu->addSection(tr("Add-ons"));
        addAddonActions(menu);
    }
    menu->addSeparator();
    add({m_copy, m_properties});
}

void MainWindow::showContextMenu(const QPoint &globalPos)
{
    QMenu menu(this);
    buildContextMenu(&menu);
    if (!menu.isEmpty())
        menu.exec(globalPos);
}

void MainWindow::activate()
{
    if (m_open->isEnabled())
        openInFileManager();
    else if (m_properties->isEnabled())
        showProperties();
}

void MainWindow::openInFileManager()
{
    if (const Volume *v = selectedVolume()) {
        const QString mp = v->mounts().value(0);
        if (!mp.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(mp));
    }
}

void MainWindow::copyDevicePath()
{
    const Volume *v = selectedVolume();
    const Disk *d = selectedDisk();
    if (v || d)
        QApplication::clipboard()->setText(v ? v->device : d->device);
}

void MainWindow::showProperties()
{
    const Disk *d = selectedDisk();
    if (!d)
        return;
    const DiskMap::Selection sel = m_map->selection();
    const Volume *v = selectedVolume();

    QList<QPair<QString, QString>> rows;
    auto add = [&rows](const QString &name, const QString &value) {
        if (!value.isEmpty())
            rows.append({name, value});
    };
    auto bytes = [](quint64 n) { return QStringLiteral("%1 (%2 bytes)").arg(formatSize(n)).arg(n); };

    QString title;
    if (v) {
        title = volumeTitle(*v);
        add(tr("Device"), v->device);
        add(tr("Label"), v->label);
        add(tr("Partition name"), v->partName);
        add(tr("File system"), v->fsType);
        add(tr("UUID"), v->uuid);
        add(tr("Partition ID"), v->partUuid);
        add(tr("Status"), volumeStatus(*v));
        add(tr("Capacity"), bytes(v->size));
        if (v->fsTotal)
            add(tr("Free space"), tr("%1 of %2").arg(formatSize(v->fsFree), formatSize(v->fsTotal)));
        if (v->number)
            add(tr("Partition number"), QString::number(v->number));
        add(tr("Partition type"), partitionTypeName(v->partType));
        add(tr("Starts at"), bytes(v->offset));
        if (!v->fstab.isEmpty())
            add(tr("Mounts at startup"), QString::fromLocal8Bit(v->fstab.value(QStringLiteral("dir")).toByteArray()).remove(QChar(0)));
        const bool veracrypt = v->fsType == QLatin1String("crypto_unknown") || v->fsType == QLatin1String("crypto_TCRYPT");
        if (v->encrypted) {
            const QString kind = v->fsType == QLatin1String("BitLocker") ? tr("BitLocker") : veracrypt ? tr("VeraCrypt") : tr("LUKS");
            add(tr("Encryption"), v->cleartextPath.isEmpty() ? tr("%1, locked").arg(kind) : tr("%1, unlocked (%2 inside)").arg(kind, v->cleartextFsType));
        }
        if (v->fsType == QLatin1String("crypto_unknown"))
            add(tr("Contents"), tr("Nothing recognizable, which is how a VeraCrypt volume looks (or an empty or damaged one): "
                                   "Unlock tries it as VeraCrypt"));
        else if (v->fsType.isEmpty() && !v->isContainer && v->number > 0)
            add(tr("Contents"), tr("Nothing recognizable: empty, damaged, or encrypted (VeraCrypt looks like this)"));
        add(tr("Disk"), d->device);
#ifdef Q_OS_WIN
        add(tr("Windows path"), v->objectPath);
#else
        add(tr("UDisks2 object"), v->objectPath);
#endif
    } else if (sel.kind == DiskMap::Selection::Kind::Free) {
        title = tr("Unallocated space on %1").arg(shortDevice(d->device));
        add(tr("Disk"), d->device);
        add(tr("Size"), bytes(sel.size));
        add(tr("Starts at"), bytes(sel.offset));
    } else {
        title = d->model.isEmpty() ? d->device : d->model;
        add(tr("Device"), d->device);
        add(tr("Model"), d->model);
        add(tr("Serial"), d->serial);
        add(tr("Type"), diskKind(*d));
        add(tr("Partition table"), tableName(*d));
        add(tr("Disk ID"), d->tableId);
        add(tr("Capacity"), bytes(d->size));
        if (d->rotationRate > 1) // 1: it spins, but the drive didn't say how fast
            add(tr("Rotation rate"), tr("%1 rpm").arg(d->rotationRate));
        add(tr("Image file"), QDir::toNativeSeparators(d->backingFile));
        add(tr("Removable"), d->removable ? tr("Yes") : tr("No"));
        if (d->health.state != Health::State::Unknown) {
            add(tr("Health"), d->health.summary);
            if (d->health.powerOnHours > 0)
                add(tr("Powered on"), tr("%L1 hours").arg(d->health.powerOnHours));
        }
        if (d->health.temperatureC > 0)
            add(tr("Temperature"), QStringLiteral("%1 °C").arg(qRound(d->health.temperatureC)));
        if (d->isSystem)
            add(tr("System disk"), tr("%1. This app won't modify it.").arg(d->systemReason));
        if (!d->nvmeNamespace && !d->isLoop)
            add(tr("Sector size"), tr("%1 bytes").arg(d->sectorSize));
        if (d->bus == QLatin1String("usb"))
            add(tr("USB connection"), usbinfo::describe(usbinfo::of(d->device)));
        if (d->nvmeNamespace) {
            if (d->nvmeSectorSize > 0) {
                QStringList others;
                for (int size : d->nvmeSectorSizes) {
                    if (size != d->nvmeSectorSize)
                        others << QString::number(size);
                }
                add(tr("Sector size"), others.isEmpty() ? tr("%1 bytes").arg(d->nvmeSectorSize)
                                                        : tr("%1 bytes (it can also do %2; Secure Erase can change it)")
                                                              .arg(d->nvmeSectorSize)
                                                              .arg(others.join(QStringLiteral(", "))));
            }
            const QString &sanitized = d->nvmeSanitizeStatus;
            add(tr("Sanitize"), sanitized == QLatin1String("never_sanitized") ? tr("Never done")
                                : sanitized == QLatin1String("success")       ? tr("The last one finished")
                                : sanitized == QLatin1String("inprogress")
                                    ? (d->nvmeSanitizePercent >= 0 ? tr("Running, %1% left").arg(d->nvmeSanitizePercent) : tr("Running"))
                                : sanitized == QLatin1String("failure") ? tr("The last one failed")
                                                                        : sanitized);
            if (d->nvmeUnallocated > 0)
                add(tr("Not in any namespace"), bytes(d->nvmeUnallocated));
        }
#ifdef Q_OS_WIN
        add(tr("Windows path"), d->blockPath);
#else
        add(tr("UDisks2 object"), d->blockPath);
#endif
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("%1 Properties").arg(title));
    auto *tree = new QTreeWidget;
    tree->setColumnCount(2);
    tree->setHeaderLabels({tr("Property"), tr("Value")});
    tree->setRootIsDecorated(false);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const auto &[name, value] : rows)
        new QTreeWidgetItem(tree, {name, value});
    tree->resizeColumnToContents(0);
    tree->resizeColumnToContents(1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(tree);
#ifndef Q_OS_WIN
    // UDisks only looks for VeraCrypt when /etc/udisks2/tcrypt.conf is there as it starts
    // (DiskAnvil Live has it). On a partition nothing is recognized on, that can be switched on.
    if (v && v->fsType.isEmpty() && !v->isContainer && v->number > 0
        && host::run({QStringLiteral("test"), QStringLiteral("-e"), QStringLiteral("/etc/udisks2/tcrypt.conf")}) != 0) {
        auto *look = new QPushButton(tr("Look for VeraCrypt…"));
        look->setObjectName(QStringLiteral("lookForVeraCrypt"));
        connect(look, &QPushButton::clicked, &dialog, [&dialog] {
            QMessageBox box(QMessageBox::Question, tr("Look for VeraCrypt"),
                            tr("VeraCrypt volumes look like random data, so UDisks (which DiskAnvil unlocks drives through) only "
                               "tries partitions like this one as VeraCrypt when the file /etc/udisks2/tcrypt.conf exists. Make it? "
                               "It takes the admin password, and UDisks restarts, which takes a moment.\n\nThen this partition "
                               "shows as \"Unknown, maybe VeraCrypt\", and Unlock asks for VeraCrypt's password."),
                            QMessageBox::Cancel, &dialog);
            QAbstractButton *on = box.addButton(tr("Switch It On"), QMessageBox::AcceptRole);
            box.exec();
            if (box.clickedButton() != on)
                return;
            const QStringList command = host::command(
                {QStringLiteral("pkexec"), QStringLiteral("sh"), QStringLiteral("-c"),
                 QStringLiteral("printf '%s\\n' '# Made by DiskAnvil: UDisks looks for VeraCrypt and TrueCrypt volumes while this file is here.' "
                                "> /etc/udisks2/tcrypt.conf && systemctl restart udisks2.service")});
            QProcess run;
            run.setProcessChannelMode(QProcess::MergedChannels);
            run.start(command.first(), command.mid(1));
            const bool ok = run.waitForFinished(180000) && run.exitStatus() == QProcess::NormalExit && run.exitCode() == 0;
            qCInfo(lcOps).noquote() << "tcrypt.conf for VeraCrypt" << (ok ? "made" : "not made");
            if (ok)
                QMessageBox::information(&dialog, tr("Look for VeraCrypt"), tr("Done: UDisks looks for VeraCrypt now."));
            else
                QMessageBox::warning(&dialog, tr("Look for VeraCrypt"),
                                     tr("It wasn't switched on: %1").arg(QString::fromLocal8Bit(run.readAll()).trimmed()));
        });
        layout->addWidget(look, 0, Qt::AlignRight);
    }
#endif
    // The hidden area (HPA): the end of an ATA drive its own settings hide, where malware and
    // some makers' recovery tools keep things. Only hdparm sees it, so only DiskAnvil Live.
    // hdparm asks the drive itself, whatever UDisks made of it; USB bridges rarely pass this on.
    const bool ataDrive = !v && sel.kind != DiskMap::Selection::Kind::Free && !d->isLoop && !d->nvmeNamespace && !d->isRaid && !d->isLvm
                       && d->bus != QLatin1String("usb") && d->device.startsWith(QLatin1String("/dev/sd"));
    if (ataDrive) {
        drivepower::HiddenArea hidden;
        if (rescue::runningInRescue()) {
            QProcess hdparm;
            hdparm.setProcessChannelMode(QProcess::MergedChannels);
            hdparm.start(QStringLiteral("sudo"), {QStringLiteral("-n"), QStringLiteral("hdparm"), QStringLiteral("-N"), d->device});
            if (hdparm.waitForFinished(10000))
                hidden = drivepower::parseHdparmN(QString::fromLocal8Bit(hdparm.readAll()));
        }
        auto *row = new QTreeWidgetItem(tree, {tr("Hidden area (HPA)"),
                                               !rescue::runningInRescue() ? tr("Only DiskAnvil Live can look")
                                               : !hidden.known            ? tr("The drive didn't say")
                                               : hidden.hidden() == 0     ? tr("None")
                                                                          : tr("%1 hidden by the drive's own settings").arg(formatSize(hidden.hidden() * 512))});
        Q_UNUSED(row)
        // The sizes the drive itself says it can switch to (only hdparm asks, so only DiskAnvil Live).
        if (rescue::runningInRescue()) {
            QProcess hdparm;
            hdparm.setProcessChannelMode(QProcess::MergedChannels);
            hdparm.start(QStringLiteral("sudo"), {QStringLiteral("-n"), QStringLiteral("hdparm"), QStringLiteral("-I"), d->device});
            const drivepower::SectorSizes sizes =
                hdparm.waitForFinished(10000) ? drivepower::parseHdparmI(QString::fromLocal8Bit(hdparm.readAll())) : drivepower::SectorSizes();
            if (sizes.logical > 0) {
                QStringList others;
                for (int size : sizes.supported) {
                    if (size != sizes.logical)
                        others << QString::number(size);
                }
                new QTreeWidgetItem(tree, {tr("Sector size (the drive's)"),
                                           others.isEmpty() ? tr("%1 bytes (%2 inside)").arg(sizes.logical).arg(sizes.physical)
                                                            : tr("%1 bytes (%2 inside); it can be switched to %3")
                                                                  .arg(sizes.logical)
                                                                  .arg(sizes.physical)
                                                                  .arg(others.join(QStringLiteral(", ")))});
            }
            if (sizes.canChange() && !d->isSystem) {
                auto *change = new QPushButton(tr("Change the Sector Size…"));
                const QString path = d->blockPath;
                connect(change, &QPushButton::clicked, &dialog, [this, &dialog, path, sizes] {
                    if (const Disk *fresh = m_udisks->diskByPath(path))
                        SectorSizeDialog(m_udisks, *fresh, sizes, &dialog).exec();
                });
                layout->addWidget(change, 0, Qt::AlignRight);
            }
        }
        if (hidden.known && hidden.hidden() > 0) {
            auto *show = new QPushButton(tr("Show the Hidden Area…"));
            show->setToolTip(tr("Makes the drive show all of itself again. Nothing on the part you see now changes."));
            const QString device = d->device;
            const quint64 native = hidden.native;
            connect(show, &QPushButton::clicked, &dialog, [this, &dialog, device, native] {
                if (QMessageBox::question(&dialog, dialog.windowTitle(),
                                          tr("Make %1 show all of itself again? What's in the hidden part (a maker's recovery copy, or "
                                             "something hiding) becomes part of the drive. Nothing on the part you see now changes; "
                                             "some drives only show the rest after being switched off and on again.")
                                              .arg(shortDevice(device)),
                                          QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
                    != QMessageBox::Yes)
                    return;
                QProcess hdparm;
                hdparm.setProcessChannelMode(QProcess::MergedChannels);
                hdparm.start(QStringLiteral("sudo"), {QStringLiteral("-n"), QStringLiteral("hdparm"), QStringLiteral("--yes-i-know-what-i-am-doing"),
                                                      QStringLiteral("-N"), QStringLiteral("p%1").arg(native), device});
                const bool ok = hdparm.waitForFinished(30000) && hdparm.exitStatus() == QProcess::NormalExit && hdparm.exitCode() == 0;
                qCInfo(lcOps).noquote() << "hdparm -N p" << native << device << (ok ? "done" : "failed");
                QMessageBox::information(&dialog, dialog.windowTitle(),
                                         ok ? tr("Done. The drive shows all of itself now (if not yet, switch the PC off and on).")
                                            : tr("The drive wouldn't: %1").arg(QString::fromLocal8Bit(hdparm.readAll()).trimmed()));
                m_udisks->rescan(m_udisks->diskByPath(device) ? m_udisks->diskByPath(device)->blockPath : QString());
            });
            layout->addWidget(show, 0, Qt::AlignRight);
        }
    }
    // Hard drives: spin-down, power saving and the write cache.
    const bool power = !v && sel.kind != DiskMap::Selection::Kind::Free && PowerBox::applies(*d);
    if (power)
        layout->addWidget(new PowerBox(m_udisks, *d));
    layout->addWidget(buttons);
    dialog.resize(power ? 620 : 560, power ? 640 : 380);
    dialog.exec();
}

int MainWindow::selectedDiskNumber() const
{
    return m_map->selection().disk;
}

void MainWindow::newPartition()
{
    const Disk *d = selectedDisk();
    const DiskMap::Selection sel = m_map->selection();
    if (!d || sel.kind != DiskMap::Selection::Kind::Free)
        return;
    const QString path = d->blockPath;
    NewPartitionDialog dialog(*d, Span{-1, sel.offset, sel.size}, m_udisks->filesystems(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const Disk *fresh = m_udisks->diskByPath(path);
    if (!fresh)
        return gone();
    statusBar()->showMessage(tr("Creating a partition on %1…").arg(shortDevice(fresh->device)));
    m_udisks->createPartition(*fresh, sel.offset, dialog.sizeBytes(), dialog.fsType(), dialog.label(), dialog.passphrase());
    updateActions();
}

void MainWindow::formatVolume()
{
    const Disk *d = selectedDisk();
    const Volume *v = selectedVolume();
    if (!d || !v)
        return;
    const QString path = v->objectPath;
    const QString device = shortDevice(v->device);
    const QString what = describeVolume(*v);
    FormatDialog dialog(*d, *v, m_udisks->filesystems(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    QMessageBox confirm(QMessageBox::Warning, tr("Format %1").arg(device),
                        tr("Formatting will erase ALL data on %1. This can't be undone.\n\nFormat it as %2?")
                            .arg(what, m_udisks->filesystem(dialog.fsType()) ? m_udisks->filesystem(dialog.fsType())->name : dialog.fsType()),
                        QMessageBox::Cancel, this);
    QAbstractButton *format = confirm.addButton(tr("Format"), QMessageBox::DestructiveRole);
    confirm.setDefaultButton(QMessageBox::Cancel);
    confirm.exec();
    if (confirm.clickedButton() != format)
        return;

    const Volume *fresh = volumeByPath(path);
    if (!fresh)
        return gone();
    statusBar()->showMessage(tr("Formatting %1…").arg(device));
    m_udisks->format(*fresh, dialog.fsType(), dialog.label(), dialog.passphrase());
    updateActions();
}

void MainWindow::deletePartition()
{
    const Disk *d = selectedDisk();
    const Volume *v = selectedVolume();
    if (!d || !v || !m_delete->isEnabled())
        return;
    QString text = tr("Delete %1?\n\nEverything on it will be lost. This can't be undone.").arg(describeVolume(*v));
    if (!diskWarning(*d).isEmpty())
        text += QStringLiteral("\n\n") + diskWarning(*d);
    const QString path = v->objectPath;
    QMessageBox confirm(QMessageBox::Warning, tr("Delete Partition"), text, QMessageBox::Cancel, this);
    QAbstractButton *del = confirm.addButton(tr("Delete"), QMessageBox::DestructiveRole);
    confirm.setDefaultButton(QMessageBox::Cancel);
    confirm.exec();
    if (confirm.clickedButton() != del)
        return;
    const Volume *fresh = volumeByPath(path);
    if (!fresh)
        return gone();
    statusBar()->showMessage(tr("Deleting %1…").arg(shortDevice(fresh->device)));
    m_udisks->deletePartition(*fresh);
    updateActions();
}

void MainWindow::changeLabel()
{
    const Volume *v = selectedVolume();
    if (!v || !m_rename->isEnabled())
        return;
    int maxLength = 255;
    for (const FsType &fs : m_udisks->filesystems()) {
        if (fs.id == v->fsType)
            maxLength = fs.maxLabel;
    }
    const QString path = v->objectPath;
    const QString oldLabel = v->label;
    bool ok = false;
    QInputDialog input(this);
    input.setWindowTitle(tr("Change Label"));
    input.setLabelText(tr("New label for %1 (up to %2 characters):").arg(shortDevice(v->device)).arg(maxLength));
    input.setTextValue(v->label);
    if (auto *edit = input.findChild<QLineEdit *>())
        edit->setMaxLength(maxLength);
    ok = input.exec() == QDialog::Accepted;
    const QString label = input.textValue().trimmed();
    if (!ok || label == oldLabel)
        return;
    const Volume *fresh = volumeByPath(path);
    if (!fresh)
        return gone();
    statusBar()->showMessage(tr("Renaming %1…").arg(shortDevice(fresh->device)));
    m_udisks->setLabel(*fresh, label);
    updateActions();
}

void MainWindow::newPartitionTable()
{
    const Disk *d = selectedDisk();
    if (!d || !m_newTable->isEnabled())
        return;
    const QString path = d->blockPath;
    PartitionTableDialog dialog(*d, selectedDiskNumber(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const Disk *fresh = m_udisks->diskByPath(path);
    if (!fresh)
        return gone();
    statusBar()->showMessage(tr("Writing a new partition table to %1…").arg(shortDevice(fresh->device)));
    m_udisks->createPartitionTable(*fresh, dialog.tableType());
    updateActions();
}

void MainWindow::backupLuksHeader()
{
    const Volume *v = selectedVolume();
    const Disk *d = selectedDisk();
    if (!v || !d || !m_luksBackup->isEnabled())
        return;
    const QString suggested = QDir::home().filePath(QStringLiteral("%1-luks-header.img").arg(v->label.isEmpty() ? v->uuid.left(8) : v->label));
    const QString file = QFileDialog::getSaveFileName(this, tr("Back Up Encryption Header"), suggested, tr("Header backups (*.img);;All files (*)"));
    if (file.isEmpty())
        return;
    if (blockio::pathIsOnDisk(QFileInfo(file).absolutePath(), d->device)) {
        QMessageBox::warning(this, tr("Back Up Encryption Header"),
                             tr("The copy has to go on another drive: if this one is damaged, a copy on it is lost with it."));
        return;
    }
    const QString path = v->objectPath, name = shortDevice(v->device);
    auto opened = std::make_shared<QMetaObject::Connection>();
    *opened = connect(m_udisks, &UDisks::deviceOpened, this, [this, opened, path, name, file](const QString &which, int fd) {
        if (which != path)
            return;
        disconnect(*opened);
        if (fd < 0)
            return; // UDisks said why
        luksheader::Info info;
        const QByteArray header = luksheader::read(fd, &info);
        blockio::close(fd);
        QSaveFile out(file);
        if (header.isEmpty() || !out.open(QIODevice::WriteOnly) || out.write(header) != header.size() || !out.commit()) {
            QMessageBox::warning(this, tr("Back Up Encryption Header"),
                                 tr("The header of %1 couldn't be copied: %2").arg(name, header.isEmpty() ? info.problem : out.errorString()));
            return;
        }
        QFile::setPermissions(file, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        qCInfo(lcOps).noquote() << "LUKS header of" << name << "copied to" << file << header.size() << "bytes";
        QMessageBox::information(this, tr("Back Up Encryption Header"),
                                 tr("The header of %1 (LUKS%2, %3) is copied to %4.\n\nKeep it safe and private: with it and any "
                                    "passphrase the drive had when it was copied, the drive can be opened, even after that passphrase "
                                    "is changed or removed. Make a new copy after changing passphrases.")
                                     .arg(name)
                                     .arg(info.version)
                                     .arg(formatSize(quint64(header.size())), QDir::toNativeSeparators(file)));
    });
    // Read-only and for a moment: it works while the drive is unlocked and in use.
    m_udisks->openBlock(path, UDisks::OpenMode::Benchmark);
}

void MainWindow::restoreLuksHeader()
{
    const Volume *v = selectedVolume();
    if (!v || !m_luksRestore->isEnabled())
        return;
    const QString file = QFileDialog::getOpenFileName(this, tr("Put Back Encryption Header"), QDir::homePath(),
                                                      tr("Header backups (*.img);;All files (*)"));
    if (file.isEmpty())
        return;
    QFile in(file);
    QByteArray backup;
    if (in.open(QIODevice::ReadOnly) && in.size() <= 64 * 1024 * 1024)
        backup = in.readAll();
    const luksheader::Info info = luksheader::inspect(backup.left(4 * 1024 * 1024 + 4096));
    if (!info.valid() || quint64(backup.size()) != info.size) {
        QMessageBox::warning(this, tr("Put Back Encryption Header"),
                             tr("%1 isn't a whole LUKS header copy: %2").arg(QFileInfo(file).fileName(), info.problem.isEmpty() ? tr("its size is wrong") : info.problem));
        return;
    }
    // What's on the drive now, to say whether the copy is its own.
    const QString path = v->objectPath, name = shortDevice(v->device);
    auto opened = std::make_shared<QMetaObject::Connection>();
    *opened = connect(m_udisks, &UDisks::deviceOpened, this, [this, opened, path, name, backup, info](const QString &which, int fd) {
        if (which != path)
            return;
        disconnect(*opened);
        if (fd < 0)
            return;
        luksheader::Info now;
        luksheader::read(fd, &now);
        blockio::close(fd);
        QString text = tr("<p>Put the copy of the header (LUKS%1, ID %2) back on <b>%3</b>?</p>").arg(info.version).arg(info.uuid, name.toHtmlEscaped());
        if (!now.valid()) {
            text += tr("<p>Its header is damaged or gone now (%1): putting a copy back is what it's for.</p>").arg(now.problem.toHtmlEscaped());
        } else if (now.uuid != info.uuid) {
            text += redText(tr("This copy is from another drive: its ID is %1, and this drive's is %2. Putting it back makes this drive "
                               "unreadable, unless the drive's data really belongs with that header.")
                                .arg(info.uuid, now.uuid));
        } else if (now.dataOffset != info.dataOffset && now.dataOffset > 0 && info.dataOffset > 0) {
            QMessageBox::warning(this, tr("Put Back Encryption Header"), tr("This copy doesn't fit %1: its data starts somewhere else.").arg(name));
            return;
        } else {
            text += tr("<p>It's this drive's own header. Its passphrases go back to the ones it had when the copy was made.</p>");
        }
        text += tr("<p>The drive is locked first.</p>");
        QDialog confirm(this);
        confirm.setWindowTitle(tr("Put Back Encryption Header"));
        auto *layout = new QVBoxLayout(&confirm);
        layout->addWidget(wrappingLabel(text));
        layout->addWidget(new QLabel(tr("Type <b>%1</b> to confirm:").arg(name)));
        auto *typed = new QLineEdit;
        typed->setObjectName(QStringLiteral("confirm"));
        layout->addWidget(typed);
        QPushButton *ok;
        layout->addWidget(dialogButtons(&confirm, tr("Put It Back"), &ok));
        ok->setEnabled(false);
        connect(typed, &QLineEdit::textChanged, &confirm, [ok, name](const QString &t) { ok->setEnabled(t.trimmed() == name); });
        if (confirm.exec() != QDialog::Accepted)
            return;
        auto writing = std::make_shared<QMetaObject::Connection>();
        *writing = connect(m_udisks, &UDisks::deviceOpened, this, [this, writing, path, name, backup](const QString &which, int fd) {
            if (which != path)
                return;
            disconnect(*writing);
            if (fd < 0)
                return;
            QString error;
            const bool ok = luksheader::write(fd, backup, &error);
            blockio::close(fd);
            qCInfo(lcOps).noquote() << "LUKS header of" << name << (ok ? "put back" : "not put back: " + error);
            m_udisks->rescan(path);
            if (ok)
                QMessageBox::information(this, tr("Put Back Encryption Header"),
                                         tr("The header is back on %1. Unlock it with a passphrase it had when the copy was made.").arg(name));
            else
                QMessageBox::warning(this, tr("Put Back Encryption Header"), tr("It couldn't be put back on %1: %2").arg(name, error));
        });
        m_udisks->openBlock(path, UDisks::OpenMode::ReadWrite); // locks it first
    });
    m_udisks->openBlock(path, UDisks::OpenMode::Benchmark);
}

void MainWindow::resizeVolume()
{
    const Disk *d = selectedDisk();
    const Volume *v = selectedVolume();
    if (!d || !v || !m_resize->isEnabled())
        return;
    const QString path = v->objectPath;
    statusBar()->showMessage(tr("Working out how far %1 can shrink and grow…").arg(shortDevice(v->device)));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_udisks->resizeLimitsThen(*v, this, [this, path](const ResizeLimits &limits) {
        QApplication::restoreOverrideCursor();
        statusBar()->clearMessage();
        const Volume *v = volumeByPath(path);
        const Disk *d = v ? m_udisks->diskOf(*v) : nullptr;
        if (!v || !d)
            return gone();
        ResizeDialog dialog(*d, *v, limits, m_udisks->resizeNeedsRemount(*v, true), m_udisks->resizeNeedsRemount(*v, false), this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        const Volume *fresh = volumeByPath(path);
        if (!fresh)
            return gone();
        statusBar()->showMessage(tr("Resizing %1…").arg(shortDevice(fresh->device)));
        m_udisks->resize(*fresh, dialog.newSize());
        updateActions();
    });
}

void MainWindow::checkForUpdates()
{
    if (!m_updates) {
        m_updates = new UpdateChecker(this);
        connect(m_updates, &UpdateChecker::finished, this, [this](const QString &latest, const QString &url, const QString &error) {
            statusBar()->clearMessage();
            const QString current = QApplication::applicationVersion();
            if (!error.isEmpty()) {
                QMessageBox::warning(this, tr("Check for Updates"), tr("Couldn't check for updates:\n%1").arg(error));
                return;
            }
            if (!isNewerVersion(latest, current)) {
                QMessageBox::information(this, tr("Check for Updates"), tr("You have the latest version (%1).").arg(current));
                return;
            }
            QMessageBox box(QMessageBox::Information, tr("Update Available"),
                            tr("DiskAnvil %1 is available. You have %2.").arg(latest, current), QMessageBox::Close, this);
            box.setInformativeText(tr("It installs over this version, so there's no need to uninstall first.\n\n"
                                      "Installed from the AUR:\n    yay -Syu\n\n"
                                      "Installed from the GitHub repo:\n    cd diskanvil && git pull\n    cd packaging/arch && makepkg -si"));
            QAbstractButton *open = box.addButton(tr("Open Release Page"), QMessageBox::ActionRole);
            box.exec();
            if (box.clickedButton() == open && !url.isEmpty())
                QDesktopServices::openUrl(QUrl(url));
        });
    }
    statusBar()->showMessage(tr("Checking for updates…"));
    m_updates->check();
}

bool MainWindow::addAddonActions(QMenu *menu)
{
    const Disk *d = selectedDisk();
    if (!d)
        return false;
    const auto kind = m_map->selection().kind;
    const auto actions = m_addons.actionsFor(*d, selectedVolume(), kind == DiskMap::Selection::Kind::Free);
    for (const auto &[addon, action] : actions) {
        // By id and label: the add-on list may be reloaded before the item is clicked.
        const QString id = addon->id, label = action->label;
        // "&&" so an "&" in the label shows as one instead of making a shortcut letter.
        menu->addAction(QIcon::fromTheme(action->icon, QIcon::fromTheme(QStringLiteral("application-x-addon"))),
                        QString(label).replace(QLatin1Char('&'), QStringLiteral("&&")), this, [this, id, label] { runAddon(id, label); });
    }
    return !actions.isEmpty();
}

void MainWindow::runAddon(const QString &addonId, const QString &label)
{
    // Copies: the form and the questions run their own event loops, and the list can be reloaded.
    for (const Addon &a : m_addons.all()) {
        for (const AddonAction &act : a.actions) {
            if (a.id == addonId && act.label == label) {
                const Addon addon = a;
                const AddonAction action = act;
                runAddonWith(addon, action, false);
                return;
            }
        }
    }
}

void MainWindow::runAddonWith(const Addon &addon, const AddonAction &action, bool test)
{
    // From the Add-on Maker, the Maker is the window on top; its children aren't blocked by it.
    QWidget *owner = QApplication::activeModalWidget() ? QApplication::activeModalWidget() : this;
    const Disk *d = selectedDisk();
    if (!d) {
        warnPlain(owner, action.label, tr("Pick a drive or partition in the main window first."));
        return;
    }
    if (test) {
        const QString why = Addons::whyNot(action, *d, selectedVolume(), m_map->selection().kind == DiskMap::Selection::Kind::Free);
        if (!why.isEmpty()) {
            warnPlain(owner, action.label, tr("It wouldn't be offered for what's selected in the main window: %1").arg(why));
            return;
        }
    }
    // Paths, not pointers: a refresh while the form is open rebuilds the disk list.
    const QString diskPath = d->blockPath;
    const QString volumePath = selectedVolume() ? selectedVolume()->objectPath : QString();

    QMap<QString, QString> answers;
    if (!action.ask.isEmpty()) {
        QMap<QString, QString> defaults;
        for (const AddonField &f : std::as_const(action.ask))
            defaults.insert(f.id, Addons::fieldDefault(f, *d, selectedVolume()));
        AddonFormDialog form(action.label, tr("\"%1\" from the add-on \"%2\" needs a few things first:").arg(action.label, addon.name),
                             action.ask, defaults, tr("Continue"), owner);
        if (form.exec() != QDialog::Accepted)
            return;
        answers = form.values();
    }
    d = m_udisks->diskByPath(diskPath);
    const Volume *v = volumePath.isEmpty() ? nullptr : volumeByPath(volumePath);
    if (!d || (!volumePath.isEmpty() && !v)) {
        warnPlain(owner, action.label, tr("The drive isn't there anymore."));
        return;
    }
    QString error;
    const QStringList argv = Addons::fillCommand(addon, action, *d, v, answers, &error);
    if (argv.isEmpty()) {
        warnPlain(owner, action.label, error);
        return;
    }
    const QString confirmText = action.confirm.isEmpty() ? QString() : Addons::expandText(action.confirm, *d, v, &error);
    if (!action.confirm.isEmpty() && confirmText.isEmpty()) {
        warnPlain(owner, action.label, error);
        return;
    }
    // A test run always asks and never remembers anything.
    if (test || !Addons::isTrusted(addon, action, argv)) {
        bool remember = false;
        if (!askRunAddon(owner, addon, action, argv, test ? nullptr : &remember))
            return;
        if (remember) {
            Addons::trust(addon, action);
            m_addons.accept(addon.id, addon.fileHash); // they've seen it and said yes, so it's theirs now
        }
    }
    if (!confirmText.isEmpty() && !askPlain(owner, action.label, confirmText))
        return;
    if (action.window) {
        if (!Addons::prepare(action, argv, &error)) {
            warnPlain(owner, action.label, error);
            return;
        }
        auto *window = new AddonOutputWindow(action.label, Addons::processCommand(action, argv), owner);
        window->show();
        return;
    }
    if (!Addons::run(action, argv, &error))
        warnPlain(owner, action.label, error);
    else
        statusBar()->showMessage(tr("Started %1").arg(action.label), 6000);
}

QString MainWindow::addonReason(const QString &addonId, const QString &label) const
{
    for (const Addon &a : m_addons.all()) {
        for (const AddonAction &act : a.actions) {
            if (a.id != addonId || act.label != label)
                continue;
            const Disk *d = selectedDisk();
            if (!d)
                return tr("Pick a drive or partition first");
            return Addons::whyNot(act, *d, selectedVolume(), m_map->selection().kind == DiskMap::Selection::Kind::Free);
        }
    }
    return tr("This add-on isn't installed anymore");
}

QList<QKeySequence> MainWindow::takenShortcuts() const
{
    QList<QKeySequence> taken;
    for (const QAction *a : findChildren<QAction *>()) {
        if (!m_pinned.contains(a))
            taken += a->shortcuts();
    }
    return taken;
}

void MainWindow::refreshAddons()
{
    // A theme add-on may have been removed or changed.
    if (Theme::instance().currentId() != QLatin1String("system"))
        Theme::instance().restore(m_addons);
    // Pins and shortcuts refer to add-on actions by id and label; build them again.
    qDeleteAll(m_pinned);
    m_pinned.clear();
    qDeleteAll(m_shortcuts);
    m_shortcuts.clear();
    for (const Addon &a : m_addons.all()) {
        if (!a.enabled || !a.error.isEmpty())
            continue;
        for (const AddonAction &act : a.actions) {
            const QString key = Addons::actionKey(a, act), id = a.id, label = act.label;
            if (Addons::isPinned(key)) {
                if (m_pinned.isEmpty())
                    m_pinned << m_toolbar->addSeparator();
                auto *pin = new QAction(QIcon::fromTheme(act.icon, QIcon::fromTheme(QStringLiteral("application-x-addon"))),
                                        QString(label).replace(QLatin1Char('&'), QStringLiteral("&&")), this);
                pin->setData(QStringList{id, label});
                connect(pin, &QAction::triggered, this, [this, id, label] { runAddon(id, label); });
                m_toolbar->addAction(pin);
                m_pinned << pin;
            }
            const QString keys = Addons::shortcut(key);
            if (keys.isEmpty())
                continue;
            // Always enabled, so pressing it when the action doesn't fit can say why.
            auto *shortcut = new QShortcut(QKeySequence(keys, QKeySequence::PortableText), this);
            connect(shortcut, &QShortcut::activated, this, [this, id, label] {
                const QString why = addonReason(id, label);
                if (why.isEmpty())
                    runAddon(id, label);
                else
                    statusBar()->showMessage(tr("%1: %2").arg(label, why), 6000);
            });
            m_shortcuts << shortcut;
        }
    }
    updateActions();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Add-on commands shown in a window belong to DiskAnvil; ask before ending them.
    QList<AddonOutputWindow *> running;
    for (AddonOutputWindow *w : findChildren<AddonOutputWindow *>()) {
        if (w->isRunning())
            running << w;
    }
    if (!running.isEmpty()
        && !askPlain(this, tr("Quit"), tr("%n add-on command(s) still running. Stop them and quit?", nullptr, int(running.size())))) {
        event->ignore();
        return;
    }
    for (AddonOutputWindow *w : std::as_const(running))
        w->stop();
    event->accept();
}

// Dialogs run their own event loop, and a UDisks refresh meanwhile replaces the disk
// list. Handlers keep the object path across a dialog and look the volume up again.
const Volume *MainWindow::volumeByPath(const QString &objectPath) const
{
    for (const Disk &d : m_udisks->disks()) {
        for (const Volume &v : d.volumes) {
            if (v.objectPath == objectPath)
                return &v;
        }
    }
    return nullptr;
}

void MainWindow::gone()
{
    QMessageBox::warning(this, windowTitle(), tr("That partition or drive isn't there anymore, so nothing was changed."));
}
