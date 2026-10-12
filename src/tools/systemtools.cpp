// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "systemtools.h"

#include "cleanup.h"
#include "dialogs.h"
#include "format.h"
#include "jobui.h"
#include "rescuestick.h"
#include "snapper.h"
#include "systemd.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QTextDocument>

namespace {

const QString kTrimService = QStringLiteral("fstrim.service");
const QString kTrimTimer = QStringLiteral("fstrim.timer");
const QString kPackageService = QStringLiteral("paccache.service");
const QString kUninstalledService = QStringLiteral("diskanvil-paccache-uninstalled.service");
const QString kJournalService = QStringLiteral("diskanvil-journal-vacuum.service");

#ifndef Q_OS_WIN // for Optimize, which is Windows' own there
// Mount options for a mount point, the per-mount and the file system's own together.
QString mountOptions(const QString &mountPoint)
{
    QFile f(QStringLiteral("/proc/self/mountinfo"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QString found;
    for (const QByteArray &line : f.readAll().split('\n')) {
        const QList<QByteArray> fields = line.split(' ');
        const int dash = fields.indexOf("-");
        if (dash < 6 || dash + 3 >= fields.size())
            continue;
        if (QString::fromLocal8Bit(fields[4]).replace(QStringLiteral("\\040"), QStringLiteral(" ")) == mountPoint)
            found = QString::fromLocal8Bit(fields[5] + ',' + fields[dash + 3]); // the last mount there wins
    }
    return found;
}

QString when(quint64 usec)
{
    if (usec == 0)
        return QObject::tr("never");
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(qint64(usec / 1000)), QLocale::ShortFormat);
}
#endif

QTreeWidget *plainList(const QStringList &headers)
{
    auto *list = new QTreeWidget;
    list->setHeaderLabels(headers);
    list->setRootIsDecorated(false);
    list->setAlternatingRowColors(true);
    list->setSelectionMode(QAbstractItemView::NoSelection);
    return list;
}

} // namespace

#ifndef Q_OS_WIN // Windows' are in win/systemtools_win.cpp

// --- Optimize -----------------------------------------------------------------

OptimizeDialog::OptimizeDialog(UDisks *udisks, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_systemd(new Systemd(this))
    , m_list(plainList({tr("Drive"), tr("Kind"), tr("Status")}))
    , m_last(new QLabel)
    , m_weekly(new QCheckBox(tr("Optimize every week")))
    , m_now(new QPushButton(tr("Optimize Now")))
    , m_status(new QLabel)
{
    setWindowTitle(tr("Optimize Drives"));
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("<p>SSDs and flash drives stay fast when they're told which space is free (this is called "
                                       "TRIM). Optimizing does that for every mounted drive that supports it.</p>")));
    layout->addWidget(m_list, 1);
    layout->addWidget(wrappingLabel(tr("<small>Hard drives don't need it, and Linux file systems don't need defragmenting the way "
                                       "Windows does. On Btrfs, defragmenting would also undo the space that snapshots share, so "
                                       "DiskAnvil leaves it alone.</small>")));
    auto *row = new QHBoxLayout;
    row->addWidget(m_weekly);
    row->addStretch();
    row->addWidget(m_last);
    layout->addLayout(row);
    layout->addWidget(m_status);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    box->addButton(m_now, QDialogButtonBox::ActionRole);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(box);

    connect(m_now, &QPushButton::clicked, this, [this] {
        m_now->setEnabled(false);
        m_status->setText(tr("Optimizing…"));
        m_systemd->startUnit(kTrimService, [this](bool ok, const QString &message) {
            m_now->setEnabled(true);
            m_status->setText(ok ? tr("Done.") : tr("Couldn't optimize: %1").arg(message));
            refresh();
        });
    });
    connect(m_weekly, &QCheckBox::clicked, this, [this](bool on) {
        m_weekly->setEnabled(false);
        m_systemd->setTimerEnabled(kTrimTimer, on, [this](bool ok, const QString &message) {
            m_weekly->setEnabled(true);
            if (!ok)
                m_status->setText(tr("Couldn't change that: %1").arg(message));
            refresh();
        });
    });
    refresh();
    resize(640, 420);
}

