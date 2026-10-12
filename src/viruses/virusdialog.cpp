// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "virusdialog.h"

#include "applog.h"
#include "blockio.h"
#include "dialogs.h"
#include "format.h"
#include "hashlookup.h"
#include "rescuestick.h"
#include "udisks.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColor>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QTabWidget>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>

#ifndef Q_OS_WIN
#include <unistd.h>
#else
#include <io.h>
#endif

namespace {

// The drives worth scanning: the file systems Windows, Linux and cameras keep files on (on
// Windows, only those Windows can open). Boot and recovery partitions hold nobody's files.
bool scannable(const Volume &v)
{
#ifdef Q_OS_WIN
    static const QStringList kinds = {QStringLiteral("ntfs"), QStringLiteral("vfat"), QStringLiteral("exfat"), QStringLiteral("refs")};
#else
    static const QStringList kinds = {QStringLiteral("ntfs"), QStringLiteral("vfat"), QStringLiteral("exfat"), QStringLiteral("ext2"),
                                      QStringLiteral("ext3"), QStringLiteral("ext4"), QStringLiteral("btrfs"), QStringLiteral("xfs"),
                                      QStringLiteral("f2fs"), QStringLiteral("hfsplus"), QStringLiteral("refs")};
#endif
    static const QStringList notFiles = {QStringLiteral("de94bba4-06d1-4d40-a16a-bfd50179d6ac"), // Windows recovery
                                         QStringLiteral("e3c9e316-0b5c-4db8-817d-f92df00215ae"), // Microsoft reserved
                                         QStringLiteral("0x27")};                                // Windows recovery (MBR)
    if (v.isEfi || isBootPartitionType(v.partType.toLower()) || notFiles.contains(v.partType.toLower()))
        return false;
    return v.canMount() && kinds.contains(v.effectiveFsType());
}

QString age(const QDateTime &when)
{
    const qint64 days = when.daysTo(QDateTime::currentDateTime());
    if (days <= 0)
        return VirusDialog::tr("today");
    if (days == 1)
        return VirusDialog::tr("yesterday");
    return VirusDialog::tr("%n day(s) ago", nullptr, int(days));
}

QString logPath()
{
    // On DiskAnvil Live: with the session's logs, which go to the stick with Save Logs.
    if (rescue::runningInRescue() && QFileInfo(QStringLiteral("/run/diskanvil-live/session")).isWritable())
        return QStringLiteral("/run/diskanvil-live/session/virus-scan.log");
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/diskanvil/virus-scan.log");
}

#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

// Drives are mounted read-only for the scan where that's possible (Windows has no such thing
// for a drive letter).
#ifdef Q_OS_WIN
constexpr bool kReadOnlyMounts = false;
#else
constexpr bool kReadOnlyMounts = true;
#endif

QIcon severityIcon(int severity)
{
    return severity >= 2 ? QIcon::fromTheme(QStringLiteral("dialog-error"), QApplication::style()->standardIcon(QStyle::SP_MessageBoxCritical))
                         : QIcon::fromTheme(QStringLiteral("dialog-warning"), QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning));
}

QColor severityColor(int severity)
{
    return severity >= 2 ? QColor(0xff, 0x33, 0x55) : severity == 1 ? QColor(0xff, 0xb3, 0x00) : QColor(0x7f, 0x93, 0xb5);
}

} // namespace

