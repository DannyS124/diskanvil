// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "addons.h"

#include <QMainWindow>
#include <QMap>

class DiskMap;
class NoticeBar;
class QTimer;
struct Job;
class HelpWindow;
class UpdateChecker;
class QAction;
class QMenu;
class QProgressBar;
class QTreeWidget;
class QTreeWidgetItem;
class UDisks;
struct Disk;
struct Volume;

class QShortcut;
class QToolBar;
class QKeySequence;
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(UDisks *udisks, QWidget *parent = nullptr);

    bool selectDevice(const QString &device); // e.g. /dev/sdb1, or /dev/sdb for the whole drive
    // Opens one of the tools by name, for --open: write-image, windows-usb, rescue-usb,
    // check-stick, lost-files. False for a name it doesn't know.
    bool openTool(const QString &name);
    // On a drive with spots it can't read: a Rescue Copy offered first, before checking or
    // repairing what's on it. True when it's to go ahead.
    bool failingDriveOk(const Volume &volume, bool repairing);
    QWidget *toolParent(); // the parent for the USB tools' dialogs
    void buildContextMenu(QMenu *menu); // the right-click menu for the current selection
    // Sets up add-on toolbar pins and shortcuts again, after add-ons or their settings changed.
    void refreshAddons();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void createActions();
    void rebuild();
    void fillTable();
    void syncTableToMap();
    void onTableSelection();
    void updateActions();
    void updateProgress();
    void updateNotices();
    void updateCloneNotices();
    QStringList m_cloneNoticeKey;
    // RAID/NAS drives and Windows dynamic disks that aren't opened yet (closedsets.h).
    void updateClosedNotices();
    void openClosedSets();
    QStringList m_closedNoticeKey;
    QAction *m_openSets = nullptr;
    // DiskAnvil Live's write protection (writeblock.h): every drive read-only but its own stick.
    QAction *m_writeBlock = nullptr;
    QAction *m_remoteHelp = nullptr;
    QAction *m_netBoot = nullptr;
    void setWriteBlock(const QStringList &args);
    void updateWriteBlockNotice();
    QString m_writeBlockKey;
    QAction *m_productKey = nullptr;
    QAction *m_drivers = nullptr;
    QAction *m_activity = nullptr;
    QAction *m_lookCloser = nullptr;
    QAction *m_files = nullptr; // DiskAnvil Live's file window (the dock's Files): a PC has its own
    QAction *m_saveFiles = nullptr;
    QAction *m_bootMenu = nullptr;
    QAction *m_linuxBoot = nullptr;
    QAction *m_winRe = nullptr;
    QAction *m_clonezilla = nullptr;
    QAction *m_testBoot = nullptr;      // a drive
    QAction *m_testBootImage = nullptr; // a disk image file
    QAction *m_certificates = nullptr;
    QAction *m_evidence = nullptr;
    void updateJobBars();
    void showNoticesIfAny();
    void stopJob(const QString &jobPath);
    QString jobTarget(const Job &job, bool *wholeDrive) const;
    void runOnDrive(const QString &device, QAction *action);
    void showContextMenu(const QPoint &globalPos);
    void activate();
    void openInFileManager();
    void copyDevicePath();
    void showProperties();
    void newPartition();
    void formatVolume();
    void deletePartition();
    void resizeVolume();
    void changeLabel();
    void newPartitionTable();
    // LUKS: a copy of the header (its keys), and putting one back.
    void backupLuksHeader();
    void restoreLuksHeader();
    void checkForUpdates();
    bool addAddonActions(QMenu *menu); // false if none apply
    void runAddon(const QString &addonId, const QString &label);
    // test: from the Add-on Maker; always asks, never remembers.
    void runAddonWith(const Addon &addon, const AddonAction &action, bool test);
    // Why an add-on action doesn't fit the current selection; empty if it does.
    QString addonReason(const QString &addonId, const QString &label) const;
    QList<QKeySequence> takenShortcuts() const; // the window's own, for the Add-ons window
    int selectedDiskNumber() const;

    const Disk *selectedDisk() const;
    const Volume *selectedVolume() const;
    const Volume *volumeByPath(const QString &objectPath) const;
    void gone();

    UDisks *m_udisks;
    QTreeWidget *m_table;
    DiskMap *m_map;
    bool m_syncing = false;
    bool m_toolOnly = false; // started with --open: only the tool shows

    QAction *m_refresh = nullptr;
    QAction *m_open = nullptr;
    QAction *m_mount = nullptr;
    QAction *m_unmount = nullptr;
    QAction *m_copy = nullptr;
    QAction *m_properties = nullptr;
    QAction *m_newPartition = nullptr;
    QAction *m_format = nullptr;
    QAction *m_delete = nullptr;
    QAction *m_resize = nullptr;
    QAction *m_rename = nullptr;
    QAction *m_typeFlags = nullptr;
    QAction *m_inspect = nullptr;
    QAction *m_recover = nullptr;
    QAction *m_findFiles = nullptr;
    QAction *m_findFilesImage = nullptr;
    QAction *m_virusScan = nullptr;
    QAction *m_raidCheck = nullptr;
    QAction *m_newTable = nullptr;
    QAction *m_safelyRemove = nullptr;
    QAction *m_check = nullptr;
    QAction *m_startup = nullptr;
    QAction *m_unlock = nullptr;
    QAction *m_lock = nullptr;
    QAction *m_changePass = nullptr;
    QAction *m_luksBackup = nullptr;
    QAction *m_luksRestore = nullptr;
    QAction *m_openImage = nullptr;
    QAction *m_detachImage = nullptr;
    QAction *m_writeImage = nullptr;
    QAction *m_rescueUsb = nullptr;
    QAction *m_liveOnPc = nullptr;
    QAction *m_windowsUsb = nullptr;
    QAction *m_wipe = nullptr;
    QAction *m_sdCard = nullptr; // format a memory card the SD Card Association's way
    QAction *m_health = nullptr;
    QAction *m_benchmark = nullptr;
    QAction *m_badSectors = nullptr;
    QAction *m_checkStick = nullptr;
    QAction *m_clone = nullptr;
    QAction *m_newIds = nullptr;
    QAction *m_backup = nullptr;
    QAction *m_restore = nullptr;
    QAction *m_rescue = nullptr;
    QAction *m_whatHappened = nullptr;
    QAction *m_usage = nullptr;
    QAction *m_optimize = nullptr;
    QAction *m_cleanup = nullptr;
    QAction *m_snapshots = nullptr;
    QAction *m_secureErase = nullptr;
    QProgressBar *m_progress = nullptr;
    Addons m_addons;
    QToolBar *m_toolbar = nullptr;
    QWidget *m_notices = nullptr; // the bars above the disk list
    QMap<QString, NoticeBar *> m_jobBars; // job path (or "selftest:" + drive) -> its bar
    QTimer *m_jobRecheck = nullptr;       // for jobs too new to have a bar yet
    QAction *m_stop = nullptr;
    QStringList m_noticeKey;      // what they show, so they're only rebuilt when it changes
    QList<QAction *> m_pinned; // add-on actions pinned to the toolbar, and their separator
    QList<QShortcut *> m_shortcuts;
    HelpWindow *m_help = nullptr;
    UpdateChecker *m_updates = nullptr;
};