void OptimizeDialog::refresh()
{
    const bool haveUnit = m_systemd->unitExists(kTrimService);
    const bool weekly = m_systemd->unitFileState(kTrimTimer) == QLatin1String("enabled");
    {
        const QSignalBlocker block(m_weekly);
        m_weekly->setChecked(weekly);
    }
    m_weekly->setEnabled(m_systemd->unitExists(kTrimTimer) && !rescue::runningInRescue());
    if (rescue::runningInRescue())
        m_weekly->setToolTip(rescue::notInRescueReason());
    m_now->setEnabled(haveUnit);
    if (!haveUnit)
        m_status->setText(tr("fstrim isn't installed (it comes with util-linux)."));
    m_last->setText(tr("Last optimized: %1").arg(when(std::max(m_systemd->timerLastTrigger(kTrimTimer), m_systemd->serviceLastRun(kTrimService)))));

    m_list->clear();
    for (const Disk &d : m_udisks->disks()) {
        if (d.isLoop)
            continue;
        for (const Volume &v : d.volumes) {
            const QStringList mounts = v.mounts();
            if (mounts.isEmpty())
                continue;
            QString status;
            if (!d.discard)
                status = d.rotationRate > 0 ? tr("Hard drive: doesn't need it") : tr("Doesn't support it");
            else if (mountOptions(mounts.first()).contains(QLatin1String("discard")))
                status = tr("Trims as it goes");
            else
                status = weekly ? tr("Trimmed every week") : tr("Can be optimized");
            auto *item = new QTreeWidgetItem(m_list, {QStringLiteral("%1 %2").arg(volumeTitle(v), mounts.first()), diskKind(d), status});
            item->setToolTip(0, Qt::convertFromPlainText(d.model, Qt::WhiteSpaceNormal));
        }
    }
    for (int c = 0; c < 3; ++c)
        m_list->resizeColumnToContents(c);
}

// --- Cleanup ------------------------------------------------------------------

namespace {

QString userCache()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
}

QStringList mountedPoints(const UDisks *udisks)
{
    QStringList points;
    for (const Disk &d : udisks->disks()) {
        for (const Volume &v : d.volumes)
            points += v.mounts();
    }
    return points;
}

// Works out the sizes on a worker thread: paccache's dry run and walking big folders
// both take a moment.
class SizeWorker : public QObject
{
    Q_OBJECT
public:
    explicit SizeWorker(const QStringList &mounts) : m_mounts(mounts) {}
public slots:
    void run()
    {
        CleanupDialog::Sizes s;
        s.packages = cleanup::packageCacheReclaimable();
        s.uninstalled = cleanup::packageCacheReclaimable(true);
        s.packageCache = cleanup::folderSize(QStringLiteral("/var/cache/pacman/pkg"));
        s.journal = cleanup::folderSize(QStringLiteral("/var/log/journal"));
        s.userCache = cleanup::folderSize(userCache());
        for (const QString &t : cleanup::trashFolders(m_mounts)) {
            s.trash += cleanup::folderSize(t + QStringLiteral("/files"));
            s.trash += cleanup::folderSize(t + QStringLiteral("/info"));
        }
        emit done(s);
    }
signals:
    void done(const CleanupDialog::Sizes &sizes);

private:
    QStringList m_mounts;
};

class DeleteWorker : public QObject
{
    Q_OBJECT
public:
    DeleteWorker(bool cache, const QStringList &trash) : m_cache(cache), m_trash(trash) {}
public slots:
    void run()
    {
        cleanup::Result total;
        auto add = [&total](const cleanup::Result &r) {
            total.freed += r.freed;
            total.failed += r.failed;
            if (total.firstError.isEmpty())
                total.firstError = r.firstError;
        };
        if (m_cache)
            add(cleanup::removeContents(userCache()));
        for (const QString &t : m_trash)
            add(cleanup::emptyTrash(t));
        emit done(total.failed, total.firstError);
    }
signals:
    void done(int failed, const QString &firstError);

private:
    bool m_cache;
    QStringList m_trash;
};

} // namespace