VirusDialog::VirusDialog(UDisks *udisks, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_pages(new QStackedWidget)
    , m_engineInfo(new QLabel)
    , m_update(new QPushButton(tr("Update the List")))
    , m_targets(new QTreeWidget)
    , m_depth(new QButtonGroup(this))
    , m_scan(new QPushButton(tr("Scan")))
    , m_pickNote(new QLabel)
    , m_status(new QLabel)
    , m_bar(new QProgressBar)
    , m_phase(new QLabel)
    , m_stop(new QPushButton(tr("Stop")))
    , m_hits(new QTreeWidget)
    , m_hitInfo(new QLabel)
    , m_delete(new QPushButton(tr("Delete Ticked Files…")))
    , m_again(new QPushButton(tr("Scan Something Else")))
    , m_tabs(new QTabWidget)
    , m_startupList(new QTreeWidget)
    , m_startupInfo(new QLabel)
    , m_startupDetails(new QLabel)
    , m_showAll(new QCheckBox(tr("Show everything that starts with Windows")))
    , m_switchOff(new QPushButton(tr("Switch Off Ticked…")))
    , m_bootList(new QTreeWidget)
    , m_bootInfo(new QLabel)
    , m_bootDetails(new QLabel)
    , m_putBack(new QPushButton(tr("Put Back Windows' Own…")))
    , m_lookUp(new QPushButton(tr("Look Up Online…")))
{
    setWindowTitle(tr("Scan for Viruses"));
    setObjectName(QStringLiteral("virusScan"));
    resize(900, 620);

    // 1. What to scan
    auto *pick = new QWidget;
    auto *pickLayout = new QVBoxLayout(pick);
    pickLayout->addWidget(wrappingLabel(rescue::runningInRescue()
        ? tr("Windows isn't running now, so malware on its drive can't hide or defend itself: this is the best time to look. "
             "Pick the drive to scan; Windows' drive is first.")
        : tr("Scan a drive or a folder for known malware. The drive Windows or Linux is running from is better scanned from "
             "DiskAnvil Live, where malware can't hide.")));
    auto *engineRow = new QHBoxLayout;
    m_engineInfo->setWordWrap(true);
    m_engineInfo->setTextFormat(Qt::RichText);
    m_engineInfo->setObjectName(QStringLiteral("engine"));
    engineRow->addWidget(m_engineInfo, 1);
    m_update->setAutoDefault(false);
    engineRow->addWidget(m_update);
    pickLayout->addLayout(engineRow);
    m_targets->setObjectName(QStringLiteral("targets"));
    m_targets->setHeaderLabels({tr("Drive"), tr("Size"), tr("File system")});
    m_targets->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_targets->setRootIsDecorated(false);
    pickLayout->addWidget(m_targets, 1);
    auto *quick = new QRadioButton(tr("Quick: programs, scripts and documents (most malware is one of those)"));
    auto *everything = new QRadioButton(tr("Everything: every file, which takes much longer"));
    quick->setObjectName(QStringLiteral("quick"));
    m_depth->addButton(quick, int(virus::Depth::Quick));
    m_depth->addButton(everything, int(virus::Depth::Everything));
    quick->setChecked(true);
    pickLayout->addWidget(quick);
    pickLayout->addWidget(everything);
    m_pickNote->setWordWrap(true);
    m_pickNote->setTextFormat(Qt::RichText);
    pickLayout->addWidget(m_pickNote);
    auto *pickButtons = new QHBoxLayout;
    auto *folder = new QPushButton(tr("A Folder…"));
    for (QPushButton *b : {folder, m_scan})
        b->setAutoDefault(false);
    pickButtons->addWidget(folder);
    pickButtons->addStretch();
    pickButtons->addWidget(m_scan);
    pickLayout->addLayout(pickButtons);
    m_pages->addWidget(pick);

    // 2. Scanning, and what it found
    auto *results = new QWidget;
    auto *resultsLayout = new QVBoxLayout(results);
    auto *top = new QHBoxLayout;
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::RichText);
    m_status->setObjectName(QStringLiteral("status"));
    top->addWidget(m_status, 1);
    m_stop->setAutoDefault(false);
    top->addWidget(m_stop);
    resultsLayout->addLayout(top);
    resultsLayout->addWidget(m_bar);
    resultsLayout->addWidget(m_phase);
    m_hits->setObjectName(QStringLiteral("hits"));
    m_hits->setHeaderLabels({tr("File"), tr("What it is")});
    m_hits->setRootIsDecorated(false);
    m_hits->setWordWrap(true);
    m_hits->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_hits->header()->resizeSection(0, 380);
    m_tabs->setObjectName(QStringLiteral("tabs"));
    m_tabs->addTab(m_hits, tr("Files"));
    // What starts with Windows: the places malware puts itself to start again.
    auto *startupPage = new QWidget;
    auto *startupLayout = new QVBoxLayout(startupPage);
    m_startupInfo->setWordWrap(true);
    m_startupInfo->setTextFormat(Qt::RichText);
    m_startupInfo->setObjectName(QStringLiteral("startupInfo"));
    startupLayout->addWidget(m_startupInfo);
    m_startupList->setObjectName(QStringLiteral("startup"));
    m_startupList->setHeaderLabels({tr("Name"), tr("How it starts"), tr("Why it looks odd")});
    m_startupList->setRootIsDecorated(false);
    m_startupList->setUniformRowHeights(true);
    m_startupList->header()->resizeSection(0, 220);
    m_startupList->header()->resizeSection(1, 280);
    startupLayout->addWidget(m_startupList, 2);
    // The one picked, in full.
    m_startupDetails->setObjectName(QStringLiteral("startupDetails"));
    m_startupDetails->setWordWrap(true);
    m_startupDetails->setTextFormat(Qt::RichText);
    m_startupDetails->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_startupDetails->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_startupDetails->setMinimumHeight(110);
    startupLayout->addWidget(m_startupDetails, 1);
    m_showAll->setObjectName(QStringLiteral("showAll"));
    auto *startupButtons = new QHBoxLayout;
    startupButtons->addWidget(m_showAll, 1);
    m_switchOff->setAutoDefault(false);
    m_switchOff->setEnabled(false);
    startupButtons->addWidget(m_switchOff);
    startupLayout->addLayout(startupButtons);
    m_tabs->addTab(startupPage, tr("Starts with Windows"));
    m_tabs->setTabVisible(1, false);
    // What starts the PC before Windows does: where bootkits hide.
    auto *bootPage = new QWidget;
    auto *bootLayout = new QVBoxLayout(bootPage);
    m_bootInfo->setWordWrap(true);
    m_bootInfo->setTextFormat(Qt::RichText);
    m_bootInfo->setObjectName(QStringLiteral("bootInfo"));
    bootLayout->addWidget(m_bootInfo);
    m_bootList->setObjectName(QStringLiteral("boot"));
    m_bootList->setHeaderLabels({tr("Name"), tr("What it is"), tr("What the check says")});
    m_bootList->setRootIsDecorated(false);
    m_bootList->setUniformRowHeights(true);
    m_bootList->header()->resizeSection(0, 200);
    m_bootList->header()->resizeSection(1, 280);
    bootLayout->addWidget(m_bootList, 2);
    m_bootDetails->setObjectName(QStringLiteral("bootDetails"));
    m_bootDetails->setWordWrap(true);
    m_bootDetails->setTextFormat(Qt::RichText);
    m_bootDetails->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_bootDetails->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_bootDetails->setMinimumHeight(90);
    bootLayout->addWidget(m_bootDetails, 1);
    auto *bootButtons = new QHBoxLayout;
    bootButtons->addStretch();
    m_putBack->setAutoDefault(false);
    m_putBack->setEnabled(false);
    m_putBack->setToolTip(tr("Copies Windows' own boot manager (kept in Windows\\Boot\\EFI) over the ticked ones, keeping the old ones in a backup"));
    bootButtons->addWidget(m_putBack);
    bootLayout->addLayout(bootButtons);
    m_tabs->addTab(bootPage, tr("Boot"));
    m_tabs->setTabVisible(2, false);
    resultsLayout->addWidget(m_tabs, 1);
    auto *bottom = new QHBoxLayout;
    m_hitInfo->setWordWrap(true);
    m_hitInfo->setObjectName(QStringLiteral("hitInfo"));
    bottom->addWidget(m_hitInfo, 1);
    m_lookUp->setObjectName(QStringLiteral("lookUp"));
    m_lookUp->setToolTip(tr("Asks a public list of known files whether it knows the programs that look odd. It asks first, every time."));
    m_lookUp->hide();
    for (QPushButton *b : {m_lookUp, m_again, m_delete}) {
        b->setAutoDefault(false);
        bottom->addWidget(b);
    }
    resultsLayout->addLayout(bottom);
    m_pages->addWidget(results);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_pages, 1);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(box, &QDialogButtonBox::rejected, this, &VirusDialog::reject);
    layout->addWidget(box);

    connect(m_update, &QPushButton::clicked, this, &VirusDialog::updateList);
    connect(m_scan, &QPushButton::clicked, this, &VirusDialog::start);
    connect(folder, &QPushButton::clicked, this, &VirusDialog::chooseFolder);
    connect(m_targets, &QTreeWidget::currentItemChanged, this, &VirusDialog::updateButtons);
    connect(m_targets, &QTreeWidget::itemDoubleClicked, this, &VirusDialog::start);
    connect(m_stop, &QPushButton::clicked, this, [this] {
        m_stopped = true;
        if (m_job)
            m_job->cancel();
        m_stop->setEnabled(false);
    });
    connect(m_showAll, &QCheckBox::toggled, this, &VirusDialog::showStartup);
    connect(m_startupList, &QTreeWidget::currentItemChanged, this, &VirusDialog::showStartupDetails);
    connect(m_startupList, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item) {
        const int index = item->data(0, Qt::UserRole).toInt();
        if (item->checkState(0) == Qt::Checked)
            m_ticked.insert(index);
        else
            m_ticked.remove(index);
        m_switchOff->setEnabled(!m_ticked.isEmpty() && !m_busy);
        m_switchOff->setText(m_ticked.isEmpty() ? tr("Switch Off Ticked…") : tr("Switch Off %n Ticked…", nullptr, int(m_ticked.size())));
    });
    connect(m_switchOff, &QPushButton::clicked, this, &VirusDialog::switchOffTicked);
    connect(m_bootList, &QTreeWidget::currentItemChanged, this, &VirusDialog::showBootDetails);
    connect(m_bootList, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item) {
        const int index = item->data(0, Qt::UserRole).toInt();
        if (item->checkState(0) == Qt::Checked)
            m_bootTicked.insert(index);
        else
            m_bootTicked.remove(index);
        m_putBack->setEnabled(!m_bootTicked.isEmpty() && !m_busy);
    });
    connect(m_putBack, &QPushButton::clicked, this, &VirusDialog::putBackTicked);
    connect(m_lookUp, &QPushButton::clicked, this, &VirusDialog::lookUpOnline);
    connect(m_again, &QPushButton::clicked, this, [this] {
        m_pages->setCurrentIndex(0);
        fillTargets();
    });
    connect(m_delete, &QPushButton::clicked, this, &VirusDialog::deleteTicked);
    connect(m_hits, &QTreeWidget::itemChanged, this, &VirusDialog::updateButtons);

    fillTargets();
    if (rescue::runningInRescue())
        loadLiveList();
    else
        refreshEngine();
}

VirusDialog::~VirusDialog()
{
    stopWorkers();
}

void VirusDialog::stopWorkers()
{
    if (m_job)
        m_job->cancel();
    if (m_updateJob)
        m_updateJob->cancel();
    for (QThread *thread : {m_thread, m_startupThread}) {
        if (thread) {
            thread->quit();
            thread->wait();
        }
    }
    m_job = nullptr;
    m_updateJob = nullptr;
    m_thread = nullptr;
    m_startupThread = nullptr;
}

void VirusDialog::reject()
{
    stopWorkers();
    // Drives mounted just for the scan go back to how they were.
    for (const Disk &d : m_udisks->disks()) {
        for (const Volume &v : d.volumes) {
            if (m_mountedHere.contains(v.objectPath) && !v.mounts().isEmpty())
                m_udisks->unmount(v);
        }
    }
    QDialog::reject();
}

