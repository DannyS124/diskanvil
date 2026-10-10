// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "virusdialog.h"

#include "applog.h"
#include "dialogs.h"
#include "format.h"
#include "rescuestick.h"
#include "udisks.h"

#include <QButtonGroup>
#include <QColor>
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
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>

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
    // On DiskForge Live: with the session's logs, which go to the stick with Save Logs.
    if (rescue::runningInRescue() && QFileInfo(QStringLiteral("/run/diskforge-live/session")).isWritable())
        return QStringLiteral("/run/diskforge-live/session/virus-scan.log");
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/diskforge/virus-scan.log");
}

// Drives are mounted read-only for the scan where that's possible (Windows has no such thing
// for a drive letter).
#ifdef Q_OS_WIN
constexpr bool kReadOnlyMounts = false;
#else
constexpr bool kReadOnlyMounts = true;
#endif

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
             "DiskForge Live, where malware can't hide.")));
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
    resultsLayout->addWidget(m_hits, 1);
    auto *bottom = new QHBoxLayout;
    m_hitInfo->setWordWrap(true);
    m_hitInfo->setObjectName(QStringLiteral("hitInfo"));
    bottom->addWidget(m_hitInfo, 1);
    for (QPushButton *b : {m_again, m_delete}) {
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
        if (m_job)
            m_job->cancel();
        m_stop->setEnabled(false);
    });
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
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
    m_job = nullptr;
    m_updateJob = nullptr;
    m_thread = nullptr;
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
    p->start(QStringLiteral("diskforge-live-virus-list"), {QStringLiteral("load")});
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
    // A system list DiskForge can't write to: the update goes into DiskForge's own folder.
    const QString into = m_engine.databaseWritable ? m_engine.database
                                                   : QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/diskforge/clamav");
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
        // On DiskForge Live the new list goes onto the stick for next time.
        refreshEngine();
        auto *p = new QProcess(this);
        connect(p, &QProcess::finished, this, [this, p] {
            p->deleteLater();
            const QString said = QString::fromUtf8(p->readAll()).trimmed();
            if (!said.isEmpty())
                m_engineInfo->setText(m_engineInfo->text() + QStringLiteral("<br>") + said.toHtmlEscaped());
        });
        p->setProcessChannelMode(QProcess::MergedChannels);
        p->start(QStringLiteral("diskforge-live-virus-list"), {QStringLiteral("save")});
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
    m_job = new virus::Scan(m_engine, folders, virus::Depth(m_depth->checkedId()));
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
    connect(m_job, &virus::Scan::finished, this, &VirusDialog::scanFinished);
    m_thread = startOnThread(this, m_job);
    updateButtons();
}

void VirusDialog::addHit(const virus::Detection &d)
{
    ++m_found;
    const int severity = virus::severity(d.name);
    auto *item = new QTreeWidgetItem({shownPath(d.path), virus::describe(d.name)});
    item->setData(0, Qt::UserRole, d.path);
    item->setToolTip(0, QDir::toNativeSeparators(d.path));
    item->setToolTip(1, virus::shownName(d.name));
    item->setIcon(0, QIcon::fromTheme(severity >= 2 ? QStringLiteral("dialog-error") : QStringLiteral("dialog-warning")));
    item->setForeground(1, severityColor(severity));
    // Real malware is ticked to go; guesses and unwanted programs are left to decide.
    item->setCheckState(0, severity >= 2 ? Qt::Checked : Qt::Unchecked);
    m_hits->addTopLevelItem(item);
    log({QStringLiteral("  found %1: %2").arg(virus::shownName(d.name), d.path)});
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
        text = tr("Stopped. %n thing(s) found before that.", nullptr, m_found);
    else if (!completed)
        text = redText(tr("The scan didn't finish: %1").arg(problem.toHtmlEscaped()));
    else if (m_found == 0)
        text = scanned > 0 ? tr("Done: %L1 files looked at, and nothing found.").arg(scanned) : tr("Done, and nothing found.");
    else
        text = scanned > 0 ? tr("Done: found %n thing(s) in %L1 files.", nullptr, m_found).arg(scanned) : tr("Done: found %n thing(s).", nullptr, m_found);
    if (unreadable > 0)
        text += QLatin1Char(' ') + tr("%n file(s) couldn't be read: another user's or the system's, or in use.", nullptr, unreadable);
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

    auto remove = [this, paths, items](const QString &root) {
        int deleted = 0;
        QStringList failed;
        for (int i = 0; i < paths.size(); ++i) {
            // Mounted again, the drive can be somewhere else: the same file, under the new place.
            QString path = paths[i];
            if (!root.isEmpty() && m_folders.size() == 1 && path.startsWith(m_folders.first() + QLatin1Char('/')))
                path = root + path.mid(m_folders.first().size());
            QFile file(path);
            file.setPermissions(file.permissions() | QFileDevice::WriteOwner);
            if (file.remove()) {
                ++deleted;
                items[i]->setData(0, Qt::UserRole + 1, true);
                items[i]->setData(0, Qt::CheckStateRole, QVariant()); // nothing left to tick
                items[i]->setFlags(items[i]->flags() & ~Qt::ItemIsUserCheckable);
                items[i]->setIcon(0, QIcon::fromTheme(QStringLiteral("edit-delete")));
                items[i]->setText(1, tr("Deleted"));
                items[i]->setForeground(1, QColor(0x2b, 0xee, 0x8a));
                log({QStringLiteral("  deleted %1").arg(path)});
            } else {
                failed << QStringLiteral("%1 (%2)").arg(QDir::toNativeSeparators(path), file.errorString());
                log({QStringLiteral("  couldn't delete %1: %2").arg(path, file.errorString())});
            }
        }
        if (!root.isEmpty())
            m_folders = QStringList{root};
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

    if (!m_mountedReadOnly) {
        remove(QString());
        return;
    }
    // The drive was mounted read-only for the scan: unmounted, and mounted again so it can change.
    const Volume *volume = nullptr;
    for (const Disk &d : m_udisks->disks()) {
        for (const Volume &v : d.volumes) {
            if (v.objectPath == m_volumePath)
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
    const QString objectPath = m_volumePath;
    auto changed = std::make_shared<QMetaObject::Connection>();
    auto failed = std::make_shared<QMetaObject::Connection>();
    auto step = std::make_shared<int>(0); // 0: unmounting, 1: mounting again
    *changed = connect(m_udisks, &UDisks::changed, this, [this, objectPath, changed, failed, step, remove] {
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
                    m_mountedReadOnly = false;
                    remove(v.mounts().first());
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
    m_udisks->unmount(*volume);
}

void VirusDialog::log(const QStringList &lines)
{
    const QString path = logPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write(lines.join(QLatin1Char('\n')).toUtf8() + '\n');
}