CleanupDialog::CleanupDialog(UDisks *udisks, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_systemd(new Systemd(this))
    , m_packages(new QCheckBox(tr("Older package versions")))
    , m_uninstalled(new QCheckBox(tr("Packages you've uninstalled")))
    , m_journal(new QCheckBox(tr("System logs")))
    , m_cache(new QCheckBox(tr("Your cache folder")))
    , m_trash(new QCheckBox(tr("Trash")))
    , m_packagesSize(new QLabel)
    , m_uninstalledSize(new QLabel)
    , m_journalSize(new QLabel)
    , m_cacheSize(new QLabel)
    , m_trashSize(new QLabel)
    , m_note(new QLabel)
    , m_status(new QLabel)
    , m_clean(new QPushButton(tr("Clean Up")))
{
    qRegisterMetaType<CleanupDialog::Sizes>();
    setWindowTitle(tr("Disk Cleanup"));
    m_packages->setChecked(true);
    m_uninstalled->setChecked(true);
    m_journal->setChecked(true);

    auto *grid = new QGridLayout;
    int row = 0;
    auto add = [&](QCheckBox *box, QLabel *size, const QString &explain) {
        size->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(box, row, 0);
        grid->addWidget(size, row++, 1);
        auto *label = wrappingLabel(QStringLiteral("<small>%1</small>").arg(explain));
        label->setContentsMargins(24, 0, 0, 6);
        grid->addWidget(label, row++, 0, 1, 2);
    };
    add(m_packages, m_packagesSize, tr("Copies pacman keeps after updates. The newest 3 of each package stay, so you can still go back."));
    add(m_uninstalled, m_uninstalledSize, tr("Downloaded files of packages that aren't installed anymore."));
    add(m_journal, m_journalSize, tr("Old entries in the system log. The newest %1 are kept.").arg(formatSize(cleanup::kJournalKeep)));
    add(m_cache, m_cacheSize, tr("Thumbnails, browser caches and the like in %1. Apps rebuild what they need; close your browser first.")
                                  .arg(userCache().toHtmlEscaped()));
    add(m_trash, m_trashSize, tr("Files you deleted, in your home folder and on mounted drives. They're gone for good afterwards."));
    grid->setColumnStretch(0, 1);

    m_note->setWordWrap(true);
    m_status->setWordWrap(true);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("<p>Frees space by removing things that are safe to remove.</p>")));
    layout->addLayout(grid);
    layout->addWidget(m_note);
    layout->addStretch();
    layout->addWidget(m_status);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    box->addButton(m_clean, QDialogButtonBox::ActionRole);
    connect(m_clean, &QPushButton::clicked, this, &CleanupDialog::clean);
    connect(box, &QDialogButtonBox::rejected, this, &CleanupDialog::reject);
    layout->addWidget(box);

    // Space held by snapshots only comes back once those snapshots are gone.
    for (const Subvolume &s : snapper::mountedSubvolumes()) {
        if (s.mountPoint == QLatin1String("/") || s.mountPoint == QLatin1String("/home")) {
            m_note->setText(tr("<small>Your system is on Btrfs. If snapshots were taken, the space they still hold only comes back "
                               "when those snapshots are removed (snapper does that on its own over time).</small>"));
            break;
        }
    }
    for (QCheckBox *c : {m_packages, m_uninstalled, m_journal, m_cache, m_trash})
        connect(c, &QCheckBox::toggled, this, [this] { showSizes(m_sizes); });
    resize(600, sizeHint().height());
    measure();
}

CleanupDialog::~CleanupDialog()
{
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
}

void CleanupDialog::reject()
{
    if (!m_busy)
        QDialog::reject();
}

void CleanupDialog::setBusy(bool busy, const QString &text)
{
    m_busy = busy;
    for (QWidget *w : std::initializer_list<QWidget *>{m_packages, m_uninstalled, m_journal, m_cache, m_trash, m_clean})
        w->setEnabled(!busy);
    if (!text.isEmpty())
        m_status->setText(text);
    if (!busy)
        showSizes(m_sizes);
}

void CleanupDialog::measure()
{
    setBusy(true, tr("Working out how much can go…"));
    for (QLabel *l : {m_packagesSize, m_uninstalledSize, m_journalSize, m_cacheSize, m_trashSize})
        l->setText(QStringLiteral("…"));
    auto *worker = new SizeWorker(mountedPoints(m_udisks));
    connect(worker, &SizeWorker::done, this, [this](const Sizes &sizes) {
        m_thread->quit();
        m_thread->wait();
        m_thread = nullptr;
        m_sizes = sizes;
        setBusy(false);
        m_status->clear();
    });
    m_thread = startOnThread(this, worker);
}