void VirusDialog::loadLiveList()
{
    // The list is kept on the stick; scans use a copy in memory.
    m_engineInfo->setText(tr("Getting the virus list from the stick…"));
    m_update->setEnabled(false);
    auto *p = new QProcess(this);
    connect(p, &QProcess::finished, this, [this, p] {
        p->deleteLater();
        refreshEngine();
    });
    p->start(QStringLiteral("diskanvil-live-virus-list"), {QStringLiteral("load")});
}

void VirusDialog::refreshEngine()
{
    m_engine = virus::findEngine(rescue::runningInRescue());
    QString text;
    bool canUpdate = !m_engine.updater.isEmpty();
    switch (m_engine.kind) {
    case virus::Engine::Kind::None:
#ifdef Q_OS_WIN
        text = redText(tr("There's nothing to scan with: Microsoft Defender isn't on this PC, and neither is ClamAV."));
#else
        text = redText(tr("Scan for Viruses uses ClamAV, and it isn't installed. Install the clamav package (Arch: pacman -S clamav; "
                          "Debian and Ubuntu: apt install clamav), then open this again."));
#endif
        canUpdate = false;
        break;
    case virus::Engine::Kind::Defender:
        text = tr("Microsoft Defender scans. Windows keeps its list of viruses up to date by itself.");
        break;
    case virus::Engine::Kind::ClamAV:
        if (!m_engine.listDate.isValid()) {
            text = tr("ClamAV is ready, but it doesn't have its list of viruses yet: a one-time download of about 250 MB, "
                      "with small updates after that.");
            m_update->setText(tr("Download the List"));
        } else {
            text = tr("ClamAV, with a list of viruses from %1 (%L2 known ones).").arg(age(m_engine.listDate)).arg(m_engine.signatures);
            if (m_engine.listDate.daysTo(QDateTime::currentDateTime()) > 7)
                text += QStringLiteral(" ") + redText(tr("That's old: update it before scanning, if there's internet."));
            m_update->setText(tr("Update the List"));
        }
        break;
    }
    m_engineInfo->setText(text);
    // Defender looks at every file whatever it's asked.
    for (QAbstractButton *depth : m_depth->buttons())
        depth->setEnabled(m_engine.kind != virus::Engine::Kind::Defender);
    m_update->setVisible(m_engine.kind != virus::Engine::Kind::None);
    m_update->setEnabled(canUpdate && !m_busy);
    updateButtons();
}

void VirusDialog::fillTargets()
{
    m_targets->clear();
    QList<QTreeWidgetItem *> windows, others;
    for (const Disk &d : m_udisks->disks()) {
        for (const Volume &v : d.volumes) {
            if (!scannable(v))
                continue;
            bool isWindows = false;
            for (const QString &mount : v.mounts())
                isWindows = isWindows || QFileInfo::exists(QDir(mount).filePath(QStringLiteral("Windows/System32")));
            const QString name = isWindows ? tr("Windows (%1)").arg(shortDevice(v.device)) : volumeTitle(v);
            auto *item = new QTreeWidgetItem({name, formatSize(v.size), v.effectiveFsType()});
            item->setData(0, Qt::UserRole, v.objectPath);
            item->setData(0, Qt::UserRole + 1, v.mounts());
            item->setIcon(0, QIcon::fromTheme(isWindows ? QStringLiteral("windows")
                                              : d.removable || d.bus == QLatin1String("usb") ? QStringLiteral("drive-removable-media-usb")
                                                                                             : QStringLiteral("drive-harddisk")));
            (isWindows || v.effectiveFsType() == QLatin1String("ntfs") ? windows : others) << item;
        }
    }
    m_targets->addTopLevelItems(windows + others);
    for (int c = 1; c < 3; ++c)
        m_targets->resizeColumnToContents(c);
    if (m_targets->topLevelItemCount() > 0)
        m_targets->setCurrentItem(m_targets->topLevelItem(0));
    updateButtons();
}

void VirusDialog::updateButtons()
{
    const bool haveList = m_engine.kind == virus::Engine::Kind::Defender
                       || (m_engine.kind == virus::Engine::Kind::ClamAV && m_engine.listDate.isValid());
    m_scan->setEnabled(haveList && m_targets->currentItem() && !m_busy);
    m_pickNote->setText(m_engine.kind == virus::Engine::Kind::ClamAV && !haveList ? tr("Download the list first.") : QString());
    int ticked = 0;
    for (int i = 0; i < m_hits->topLevelItemCount(); ++i)
        ticked += m_hits->topLevelItem(i)->checkState(0) == Qt::Checked && !m_hits->topLevelItem(i)->data(0, Qt::UserRole + 1).toBool();
    m_delete->setEnabled(ticked > 0 && !m_busy);
    m_delete->setText(ticked > 0 ? tr("Delete %n Ticked File(s)…", nullptr, ticked) : tr("Delete Ticked Files…"));
}

void VirusDialog::updateList()
{
    if (m_busy)
        return;
    m_busy = true;
    m_update->setEnabled(false);
    m_scan->setEnabled(false);
    m_engineInfo->setText(tr("Getting the newest list…"));
    // A system list DiskAnvil can't write to: the update goes into DiskAnvil's own folder.
    const QString into = m_engine.databaseWritable ? m_engine.database
                                                   : QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/diskanvil/clamav");
    m_updateJob = new virus::Update(m_engine, into);
    connect(m_updateJob, &virus::Update::progress, this, [this](const QString &line) {
        m_engineInfo->setText(tr("Getting the newest list: %1").arg(line.toHtmlEscaped().left(120)));
    });
    connect(m_updateJob, &virus::Update::finished, this, [this](bool ok, const QString &message) {
        if (m_thread)
            m_thread->quit();
        m_thread = nullptr;
        m_updateJob = nullptr;
        m_busy = false;
        if (!ok) {
            refreshEngine();
            m_engineInfo->setText(m_engineInfo->text() + QStringLiteral("<br>")
                                  + redText(tr("Updating didn't work: %1. Is there internet?").arg(message.toHtmlEscaped())));
            return;
        }
        if (!rescue::runningInRescue()) {
            refreshEngine();
            return;
        }
        // On DiskAnvil Live the new list goes onto the stick for next time.
        refreshEngine();
        auto *p = new QProcess(this);
        connect(p, &QProcess::finished, this, [this, p] {
            p->deleteLater();
            const QString said = QString::fromUtf8(p->readAll()).trimmed();
            if (!said.isEmpty())
                m_engineInfo->setText(m_engineInfo->text() + QStringLiteral("<br>") + said.toHtmlEscaped());
        });
        p->setProcessChannelMode(QProcess::MergedChannels);
        p->start(QStringLiteral("diskanvil-live-virus-list"), {QStringLiteral("save")});
    });
    m_thread = startOnThread(this, m_updateJob);
}

void VirusDialog::chooseFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Scan Which Folder?"), QDir::homePath());
    if (!folder.isEmpty())
        scanFolder(folder);
}

void VirusDialog::start()
{
    const QTreeWidgetItem *item = m_targets->currentItem();
    if (!item || !m_scan->isEnabled())
        return;
    // Mounted already: everywhere it's mounted (a Btrfs drive's subvolumes can be in several
    // places), but not its snapshots.
    QStringList folders;
    for (const QString &mount : item->data(0, Qt::UserRole + 1).toStringList()) {
        if (!virus::Scan::skipped(mount))
            folders << mount;
    }
    if (folders.isEmpty()) {
        mountThenScan(item->data(0, Qt::UserRole).toString(), item->text(0));
        return;
    }
    m_volumePath = item->data(0, Qt::UserRole).toString();
    m_mountedReadOnly = false;
    startScan(folders, item->text(0));
}

void VirusDialog::mountThenScan(const QString &objectPath, const QString &title)
{
    const Volume *volume = nullptr;
    for (const Disk &d : m_udisks->disks()) {
        for (const Volume &v : d.volumes) {
            if (v.objectPath == objectPath)
                volume = &v;
        }
    }
    if (!volume)
        return;
    // Read-only: nothing on it changes while it's looked at.
    m_busy = true;
    updateButtons();
    m_pickNote->setText(tr("Mounting %1 (read-only)…").arg(title.toHtmlEscaped()));
    auto mounted = std::make_shared<QMetaObject::Connection>();
    auto failed = std::make_shared<QMetaObject::Connection>();
    *mounted = connect(m_udisks, &UDisks::changed, this, [this, objectPath, title, mounted, failed] {
        for (const Disk &d : m_udisks->disks()) {
            for (const Volume &v : d.volumes) {
                if (v.objectPath != objectPath || v.mounts().isEmpty())
                    continue;
                disconnect(*mounted);
                disconnect(*failed);
                m_busy = false;
                m_volumePath = objectPath;
                m_mountedReadOnly = kReadOnlyMounts;
                if (!m_mountedHere.contains(objectPath))
                    m_mountedHere << objectPath;
                startScan({v.mounts().first()}, title);
                return;
            }
        }
    });
    *failed = connect(m_udisks, &UDisks::operationFinished, this, [this, mounted, failed](bool ok, const QString &message) {
        if (ok)
            return;
        disconnect(*mounted);
        disconnect(*failed);
        m_busy = false;
        m_pickNote->setText(redText(message.toHtmlEscaped()));
        updateButtons();
    });
    m_udisks->mount(*volume, kReadOnlyMounts);
}

void VirusDialog::scanFolder(const QString &folder)
{
    if (m_engine.kind == virus::Engine::Kind::None || (m_engine.kind == virus::Engine::Kind::ClamAV && !m_engine.listDate.isValid())) {
        m_pickNote->setText(redText(m_engine.kind == virus::Engine::Kind::None ? tr("There's nothing to scan with.") : tr("Download the list first.")));
        return;
    }
    m_volumePath.clear();
    m_mountedReadOnly = false;
    startScan({folder}, QDir::toNativeSeparators(folder));
}

void VirusDialog::startScan(const QStringList &folders, const QString &title)
{
    stopWorkers();
    m_folders = folders;
    m_title = title;
    m_found = 0;
    m_hits->clear();
    m_pages->setCurrentIndex(1);
    m_busy = true;
    m_stop->show();
    m_stop->setEnabled(true);
    m_bar->show();
    m_bar->setRange(0, 0);
    m_phase->show();
    m_again->setEnabled(false);
    m_hitInfo->setText(QString());
    m_status->setText(tr("Scanning %1 with %2…").arg(title.toHtmlEscaped(), m_engine.name()));
    log({QStringLiteral("%1 scanning %2 (%3) with %4, list from %5")
             .arg(QDateTime::currentDateTime().toString(Qt::ISODate), folders.join(QStringLiteral(", ")), title, m_engine.name(),
                  m_engine.listDate.isValid() ? m_engine.listDate.toString(Qt::ISODate) : QStringLiteral("-"))});
    m_stopped = false;
    m_startup = {};
    m_ticked.clear();
    m_switched.clear();
    m_startupList->clear();
    m_tabs->setTabVisible(1, false);
    m_tabs->setCurrentIndex(0);
    m_tabs->setTabText(0, tr("Files"));
    updateButtons();
    m_boot = {};
    m_bootTicked.clear();
    m_putBackDone.clear();
    m_bootList->clear();
    m_tabs->setTabVisible(2, false);
    m_online.clear();
    m_lookUp->hide();

    // What starts with Windows comes first: those programs are scanned whatever else is.
    QString windows;
    for (const QString &folder : folders) {
        if (startup::hasWindows(folder)) {
            windows = folder;
            break;
        }
    }
    bool running = false;
#ifdef Q_OS_WIN
    // The Windows that's running is read through its registry, not its files.
    // Only for the whole drive, not a folder on it.
    const QString systemDrive = qEnvironmentVariable("SystemDrive").toLower();
    for (QString folder : folders) {
        folder = QDir::fromNativeSeparators(folder).toLower();
        while (folder.endsWith(QLatin1Char('/')))
            folder.chop(1);
        running = running || folder == systemDrive;
    }
    if (running)
        windows.clear();
#endif
    m_windowsRoot = windows;
    // What starts the PC before Windows: alongside the rest, it's quick.
    if (!windows.isEmpty() || running)
        checkBoot(running);
    if (windows.isEmpty() && !running) {
        runScan({});
        return;
    }
    m_phase->setText(tr("Reading what starts with Windows"));
    auto *finder = new startup::Finder(windows);
    connect(finder, &startup::Finder::finished, this, [this](const startup::Report &report) {
        if (m_startupThread) {
            m_startupThread->quit();
            m_startupThread->wait();
            m_startupThread = nullptr;
        }
        m_startup = report;
        QStringList lines;
        QStringList programs;
        for (const startup::Entry &e : std::as_const(m_startup.entries)) {
            if (!e.file.isEmpty())
                programs << e.file;
            if (e.severity > 0)
                lines << QStringLiteral("  starts with Windows, looks odd (%1): %2, %3 = %4").arg(e.severity).arg(e.location, e.name, e.command);
        }
        programs.removeDuplicates();
        log(QStringList{QStringLiteral("  %1 things start with Windows").arg(m_startup.entries.size())} + lines);
        showStartup();
        if (m_stopped) {
            scanFinished(false, 0, 0, tr("Stopped."));
            return;
        }
        runScan(programs);
    });
    m_startupThread = startOnThread(this, finder);
}

void VirusDialog::runScan(const QStringList &alsoScan)
{
    m_job = new virus::Scan(m_engine, m_folders, virus::Depth(m_depth->checkedId()));
    m_job->alsoScan(alsoScan);
    connect(m_job, &virus::Scan::phase, this, [this](const QString &text) { m_phase->setText(text); });
    connect(m_job, &virus::Scan::progress, this, [this](quint64 done, quint64 total) {
        if (total == 0) {
            m_bar->setRange(0, 0);
            return;
        }
        m_bar->setRange(0, 1000);
        m_bar->setValue(int(done * 1000 / total));
        m_phase->setText(tr("Scanning: %L1 of %L2 files").arg(done).arg(total));
    });
    connect(m_job, &virus::Scan::found, this, &VirusDialog::addHit);
    m_compressed = 0;
    connect(m_job, &virus::Scan::compressedByWindows, this, [this](int count) { m_compressed = count; });
    connect(m_job, &virus::Scan::finished, this, &VirusDialog::scanFinished);
    m_thread = startOnThread(this, m_job);
}

void VirusDialog::showStartupDetails()
{
    const QTreeWidgetItem *item = m_startupList->currentItem();
    if (!item) {
        m_startupDetails->clear();
        return;
    }
    const startup::Entry &e = m_startup.entries.at(item->data(0, Qt::UserRole).toInt());
    auto code = [](const QString &text) { return QStringLiteral("<code>%1</code>").arg(text.toHtmlEscaped()); };
    QString html = QStringLiteral("<b>%1</b>: %2").arg(e.name.toHtmlEscaped(), e.where.toHtmlEscaped());
    if (!e.reasons.isEmpty()) {
        html += QStringLiteral("<ul style=\"margin: 4px 0\">");
        for (const QString &reason : e.reasons)
            html += QStringLiteral("<li>%1</li>").arg(reason.toHtmlEscaped());
        html += QStringLiteral("</ul>");
    } else {
        html += QStringLiteral("<br>");
    }
    html += tr("Kept in: %1").arg(code(e.value.isEmpty() ? e.location : e.location + QStringLiteral(" → ") + e.value)) + QStringLiteral("<br>");
    html += tr("It runs: %1").arg(code(e.command)) + QStringLiteral("<br>");
    if (!e.program.isEmpty())
        html += tr("The program: %1").arg(code(e.program)) + (e.file.isEmpty() ? QLatin1Char(' ') + tr("(not there)") : QString());
    if (m_online.contains(e.file))
        html += QStringLiteral("<br>") + tr("Looked up online: %1").arg(m_online.value(e.file).toHtmlEscaped());
    m_startupDetails->setText(html);
}