void CleanupDialog::showSizes(const Sizes &s)
{
    const quint64 journalExtra = s.journal > cleanup::kJournalKeep ? s.journal - cleanup::kJournalKeep : 0;
    auto amount = [](qint64 bytes) { return bytes > 0 ? formatSize(quint64(bytes)) : tr("nothing to remove"); };
    m_packagesSize->setText(s.packages < 0 ? tr("needs pacman-contrib") : amount(s.packages));
    m_uninstalledSize->setText(s.uninstalled < 0 ? tr("needs pacman-contrib") : amount(s.uninstalled));
    m_journalSize->setText(amount(qint64(journalExtra)));
    m_cacheSize->setText(amount(qint64(s.userCache)));
    m_trashSize->setText(amount(qint64(s.trash)));
    if (m_busy)
        return;

    const bool packageUnit = s.packages >= 0 && m_systemd->unitExists(kPackageService);
    const bool uninstalledUnit = s.uninstalled >= 0 && m_systemd->unitExists(kUninstalledService);
    const bool journalUnit = m_systemd->unitExists(kJournalService);
    m_packages->setEnabled(packageUnit && s.packages > 0);
    m_uninstalled->setEnabled(uninstalledUnit && s.uninstalled > 0);
    m_journal->setEnabled(journalUnit && journalExtra > 0);
    m_cache->setEnabled(s.userCache > 0);
    m_trash->setEnabled(s.trash > 0);
    for (QCheckBox *c : {m_journal, m_uninstalled})
        c->setToolTip(c->isEnabled() || (c == m_journal ? journalUnit : uninstalledUnit) ? QString()
                                                                                       : tr("Comes with DiskAnvil's package; it isn't installed here"));
    quint64 total = 0;
    total += m_packages->isEnabled() && m_packages->isChecked() ? quint64(s.packages) : 0;
    total += m_uninstalled->isEnabled() && m_uninstalled->isChecked() ? quint64(s.uninstalled) : 0;
    total += m_journal->isEnabled() && m_journal->isChecked() ? journalExtra : 0;
    total += m_cache->isEnabled() && m_cache->isChecked() ? s.userCache : 0;
    total += m_trash->isEnabled() && m_trash->isChecked() ? s.trash : 0;
    m_clean->setEnabled(total > 0);
    m_clean->setText(total > 0 ? tr("Free %1").arg(formatSize(total)) : tr("Clean Up"));
}

void CleanupDialog::clean()
{
    const bool packages = m_packages->isEnabled() && m_packages->isChecked();
    const bool uninstalled = m_uninstalled->isEnabled() && m_uninstalled->isChecked();
    const bool journal = m_journal->isEnabled() && m_journal->isChecked();
    const bool cache = m_cache->isEnabled() && m_cache->isChecked();
    const bool trash = m_trash->isEnabled() && m_trash->isChecked();
    if ((cache || trash)
        && QMessageBox::question(this, windowTitle(), tr("Delete them? This can't be undone.")) != QMessageBox::Yes)
        return;
    const Sizes &s = m_sizes;
    auto counted = [](const Sizes &x) {
        return x.userCache + x.trash + quint64(std::max<qint64>(x.packages, 0)) + quint64(std::max<qint64>(x.uninstalled, 0)) + x.journal;
    };
    m_before = counted(s);
    m_problems.clear();

    QStringList units;
    if (packages)
        units << kPackageService;
    if (uninstalled)
        units << kUninstalledService;
    if (journal)
        units << kJournalService;
    auto afterwards = [this, counted] {
        // Measure again; what changed is what was freed.
        auto *worker = new SizeWorker(mountedPoints(m_udisks));
        connect(worker, &SizeWorker::done, this, [this, counted](const Sizes &now) {
            m_thread->quit();
            m_thread->wait();
            m_thread = nullptr;
            m_sizes = now;
            const quint64 after = counted(now);
            setBusy(false, tr("Freed about %1.").arg(formatSize(m_before > after ? m_before - after : 0))
                               + (m_problems.isEmpty() ? QString() : QLatin1Char(' ') + m_problems.join(QLatin1Char(' '))));
        });
        m_thread = startOnThread(this, worker);
    };

    setBusy(true, tr("Cleaning up…"));
    if (!cache && !trash)
        return startUnits(units, afterwards);
    auto *worker = new DeleteWorker(cache, trash ? cleanup::trashFolders(mountedPoints(m_udisks)) : QStringList());
    connect(worker, &DeleteWorker::done, this, [this, units, afterwards](int failed, const QString &firstError) {
        m_thread->quit();
        m_thread->wait();
        m_thread = nullptr;
        if (failed > 0)
            m_problems << tr("%n item(s) couldn't be deleted (first: %1).", nullptr, failed).arg(firstError);
        startUnits(units, afterwards);
    });
    m_thread = startOnThread(this, worker);
}