void VirusDialog::showStartup()
{
    m_startupList->clear();
    QVector<int> order;
    int odd = 0;
    for (int i = 0; i < m_startup.entries.size(); ++i) {
        odd += m_startup.entries[i].severity > 0;
        if (m_startup.entries[i].severity > 0 || m_showAll->isChecked())
            order << i;
    }
    // The worst first; otherwise as Windows has them.
    std::stable_sort(order.begin(), order.end(), [this](int a, int b) { return m_startup.entries[a].severity > m_startup.entries[b].severity; });
    for (int i : std::as_const(order)) {
        const startup::Entry &e = m_startup.entries[i];
        auto *item = new QTreeWidgetItem({e.name, e.where, e.reasons.isEmpty() ? tr("Nothing odd") : e.reasons.first()});
        item->setData(0, Qt::UserRole, i);
        if (e.severity > 0) {
            item->setIcon(0, severityIcon(e.severity));
            item->setForeground(2, severityColor(e.severity));
        }
        const auto switched = m_switched.constFind(i);
        if (switched != m_switched.constEnd() && switched->first) {
            item->setText(2, tr("Switched off: %1").arg(switched->second));
            item->setForeground(2, QColor(0x2b, 0xee, 0x8a));
            item->setIcon(0, QIcon::fromTheme(QStringLiteral("dialog-ok-apply"), QApplication::style()->standardIcon(QStyle::SP_DialogApplyButton)));
        } else if (e.severity > 0 && startup::canSwitchOff(e)) {
            item->setCheckState(0, m_ticked.contains(i) ? Qt::Checked : Qt::Unchecked);
            if (switched != m_switched.constEnd())
                item->setText(2, tr("Not switched off: %1").arg(switched->second));
        }
        m_startupList->addTopLevelItem(item);
    }
    if (m_startupList->topLevelItemCount() > 0)
        m_startupList->setCurrentItem(m_startupList->topLevelItem(0));
    showStartupDetails();
    m_switchOff->setEnabled(!m_ticked.isEmpty() && !m_busy);
    m_switchOff->setText(m_ticked.isEmpty() ? tr("Switch Off Ticked…") : tr("Switch Off %n Ticked…", nullptr, int(m_ticked.size())));
    bool lookable = false;
    for (const startup::Entry &e : std::as_const(m_startup.entries))
        lookable = lookable || (e.severity > 0 && !e.file.isEmpty());
    m_lookUp->setVisible(lookable || m_lookUp->isVisible());
    m_tabs->setTabVisible(1, m_startup.windows);
    m_tabs->setTabText(1, odd ? tr("Starts with Windows (%n odd)", nullptr, odd) : tr("Starts with Windows"));
    QString info = odd ? tr("%1 things start with Windows. Odd ones: %n. Pick one to see exactly where it's kept and what it runs.",
                            nullptr, odd)
                             .arg(m_startup.entries.size())
                       : tr("Nothing that starts with Windows looks odd (%n looked at).", nullptr, int(m_startup.entries.size()));
    for (const QString &note : std::as_const(m_startup.notes))
        info += QStringLiteral("<br><small>") + note.toHtmlEscaped() + QStringLiteral("</small>");
    m_startupInfo->setText(info);
}

void VirusDialog::addHit(const virus::Detection &d)
{
    ++m_found;
    const int severity = virus::severity(d.name);
    auto *item = new QTreeWidgetItem({shownPath(d.path), virus::describe(d.name)});
    item->setData(0, Qt::UserRole, d.path);
    item->setToolTip(0, QDir::toNativeSeparators(d.path));
    item->setToolTip(1, virus::shownName(d.name));
    item->setIcon(0, severityIcon(severity));
    item->setForeground(1, severityColor(severity));
    // Real malware is ticked to go; guesses and unwanted programs are left to decide.
    item->setCheckState(0, severity >= 2 ? Qt::Checked : Qt::Unchecked);
    m_hits->addTopLevelItem(item);
    log({QStringLiteral("  found %1: %2").arg(virus::shownName(d.name), d.path)});
    bool starts = false;
    for (int i = 0; i < m_startup.entries.size(); ++i) {
        startup::Entry &e = m_startup.entries[i];
        if (!e.file.isEmpty() && QDir::cleanPath(e.file).compare(QDir::cleanPath(d.path), kPathCase) == 0) {
            // The most important thing about it, so it goes first.
            const QString found = tr("It starts something the virus scan found: %1").arg(virus::describe(d.name));
            e.reasons.removeAll(found);
            e.reasons.prepend(found);
            e.severity = 2;
            if (startup::canSwitchOff(e))
                m_ticked.insert(i);
            starts = true;
        }
    }
    if (starts) {
        item->setText(1, item->text(1) + QLatin1Char(' ') + tr("It starts with Windows."));
        showStartup();
    }
    updateButtons();
}

QString VirusDialog::shownPath(const QString &path) const
{
#ifndef Q_OS_WIN
    // A drive mounted somewhere: where it is on that drive ("/Users/a/setup.exe"). The running
    // system, or a drive in several places: the whole path.
    const QString root = m_folders.value(0);
    if (m_folders.size() == 1 && root != QLatin1String("/") && path.startsWith(root + QLatin1Char('/')))
        return path.mid(root.size());
#endif
    return QDir::toNativeSeparators(path);
}

void VirusDialog::scanFinished(bool completed, int scanned, int unreadable, const QString &problem)
{
    if (m_thread)
        m_thread->quit();
    m_thread = nullptr;
    m_job = nullptr;
    m_busy = false;
    m_stop->hide();
    m_bar->setRange(0, 1);
    m_bar->setValue(1);
    m_again->setEnabled(true);
    QString text;
    if (!completed && problem == tr("Stopped."))
        text = m_found ? tr("Stopped. %n thing(s) found before that.", nullptr, m_found) : tr("Stopped.");
    else if (!completed)
        text = redText(tr("The scan didn't finish: %1").arg(problem.toHtmlEscaped()));
    else if (m_found == 0)
        text = scanned > 0 ? tr("Done: %L1 files looked at, and nothing found.").arg(scanned) : tr("Done, and nothing found.");
    else
        text = scanned > 0 ? tr("Done: found %n thing(s) in %L1 files.", nullptr, m_found).arg(scanned) : tr("Done: found %n thing(s).", nullptr, m_found);
    if (unreadable > 0)
        text += QLatin1Char(' ') + tr("%n file(s) couldn't be read: another user's or the system's, or in use.", nullptr, unreadable);
    if (m_compressed > 0)
        text += QLatin1Char(' ') + tr("%n file(s) are kept compressed by Windows (CompactOS or compact.exe), which Linux can't read: "
                                      "Microsoft Defender, in Windows, checks those.", nullptr, m_compressed);
    int odd = 0;
    for (const startup::Entry &e : std::as_const(m_startup.entries))
        odd += e.severity > 0;
    if (odd > 0)
        text += QLatin1Char(' ') + tr("Starts with Windows: %n odd thing(s) to look at.", nullptr, odd);
    m_tabs->setTabText(0, m_found ? tr("Files (%1)").arg(m_found) : tr("Files"));
    if (m_found == 0 && odd > 0)
        m_tabs->setCurrentIndex(1);
    m_status->setText(text);
    m_bar->hide();
    m_phase->setText(m_found > 0 ? tr("What's ticked can be deleted. Real malware is ticked already; look at the rest first.") : QString());
    m_hitInfo->setText(m_found > 0 ? tr("Hover over a file to see the antivirus's own name for it.") : QString());
    log({QStringLiteral("  %1, %2 files, %3 found, %4 unreadable")
             .arg(completed ? QStringLiteral("done") : QStringLiteral("not finished: ") + problem)
             .arg(scanned)
             .arg(m_found)
             .arg(unreadable)});
    updateButtons();
}