void CleanupDialog::startUnits(QStringList units, const std::function<void()> &done)
{
    if (units.isEmpty())
        return done();
    const QString unit = units.takeFirst();
    m_systemd->startUnit(unit, [this, units, done, unit](bool ok, const QString &message) {
        if (!ok)
            m_problems << tr("%1 didn't run: %2.").arg(unit, message);
        startUnits(units, done);
    });
}

#endif // !Q_OS_WIN

// --- Btrfs snapshots ----------------------------------------------------------

SnapshotsDialog::SnapshotsDialog(QWidget *parent)
    : QDialog(parent)
    , m_subvolumes(plainList({tr("Subvolume"), tr("Mounted at"), tr("Drive"), tr("ID")}))
    , m_configs(new QComboBox)
    , m_snapshots(plainList({tr("#"), tr("Taken"), tr("Kind"), tr("Description"), tr("Kept by")}))
    , m_snapperNote(new QLabel)
{
    setWindowTitle(tr("Btrfs Snapshots"));
    for (const Subvolume &s : snapper::mountedSubvolumes())
        new QTreeWidgetItem(m_subvolumes, {s.path, s.mountPoint, shortDevice(s.device), s.id ? QString::number(s.id) : QString()});
    for (int c = 0; c < 4; ++c)
        m_subvolumes->resizeColumnToContents(c);
    m_snapperNote->setWordWrap(true);
    m_snapperNote->setTextFormat(Qt::RichText);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("<p><b>Subvolumes</b> are the parts a Btrfs drive is split into, each mounted somewhere. "
                                       "<b>Snapshots</b> are frozen copies of one, taken by snapper; they share space with "
                                       "the live files until those change.</p>")));
    layout->addWidget(m_subvolumes, 1);
    auto *configRow = new QHBoxLayout;
    configRow->addWidget(new QLabel(tr("Snapper snapshots of:")));
    configRow->addWidget(m_configs, 1);
    layout->addLayout(configRow);
    layout->addWidget(m_snapshots, 2);
    layout->addWidget(m_snapperNote);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(box);

    if (!snapper::available()) {
        m_configs->setEnabled(false);
        m_snapperNote->setText(tr("Snapper isn't installed. It takes snapshots of Btrfs on a schedule and before updates; "
                                  "see the Arch Wiki page on Snapper."));
    } else {
        QString error;
        for (const SnapperConfig &c : snapper::configs(&error))
            m_configs->addItem(QStringLiteral("%1 (%2)").arg(c.subvolume, c.name), c.name);
        if (!error.isEmpty())
            m_snapperNote->setText(redText(error));
        else if (m_configs->count() == 0)
            m_snapperNote->setText(tr("Snapper is installed but has nothing set up yet."));
        connect(m_configs, &QComboBox::currentIndexChanged, this, &SnapshotsDialog::showSnapshots);
        showSnapshots();
    }
    resize(760, 600);
}

void SnapshotsDialog::showSnapshots()
{
    m_snapshots->clear();
    const QString config = m_configs->currentData().toString();
    if (config.isEmpty())
        return;
    QString error;
    const QVector<Snapshot> list = snapper::snapshots(config, &error);
    for (const Snapshot &s : list) {
        if (s.number == 0)
            continue; // "current": the live system, not a snapshot
        QString kind = tr("Single");
        if (s.type == Snapshot::Type::Pre)
            kind = tr("Before");
        else if (s.type == Snapshot::Type::Post)
            kind = tr("After #%1").arg(s.preNumber);
        new QTreeWidgetItem(m_snapshots, {QString::number(s.number), QLocale().toString(s.date, QLocale::ShortFormat), kind,
                                          s.description, s.cleanup});
    }
    for (int c = 0; c < 5; ++c)
        m_snapshots->resizeColumnToContents(c);
    m_snapshots->scrollToBottom();
    if (!error.isEmpty()) {
        m_snapperNote->setText(redText(error));
        return;
    }
    m_snapperNote->setText(tr("<small>%n snapshot(s). Sizes aren't shown: Btrfs only knows them with quotas switched on, which "
                              "slows it down. To go back to a snapshot, use snapper or the snapshot entries in your boot menu.</small>",
                              nullptr, int(m_snapshots->topLevelItemCount())));
}


#include "systemtools.moc"