void VirusDialog::deleteTicked()
{
    QStringList paths;
    QList<QTreeWidgetItem *> items;
    bool inWindows = false;
    for (int i = 0; i < m_hits->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_hits->topLevelItem(i);
        if (item->checkState(0) != Qt::Checked || item->data(0, Qt::UserRole + 1).toBool())
            continue;
        paths << item->data(0, Qt::UserRole).toString();
        items << item;
        inWindows = inWindows || item->text(0).contains(QRegularExpression(QStringLiteral("^([A-Za-z]:)?[/\\\\]Windows[/\\\\]"), QRegularExpression::CaseInsensitiveOption));
    }
    if (paths.isEmpty())
        return;
    QStringList shown;
    for (const QTreeWidgetItem *item : std::as_const(items).mid(0, 15))
        shown << QStringLiteral("• ") + item->text(0);
    if (items.size() > 15)
        shown << tr("…and %n more", nullptr, int(items.size() - 15));
    QString question = tr("Delete these %n file(s) for good?", nullptr, int(paths.size())) + QStringLiteral("\n\n") + shown.join(QLatin1Char('\n'));
    if (inWindows)
        question += QStringLiteral("\n\n") + tr("Some of them are in Windows' own folders. Malware often replaces Windows' files, but deleting one Windows "
                                                "needs can stop it from starting. If that happens, Windows' installation USB can repair it.");
    if (QMessageBox::question(this, windowTitle(), question, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    auto remove = [this, paths, items](const QString &from, const QString &to) {
        int deleted = 0;
        QStringList failed;
        for (int i = 0; i < paths.size(); ++i) {
            // Mounted again, the drive can be somewhere else: the same file, under the new place.
            QString path = paths[i];
            if (from != to && path.startsWith(from + QLatin1Char('/')))
                path = to + path.mid(from.size());
            QFile file(path);
            file.setPermissions(file.permissions() | QFileDevice::WriteOwner);
            if (file.remove()) {
                ++deleted;
                items[i]->setData(0, Qt::UserRole + 1, true);
                items[i]->setData(0, Qt::CheckStateRole, QVariant()); // nothing left to tick
                items[i]->setFlags(items[i]->flags() & ~Qt::ItemIsUserCheckable);
                items[i]->setIcon(0, QIcon::fromTheme(QStringLiteral("edit-delete"), QApplication::style()->standardIcon(QStyle::SP_TrashIcon)));
                items[i]->setText(1, tr("Deleted"));
                items[i]->setForeground(1, QColor(0x2b, 0xee, 0x8a));
                log({QStringLiteral("  deleted %1").arg(path)});
            } else {
                failed << QStringLiteral("%1 (%2)").arg(QDir::toNativeSeparators(path), file.errorString());
                log({QStringLiteral("  couldn't delete %1: %2").arg(path, file.errorString())});
            }
        }
        bool left = false;
        for (int i = 0; i < m_hits->topLevelItemCount(); ++i)
            left = left || !m_hits->topLevelItem(i)->data(0, Qt::UserRole + 1).toBool();
        if (!left)
            m_phase->setText(QString());
        m_status->setText(failed.isEmpty() ? tr("Deleted %n file(s). The list of what was done is in %1.", nullptr, deleted).arg(QDir::toNativeSeparators(logPath()))
                                           : redText(tr("Deleted %1, but not these: %2").arg(deleted).arg(failed.join(QStringLiteral(", ")).toHtmlEscaped())));
        m_busy = false;
        updateButtons();
    };

    whenWritable([remove](const QString &from, const QString &to) { remove(from, to); });
}

void VirusDialog::whenWritable(const std::function<void(const QString &from, const QString &to)> &then)
{
    const QString from = m_folders.value(0);
    if (!m_mountedReadOnly) {
        then(from, from);
        return;
    }
    remountWritable(m_volumePath, [this, from, then](const QString &to) {
        m_mountedReadOnly = false;
        for (QString &folder : m_folders) {
            if (folder == from)
                folder = to;
        }
        if (m_windowsRoot == from)
            m_windowsRoot = to;
        then(from, to);
    });
}

void VirusDialog::remountWritable(const QString &objectPath, const std::function<void(const QString &mount)> &then)
{
    const Volume *volume = nullptr;
    for (const Disk &d : m_udisks->disks()) {
        for (const Volume &v : d.volumes) {
            if (v.objectPath == objectPath)
                volume = &v;
        }
    }
    if (!volume) {
        m_status->setText(redText(tr("The drive isn't there anymore.")));
        return;
    }
    m_busy = true;
    updateButtons();
    m_status->setText(tr("Making the drive writable…"));
    auto changed = std::make_shared<QMetaObject::Connection>();
    auto failed = std::make_shared<QMetaObject::Connection>();
    auto step = std::make_shared<int>(volume->mounts().isEmpty() ? 1 : 0); // 0: unmounting, 1: mounting again
    *changed = connect(m_udisks, &UDisks::changed, this, [this, objectPath, changed, failed, step, then] {
        for (const Disk &d : m_udisks->disks()) {
            for (const Volume &v : d.volumes) {
                if (v.objectPath != objectPath)
                    continue;
                if (*step == 0 && v.mounts().isEmpty()) {
                    *step = 1;
                    m_udisks->mount(v, false);
                } else if (*step == 1 && !v.mounts().isEmpty()) {
                    disconnect(*changed);
                    disconnect(*failed);
                    m_busy = false;
                    then(v.mounts().first());
                    updateButtons();
                }
                return;
            }
        }
    });
    *failed = connect(m_udisks, &UDisks::operationFinished, this, [this, changed, failed](bool ok, const QString &message) {
        if (ok)
            return;
        disconnect(*changed);
        disconnect(*failed);
        m_busy = false;
        // Windows left hibernated (Fast Startup) can't be written to from outside.
        m_status->setText(redText(tr("The drive can't be made writable: %1. If Windows was put to sleep or shut down with Fast "
                                     "Startup, start it once and shut it down fully (hold Shift while clicking Shut down), then "
                                     "scan again.")
                                      .arg(message.toHtmlEscaped())));
        updateButtons();
    });
    if (*step == 1)
        m_udisks->mount(*volume, false);
    else
        m_udisks->unmount(*volume);
}

void VirusDialog::checkBoot(bool running)
{
    // The drive being scanned: the volume picked, or the one Windows is running from.
    const Disk *disk = nullptr;
    for (const Disk &d : m_udisks->disks()) {
        for (const Volume &v : d.volumes) {
            bool match = v.objectPath == m_volumePath;
#ifdef Q_OS_WIN
            const QString systemDrive = qEnvironmentVariable("SystemDrive").toLower();
            for (const QString &mount : v.mounts())
                match = match || (running && QDir::fromNativeSeparators(mount).toLower().startsWith(systemDrive));
#else
            Q_UNUSED(running)
#endif
            if (match)
                disk = &d;
        }
    }
    if (!disk)
        return;
    const QString windows = running ? QDir::fromNativeSeparators(qEnvironmentVariable("SystemDrive")) + QLatin1Char('/') : m_windowsRoot;
    // The old BIOS way: what's in the first sector matters only on a drive without a GPT.
    if (disk->tableType == QLatin1String("dos")) {
        openBlockThen(m_udisks, this, disk->blockPath, UDisks::OpenMode::Benchmark, [this](int fd) {
            if (fd < 0)
                return;
            blockio::Buffer sector = blockio::alignedBuffer(4096);
            const int size = std::max(512, blockio::logicalSize(fd));
            if (blockio::readAt(fd, sector.get(), quint64(size), 0)) {
                m_boot.findings.prepend(boot::checkFirstSector(QByteArray(sector.get(), 512), false));
                showBoot();
            }
            ::close(fd);
        });
    }
    const Volume *esp = nullptr;
    for (const Volume &v : disk->volumes) {
        if (v.isEfi && v.canMount())
            esp = &v;
    }
    if (!esp)
        return;
    m_espPath = esp->objectPath;
    const QString partition = esp->partUuid;
    if (!esp->mounts().isEmpty()) {
        m_espReadOnly = false;
        checkEfiPartition(esp->mounts().first(), windows);
        return;
    }
    // Mounted read-only to look at, like the drive itself.
    const QString objectPath = esp->objectPath;
    auto mounted = std::make_shared<QMetaObject::Connection>();
    *mounted = connect(m_udisks, &UDisks::changed, this, [this, objectPath, windows, mounted] {
        for (const Disk &d : m_udisks->disks()) {
            for (const Volume &v : d.volumes) {
                if (v.objectPath != objectPath || v.mounts().isEmpty())
                    continue;
                disconnect(*mounted);
                if (!m_mountedHere.contains(objectPath))
                    m_mountedHere << objectPath;
                m_espReadOnly = kReadOnlyMounts;
                checkEfiPartition(v.mounts().first(), windows);
                return;
            }
        }
    });
    m_udisks->mount(*esp, kReadOnlyMounts);
    Q_UNUSED(partition)
}

void VirusDialog::checkEfiPartition(const QString &esp, const QString &windows)
{
    m_espMount = esp;
    m_pages->setCurrentIndex(1);
    QString partition;
    for (const Disk &d : m_udisks->disks()) {
        for (const Volume &v : d.volumes) {
            if (v.objectPath == m_espPath)
                partition = v.partUuid;
        }
    }
    const efi::Trust trust = efi::trust();
    boot::Report report = boot::checkEfiPartition(esp, windows, trust);
    report.findings += boot::checkFirmware(trust, {});
    // This PC's boot entries for that partition: when the drive is this PC's own.
    if (!partition.isEmpty())
        report.findings += boot::checkBootEntries(efi::bootEntries(), partition, esp);
    for (const boot::Finding &f : std::as_const(m_boot.findings)) {
        if (f.where == tr("Sector 0") || f.name == QLatin1String("The first sector"))
            report.findings.prepend(f);
    }
    m_boot = report;
    for (int i = 0; i < m_boot.findings.size(); ++i) {
        if (!m_boot.findings[i].cleanCopy.isEmpty())
            m_bootTicked.insert(i);
    }
    // fwupd, where there is one, says how well the firmware itself is protected; it takes a
    // few seconds, so it's added when it answers.
    const QString fwupd = askFwupd ? QStandardPaths::findExecutable(QStringLiteral("fwupdmgr")) : QString();
    if (!fwupd.isEmpty()) {
        auto *p = new QProcess(this);
        connect(p, &QProcess::finished, this, [this, p] {
            p->deleteLater();
            m_boot.findings += boot::checkFirmware(efi::Trust(), p->readAllStandardOutput());
            showBoot();
        });
        p->start(fwupd, {QStringLiteral("security"), QStringLiteral("--json")});
    }
    QStringList lines{QStringLiteral("  boot: %1 things looked at on %2").arg(m_boot.findings.size()).arg(esp)};
    for (const boot::Finding &f : std::as_const(m_boot.findings)) {
        if (f.severity > 0)
            lines << QStringLiteral("  boot, looks wrong (%1): %2 %3 - %4").arg(f.severity).arg(f.where, f.verdict, f.reasons.join(QStringLiteral(" / ")));
    }
    log(lines);
    showBoot();
}

void VirusDialog::showBoot()
{
    m_bootList->clear();
    int odd = 0;
    QVector<int> order;
    for (int i = 0; i < m_boot.findings.size(); ++i) {
        order << i;
        odd += m_boot.findings[i].severity > 0;
    }
    std::stable_sort(order.begin(), order.end(), [this](int a, int b) { return m_boot.findings[a].severity > m_boot.findings[b].severity; });
    for (int i : std::as_const(order)) {
        const boot::Finding &f = m_boot.findings[i];
        auto *item = new QTreeWidgetItem({f.name, f.kind, f.reasons.isEmpty() ? f.verdict : f.reasons.first()});
        item->setData(0, Qt::UserRole, i);
        if (f.severity > 0) {
            item->setIcon(0, severityIcon(f.severity));
            item->setForeground(2, severityColor(f.severity));
        }
        const auto done = m_putBackDone.constFind(i);
        if (done != m_putBackDone.constEnd() && done->first) {
            item->setText(2, done->second);
            item->setForeground(2, QColor(0x2b, 0xee, 0x8a));
        } else if (!f.cleanCopy.isEmpty()) {
            item->setCheckState(0, m_bootTicked.contains(i) ? Qt::Checked : Qt::Unchecked);
            if (done != m_putBackDone.constEnd())
                item->setText(2, done->second);
        }
        m_bootList->addTopLevelItem(item);
    }
    if (m_bootList->topLevelItemCount() > 0)
        m_bootList->setCurrentItem(m_bootList->topLevelItem(0));
    showBootDetails();
    m_tabs->setTabVisible(2, !m_boot.findings.isEmpty());
    m_tabs->setTabText(2, odd ? tr("Boot (%n wrong)", nullptr, odd) : tr("Boot"));
    QString info = odd ? tr("Something that starts the PC before Windows looks wrong. Pick it to see why.")
                       : tr("What starts the PC before Windows looks as it should.");
    for (const QString &note : std::as_const(m_boot.notes))
        info += QStringLiteral("<br><small>") + note.toHtmlEscaped() + QStringLiteral("</small>");
    m_bootInfo->setText(info);
    m_putBack->setEnabled(!m_bootTicked.isEmpty() && !m_busy);
    bool lookable = false;
    for (const boot::Finding &f : std::as_const(m_boot.findings))
        lookable = lookable || (f.severity > 0 && !f.file.isEmpty());
    m_lookUp->setVisible(lookable || m_lookUp->isVisible());
}

void VirusDialog::showBootDetails()
{
    const QTreeWidgetItem *item = m_bootList->currentItem();
    if (!item) {
        m_bootDetails->clear();
        return;
    }
    const boot::Finding &f = m_boot.findings.at(item->data(0, Qt::UserRole).toInt());
    QString html = QStringLiteral("<b>%1</b>: %2<br>%3").arg(f.where.toHtmlEscaped(), f.kind.toHtmlEscaped(), f.verdict.toHtmlEscaped());
    if (!f.reasons.isEmpty()) {
        html += QStringLiteral("<ul style=\"margin: 4px 0\">");
        for (const QString &reason : f.reasons)
            html += QStringLiteral("<li>%1</li>").arg(reason.toHtmlEscaped());
        html += QStringLiteral("</ul>");
    }
    if (!f.cleanCopy.isEmpty())
        html += tr("Windows' own copy, which checks out, can be put back: %1").arg(QDir::toNativeSeparators(f.cleanCopy).toHtmlEscaped());
    if (m_online.contains(f.file))
        html += QStringLiteral("<br>") + tr("Looked up online: %1").arg(m_online.value(f.file).toHtmlEscaped());
    m_bootDetails->setText(html);
}

void VirusDialog::putBackTicked()
{
    QVector<int> picked(m_bootTicked.cbegin(), m_bootTicked.cend());
    picked.removeIf([this](int i) { return m_putBackDone.value(i).first || m_boot.findings.at(i).cleanCopy.isEmpty(); });
    std::sort(picked.begin(), picked.end());
    if (picked.isEmpty())
        return;
    QStringList shown;
    for (int i : std::as_const(picked))
        shown << QStringLiteral("• ") + m_boot.findings.at(i).where;
    const QString question = tr("Put Windows' own boot manager back over these?") + QStringLiteral("\n\n") + shown.join(QLatin1Char('\n'))
                           + QStringLiteral("\n\n") + tr("The ones there now are kept in a \"DiskAnvil backups\" folder on the EFI partition.");
    if (QMessageBox::question(this, windowTitle(), question, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    auto apply = [this, picked](const QString &mount) {
        const QString from = m_espMount;
        m_espMount = mount;
        const QString backup = mount + QStringLiteral("/DiskAnvil backups/") + QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH.mm.ss"));
        int done = 0;
        QStringList lines;
        for (int i : picked) {
            boot::Finding f = m_boot.findings.at(i);
            if (from != mount && f.file.startsWith(from + QLatin1Char('/')))
                f.file = mount + f.file.mid(from.size());
            QString message;
            const bool ok = boot::putBack(f, backup, &message);
            m_putBackDone.insert(i, {ok, message});
            if (ok) {
                ++done;
                m_bootTicked.remove(i);
            }
            lines << QStringLiteral("  boot: %1 %2: %3").arg(ok ? QStringLiteral("put back") : QStringLiteral("not put back"), f.where, message);
        }
        log(lines);
        showBoot();
        m_status->setText(done == picked.size() ? tr("Put back %n of Windows' boot files. The old ones are in %1.", nullptr, done).arg(QDir::toNativeSeparators(backup))
                                                : redText(tr("Put back %1 of %2; the list says why not the others.").arg(done).arg(picked.size())));
    };
    if (!m_espReadOnly) {
        apply(m_espMount);
        return;
    }
    remountWritable(m_espPath, [this, apply](const QString &mount) {
        m_espReadOnly = false;
        apply(mount);
    });
}

void VirusDialog::lookUpOnline()
{
    // The odd ones whose file is here, by their fingerprint.
    QHash<QString, QStringList> files; // SHA-256, the files with it
    auto add = [&files](const QString &path) {
        QFile f(path);
        if (path.isEmpty() || f.size() > (512 << 20) || !f.open(QIODevice::ReadOnly))
            return;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (hash.addData(&f))
            files[QString::fromLatin1(hash.result().toHex())] << path;
    };
    for (const startup::Entry &e : std::as_const(m_startup.entries)) {
        if (e.severity > 0)
            add(e.file);
    }
    for (const boot::Finding &f : std::as_const(m_boot.findings)) {
        if (f.severity > 0)
            add(f.file);
    }
    if (files.isEmpty()) {
        m_status->setText(tr("There's nothing odd here to look up."));
        return;
    }
    const QString question =
        tr("Look up %n program(s) online?", nullptr, int(files.size())) + QStringLiteral("\n\n")
        + tr("Only their fingerprints (SHA-256) are sent: not the files, their names, or anything about this PC. They go to "
             "CIRCL, Luxembourg's computer emergency response team, whose free hashlookup service knows tens of millions of "
             "files from software makers and Linux distributions.")
        + QStringLiteral("\n\n") + tr("A program it knows is very likely what it says it is. One it doesn't know isn't proof of anything.");
    if (QMessageBox::question(this, windowTitle(), question, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    m_lookUp->setEnabled(false);
    m_status->setText(tr("Looking them up…"));
    auto *lookup = new hashlookup::Lookup(this);
    auto known = std::make_shared<int>(0), unknown = std::make_shared<int>(0), failed = std::make_shared<int>(0);
    connect(lookup, &hashlookup::Lookup::answered, this, [this, files, known, unknown, failed](const hashlookup::Answer &a) {
        const QString words = a.failed ? tr("couldn't be asked (%1)").arg(a.what)
                            : a.known  ? tr("a known file: %1").arg(a.what)
                                       : tr("not a file the list knows");
        ++*(a.failed ? failed : a.known ? known : unknown);
        for (const QString &path : files.value(a.sha256))
            m_online.insert(path, words);
        log({QStringLiteral("  online: %1 %2").arg(a.sha256, words)});
    });
    connect(lookup, &hashlookup::Lookup::finished, this, [this, lookup, known, unknown, failed] {
        lookup->deleteLater();
        m_lookUp->setEnabled(true);
        showStartupDetails();
        showBootDetails();
        m_status->setText(*failed && !*known && !*unknown ? redText(tr("The lookup didn't work. Is there internet?"))
                                                          : tr("Looked up online: %1 known, %2 not known%3. Pick one to see what was said.")
                                                                .arg(*known)
                                                                .arg(*unknown)
                                                                .arg(*failed ? tr(", %1 couldn't be asked").arg(*failed) : QString()));
    });
    log({QStringLiteral("  online lookup of %1 files at %2").arg(files.size()).arg(hashlookup::server)});
    lookup->ask(files.keys());
}

void VirusDialog::switchOffTicked()
{
    QVector<int> picked;
    QStringList shown;
    for (int i : std::as_const(m_ticked)) {
        if (m_switched.value(i).first)
            continue;
        picked << i;
        const startup::Entry &e = m_startup.entries.at(i);
        if (shown.size() < 15)
            shown << QStringLiteral("• %1 (%2)").arg(e.name, e.where);
    }
    std::sort(picked.begin(), picked.end());
    if (picked.isEmpty())
        return;
    if (picked.size() > 15)
        shown << tr("…and %n more", nullptr, int(picked.size() - 15));
    const QString question = tr("Switch off these %n thing(s) that start with Windows?", nullptr, int(picked.size())) + QStringLiteral("\n\n")
                           + shown.join(QLatin1Char('\n')) + QStringLiteral("\n\n")
                           + tr("Everything changed is copied to a \"DiskAnvil backups\" folder on that drive first, with a note saying how to put "
                                "it back.");
    if (QMessageBox::question(this, windowTitle(), question, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    whenWritable([this, picked](const QString &from, const QString &to) {
        // On the drive that Windows is on; for the running Windows, its own drive.
        QString drive = m_windowsRoot;
#ifdef Q_OS_WIN
        if (drive.isEmpty())
            drive = QDir::fromNativeSeparators(qEnvironmentVariable("SystemDrive")) + QLatin1Char('/');
#endif
        if (drive.endsWith(QLatin1Char('/')))
            drive.chop(1);
        const QString backup = drive + QStringLiteral("/DiskAnvil backups/") + QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH.mm.ss"));
        auto moved = [&from, &to](QString path) {
            if (from != to && !path.isEmpty() && path.startsWith(from + QLatin1Char('/')))
                path = to + path.mid(from.size());
            return path;
        };
        QVector<startup::Entry> entries;
        for (int i : picked) {
            startup::Entry e = m_startup.entries.at(i);
            e.hiveFile = e.hiveFile.startsWith(QLatin1String("registry:")) ? e.hiveFile : moved(e.hiveFile);
            e.entryFile = moved(e.entryFile);
            entries << e;
        }
        const QVector<startup::Change> changes = startup::switchOff(entries, backup);
        int done = 0;
        QStringList lines{QStringLiteral("  switching off %1 startup entries, backup in %2").arg(picked.size()).arg(backup)};
        for (int n = 0; n < changes.size(); ++n) {
            m_switched.insert(picked[n], {changes[n].done, changes[n].message});
            if (changes[n].done) {
                ++done;
                m_ticked.remove(picked[n]);
            }
            lines << QStringLiteral("    %1 %2: %3").arg(changes[n].done ? QStringLiteral("switched off") : QStringLiteral("not switched off"), entries[n].name,
                                                       changes[n].message);
        }
        log(lines);
        showStartup();
        m_status->setText(done == changes.size()
                              ? tr("Switched off %n thing(s). The backup, and how to undo it, is in %1.", nullptr, done).arg(QDir::toNativeSeparators(backup))
                              : redText(tr("Switched off %1 of %2; the list says why not the others. The backup is in %3.")
                                            .arg(done)
                                            .arg(changes.size())
                                            .arg(QDir::toNativeSeparators(backup).toHtmlEscaped())));
        updateButtons();
    });
}

void VirusDialog::log(const QStringList &lines)
{
    const QString path = logPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write(lines.join(QLatin1Char('\n')).toUtf8() + '\n');
}
