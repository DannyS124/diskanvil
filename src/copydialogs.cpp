// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "copydialogs.h"

#include "applog.h"
#include "blockio.h"
#include "blockmapwidget.h"
#include "dialogs.h"
#include "format.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QTimer>
#include <QGroupBox>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr quint64 MiB = 1024 * 1024;

struct Found {
    const Disk *disk = nullptr;
    const Volume *volume = nullptr; // null: the whole drive
};

Found lookUp(const UDisks *udisks, const QString &objectPath)
{
    for (const Disk &d : udisks->disks()) {
        if (d.blockPath == objectPath)
            return {&d, nullptr};
        for (const Volume &v : d.volumes) {
            if (v.objectPath == objectPath)
                return {&d, &v};
        }
    }
    return {};
}

bool isSlow(const Disk &d)
{
    return d.rotationRate > 0 || d.bus == QLatin1String("usb") || d.removable;
}

// Rough copy speed between two drives: the slower one sets the pace.
double copySeconds(quint64 bytes, const Disk &from, const Disk *to = nullptr)
{
    const bool slow = isSlow(from) || (to && isSlow(*to));
    return double(bytes) / (slow ? 80e6 : 350e6);
}

QString safeName(QString name)
{
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("-"));
    name.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    return name.isEmpty() ? QStringLiteral("drive") : name;
}

QString whatTitle(const Found &f)
{
    if (!f.disk)
        return {};
    return f.volume ? QObject::tr("%1 on %2").arg(describeVolume(*f.volume), f.disk->model) : diskTitle(*f.disk);
}

QLabel *noteLabel(const QString &text)
{
    auto *label = wrappingLabel(QStringLiteral("<small>%1</small>").arg(text));
    label->setContentsMargins(24, 0, 0, 4);
    return label;
}

// Type-the-name confirmation shared by everything that overwrites a drive.
bool confirmMatches(QLabel *warning, QLineEdit *confirm, const QString &erases, const Disk &disk, const QString &name)
{
    QString text = redText(erases);
    if (!diskWarning(disk).isEmpty())
        text += QStringLiteral("<br>") + redText(diskWarning(disk));
    text += QStringLiteral("<br>") + QObject::tr("Type <b>%1</b> to confirm:").arg(name);
    warning->setText(text);
    confirm->setPlaceholderText(name);
    return confirm->text().trimmed() == name;
}

} // namespace

// --- Clone --------------------------------------------------------------------

CloneDialog::CloneDialog(UDisks *udisks, const QString &sourceBlockPath, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_source(sourceBlockPath)
    , m_targets(new QComboBox)
    , m_plan(new QLabel)
    , m_replace(new QRadioButton(tr("The copy replaces the old drive")))
    , m_keepBoth(new QRadioButton(tr("I'll keep using both drives")))
    , m_grow(new QCheckBox)
    , m_verify(new QCheckBox(tr("Check the copy afterwards (takes about as long again)")))
    , m_warning(new QLabel)
    , m_confirm(new QLineEdit)
    , m_progress(new QProgressBar)
    , m_phase(new QLabel)
    , m_meter(m_progress, m_phase)
{
    setWindowTitle(tr("Clone Drive"));
    const Disk *source = m_udisks->diskByPath(sourceBlockPath);
    auto *intro = wrappingLabel(tr("<p>Copies <b>%1</b> onto another drive: its partition table, its boot area and every "
                                   "partition, exactly as they are. Empty space is skipped.</p>")
                                    .arg(source ? diskTitle(*source).toHtmlEscaped() : QString()));
    m_plan->setWordWrap(true);
    m_plan->setTextFormat(Qt::RichText);
    m_replace->setChecked(true);
    m_grow->setChecked(true);
    m_verify->setChecked(true);
    m_warning->setWordWrap(true);
    m_warning->setTextFormat(Qt::RichText);
    m_progress->setVisible(false);

    auto *form = new QFormLayout;
    form->addRow(tr("Copy to:"), m_targets);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(form);
    layout->addWidget(m_plan);
    layout->addSpacing(6);
    layout->addWidget(m_replace);
#ifdef Q_OS_WIN
    layout->addWidget(noteLabel(tr("An exact copy, IDs included. Afterwards, don't keep both plugged in at the same time: "
                                   "Windows takes one of two drives with the same IDs offline.")));
#else
    layout->addWidget(noteLabel(tr("An exact copy, IDs included. Afterwards, don't keep both plugged in at the same time: "
                                   "Linux can't tell them apart and may use the wrong one.")));
#endif
    layout->addWidget(m_keepBoth);
    layout->addWidget(noteLabel(tr("The copy gets new IDs, so both drives can be plugged in at once. "
                                   "Encrypted partitions keep theirs.")));
    layout->addWidget(m_grow);
    layout->addWidget(m_verify);
    layout->addWidget(m_warning);
    layout->addWidget(m_confirm);
    layout->addWidget(m_phase);
    layout->addWidget(m_progress);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_start = box->addButton(tr("Clone"), QDialogButtonBox::ActionRole);
    m_start->setAutoDefault(false);
    box->button(QDialogButtonBox::Cancel)->setDefault(true);
    connect(m_start, &QPushButton::clicked, this, &CloneDialog::start);
    connect(box, &QDialogButtonBox::rejected, this, &CloneDialog::reject);
    layout->addWidget(box);

    fillTargets();
    connect(m_targets, &QComboBox::currentIndexChanged, this, &CloneDialog::updateState);
    connect(m_confirm, &QLineEdit::textChanged, this, &CloneDialog::updateState);
    connect(m_udisks, &UDisks::changed, this, [this] {
        if (!m_running)
            updateState();
    });
    updateState();
    resize(620, sizeHint().height());
}

CloneDialog::~CloneDialog()
{
    disconnect(m_opConn);
    if (m_job)
        m_job->cancel();
    stopThread();
}

void CloneDialog::stopThread()
{
    if (!m_thread)
        return;
    m_thread->quit();
    m_thread->wait();
    m_thread = nullptr;
    m_job = nullptr;
}

void CloneDialog::fillTargets()
{
    const QVector<Disk> &disks = m_udisks->disks();
    for (int i = 0; i < disks.size(); ++i) {
        const Disk &d = disks[i];
        if (d.blockPath == m_source || d.isSystem || d.readOnly)
            continue;
        m_targets->addItem(tr("Disk %1: %2").arg(i).arg(diskTitle(d)), d.blockPath);
    }
    if (m_targets->count() == 0)
        m_targets->addItem(tr("No other drive to copy to (plug one in)"));
}

void CloneDialog::updateState()
{
    // Shown or hidden once, at the end: a box hidden while it has the keyboard loses it.
    bool asked = false;
    bool grow = false;
    const Disk *source = m_udisks->diskByPath(m_source);
    const Disk *target = m_udisks->diskByPath(m_targets->currentData().toString());
    bool ok = false;
    if (!source) {
        m_plan->setText(redText(tr("The drive to copy isn't there anymore.")));
    } else if (!target) {
        m_plan->clear();
    } else {
        const diskclone::Plan plan = diskclone::plan(*source, *target);
        if (!plan.error.isEmpty()) {
            m_plan->setText(redText(plan.error));
        } else {
            int partitions = 0;
            for (const Volume &v : source->volumes)
                partitions += !v.isContainer;
            const double seconds = copySeconds(plan.bytes, *source, target) * (m_verify->isChecked() ? 2 : 1);
            m_plan->setText(source->tableType.isEmpty()
                                ? tr("Copies all %1. Takes %2.").arg(formatSize(plan.bytes), durationText(seconds))
                                : tr("Copies %n partition(s), %1 in all. Takes %2.", nullptr, partitions)
                                      .arg(formatSize(plan.bytes), durationText(seconds)));
            if (!source->tableType.isEmpty() && target->size > source->size + MiB) {
                m_grow->setText(tr("Let the last partition use the extra %1").arg(formatSize(target->size - source->size)));
                grow = true;
            }
            ok = confirmMatches(m_warning, m_confirm, tr("Everything on %1 will be erased.").arg(diskTitle(*target)), *target,
                                shortDevice(target->device));
            asked = true;
        }
    }
    m_grow->setVisible(grow);
    m_warning->setVisible(asked);
    m_confirm->setVisible(asked);
    m_start->setEnabled(ok && !m_running);
    fitHeight(this);
}

void CloneDialog::setRunning(bool running)
{
    m_running = running;
    for (QWidget *w : std::initializer_list<QWidget *>{m_targets, m_replace, m_keepBoth, m_grow, m_verify, m_confirm})
        w->setEnabled(!running);
    m_start->setEnabled(!running);
}

void CloneDialog::reject()
{
    if (m_job) {
        const auto answer = QMessageBox::warning(this, windowTitle(),
                                                 tr("Stop cloning? The target drive will hold a partial copy and needs formatting before it's used."),
                                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes)
            m_job->cancel(); // finished() closes the dialog
        return;
    }
    if (m_running)
        return; // finishing up (new IDs, growing); it takes seconds
    QDialog::reject();
}

void CloneDialog::start()
{
    const Disk *s = m_udisks->diskByPath(m_source);
    const Disk *t = m_udisks->diskByPath(m_targets->currentData().toString());
    if (!s || !t)
        return updateState();
    const diskclone::Plan plan = diskclone::plan(*s, *t);
    if (!plan.error.isEmpty())
        return updateState();
    m_target = t->blockPath;
    m_sourceVolumes = s->volumes;
    m_newIds = m_keepBoth->isChecked();
    m_growLast = m_grow->isVisible() && m_grow->isChecked();
    const bool verify = m_verify->isChecked();
    const QString sourceName = shortDevice(s->device), targetName = shortDevice(t->device);

    setRunning(true);
    m_phase->setText(tr("Waiting for permission…"));
    auto failed = [this](const QString &text) {
        setRunning(false);
        m_phase->setText(text);
        updateState();
    };
    openBlockThen(m_udisks, this, m_source, UDisks::OpenMode::Read, [=, this](int in) {
        if (in < 0)
            return failed(tr("Couldn't open %1.").arg(sourceName));
        openBlockThen(m_udisks, this, m_target, UDisks::OpenMode::ReadWrite, [=, this](int out) {
            if (out < 0) {
                blockio::close(in);
                return failed(tr("Couldn't open %1.").arg(targetName));
            }
            m_job = new CloneJob(in, out, plan, m_newIds, verify);
            connect(m_job, &CloneJob::progress, this, [this](const QString &phase, quint64 done, quint64 total) {
                m_meter.update(phase, done, total);
            });
            connect(m_job, &CloneJob::finished, this, [this](bool ok, const QString &message) {
                stopThread();
                if (!ok)
                    return finish(false, message);
                m_copied = message;
                afterCopy();
            });
            m_thread = startOnThread(this, m_job);
        });
    });
}

// The copy is done; now the parts that go through UDisks, one at a time.
void CloneDialog::afterCopy()
{
    m_progress->setRange(0, 0);
    m_phase->setText(tr("Finishing the copy…"));
    m_opConn = connect(m_udisks, &UDisks::operationFinished, this, [this](bool ok, const QString &message) {
        if (!ok)
            m_notes << message;
        QTimer::singleShot(0, this, &CloneDialog::nextStep);
    });
    m_steps = {
        [this] {
            m_udisks->rescan(m_target);
            return true;
        },
        [this] {
            waitForPartitions();
            return true;
        },
    };
    nextStep();
}

void CloneDialog::nextStep()
{
    while (!m_steps.isEmpty()) {
        if (m_steps.takeFirst()())
            return;
    }
    disconnect(m_opConn);
    finish(true, m_copied);
}

void CloneDialog::waitForPartitions()
{
    // Ready once every partition is back, with the IDs the originals had.
    auto ready = [this]() -> const Disk * {
        const Disk *t = m_udisks->diskByPath(m_target);
        if (!t || t->volumes.size() != m_sourceVolumes.size())
            return nullptr;
        for (const Volume &s : m_sourceVolumes) {
            bool found = false;
            for (const Volume &v : t->volumes)
                found = found || (v.offset == s.offset && (s.uuid.isEmpty() || !v.uuid.isEmpty()));
            if (!found)
                return nullptr;
        }
        return t;
    };
    // udev re-reads the table again a moment after the copy closes the drive, so the
    // partitions can vanish and come back. Go on once nothing has changed for 2 s.
    auto *settled = new QTimer(this);
    settled->setSingleShot(true);
    settled->setInterval(2000);
    auto *timeout = new QTimer(this);
    timeout->setSingleShot(true);
    auto conn = std::make_shared<QMetaObject::Connection>();
    auto changed = [ready, settled] {
        if (ready())
            settled->start();
        else
            settled->stop();
    };
    auto go = [this, ready, settled, timeout, conn] {
        disconnect(*conn);
        settled->stop();
        timeout->stop();
        settled->deleteLater();
        timeout->deleteLater();
        const Disk *t = ready();
        if (!t) {
            m_notes << tr("The copy's partitions didn't show up. Unplug the drive and plug it back in.");
            return nextStep();
        }
        if (m_newIds) {
            for (const Volume &v : t->volumes) {
                if (v.encrypted) {
                    m_notes << tr("%1 is encrypted and keeps its ID.").arg(shortDevice(v.device));
                    continue;
                }
                const QString uuid = diskclone::newUuid(v.fsType);
                if (!v.hasFilesystem || uuid.isEmpty())
                    continue;
                const QString path = v.objectPath;
                m_steps << [this, path, uuid] {
                    const Found f = lookUp(m_udisks, path);
                    if (!f.volume)
                        return false;
                    m_udisks->setUuid(*f.volume, uuid);
                    return true;
                };
            }
        }
        if (m_growLast) {
            const Volume *last = nullptr;
            for (const Volume &v : t->volumes) {
                if (!v.isContainer && v.number > 0 && (!last || v.offset > last->offset))
                    last = &v;
            }
            if (last && last->isContained) {
                m_notes << tr("The last partition is inside an extended partition, so it wasn't grown.");
            } else if (last) {
                const QString path = last->objectPath;
                m_steps << [this, path] {
                    const Found f = lookUp(m_udisks, path);
                    const ResizeLimits limits = f.volume ? m_udisks->resizeLimits(*f.volume) : ResizeLimits();
                    if (!limits.possible || limits.maxSize <= f.volume->size) {
                        if (f.volume)
                            m_notes << tr("%1 couldn't be grown: %2").arg(shortDevice(f.volume->device), limits.reason);
                        return false;
                    }
                    m_phase->setText(tr("Growing %1…").arg(shortDevice(f.volume->device)));
                    m_udisks->resize(*f.volume, limits.maxSize);
                    return true;
                };
            }
        }
        nextStep();
    };
    *conn = connect(m_udisks, &UDisks::changed, this, changed);
    connect(settled, &QTimer::timeout, this, go);
    connect(timeout, &QTimer::timeout, this, go);
    timeout->start(20000);
    changed();
}

void CloneDialog::finish(bool ok, const QString &message)
{
    qCInfo(lcOps).noquote() << "Clone" << (ok ? "finished:" : "failed:") << message;
    setRunning(false);
    m_udisks->refresh();
    QString text = message;
#ifdef Q_OS_WIN
    if (ok && !m_newIds)
        text += QStringLiteral("\n\n") + tr("Unplug one of the two drives: with the same IDs, Windows switches one of them off.");
#else
    if (ok && !m_newIds)
        text += QStringLiteral("\n\n") + tr("Unplug one of the two drives before the next restart, or Linux may mix them up.");
#endif
    if (!m_notes.isEmpty())
        text += QStringLiteral("\n\n") + m_notes.join(QLatin1Char('\n'));
    if (ok) {
        QMessageBox::information(this, windowTitle(), text);
        QDialog::accept();
    } else {
        QMessageBox::warning(this, windowTitle(), text);
        QDialog::reject();
    }
}

// --- Back up ------------------------------------------------------------------

BackupDialog::BackupDialog(UDisks *udisks, const QString &objectPath, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_object(objectPath)
    , m_file(new QLineEdit)
    , m_info(new QLabel)
    , m_problem(new QLabel)
    , m_progress(new QProgressBar)
    , m_phase(new QLabel)
    , m_meter(m_progress, m_phase)
{
    const Found f = lookUp(udisks, objectPath);
    setWindowTitle(f.volume ? tr("Back Up Partition") : tr("Back Up Drive"));
    QString name, title;
    if (f.disk) {
        title = whatTitle(f);
        name = f.volume ? (!f.volume->label.isEmpty() ? f.volume->label : shortDevice(f.volume->device)) : f.disk->model;
    }
    const QString folder = QDir::homePath() + QStringLiteral("/Backups/");
    m_file->setText(folder + safeName(name) + QDate::currentDate().toString(QStringLiteral("-yyyy-MM-dd")) + QStringLiteral(".img.zst"));

    auto *browse = new QPushButton(tr("Browse…"));
    browse->setAutoDefault(false);
    connect(browse, &QPushButton::clicked, this, [this] {
        QDir().mkpath(QFileInfo(m_file->text()).absolutePath());
        QString file = QFileDialog::getSaveFileName(this, tr("Save Backup As"), m_file->text(), tr("Compressed disk images (*.img.zst)"));
        if (file.isEmpty())
            return;
        if (!file.endsWith(QLatin1String(".img.zst")))
            file += QStringLiteral(".img.zst");
        m_file->setText(file);
    });
    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(m_file, 1);
    fileRow->addWidget(browse);

    m_info->setWordWrap(true);
    m_problem->setWordWrap(true);
    m_problem->setTextFormat(Qt::RichText);
    m_progress->setVisible(false);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("<p>Saves everything on <b>%1</b> into one compressed file, which Restore Backup can "
                                       "put back later, here or on another drive.</p>")
                                        .arg(title.toHtmlEscaped())));
    auto *form = new QFormLayout;
    form->addRow(tr("Save to:"), fileRow);
    layout->addLayout(form);
    layout->addWidget(m_info);
    layout->addWidget(m_problem);
    layout->addWidget(m_phase);
    layout->addWidget(m_progress);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_start = box->addButton(tr("Back Up"), QDialogButtonBox::AcceptRole);
    connect(m_start, &QPushButton::clicked, this, &BackupDialog::start);
    connect(box, &QDialogButtonBox::rejected, this, &BackupDialog::reject);
    layout->addWidget(box);

    connect(m_file, &QLineEdit::textChanged, this, &BackupDialog::updateState);
    updateState();
    resize(600, sizeHint().height());
}

BackupDialog::~BackupDialog()
{
    if (m_job)
        m_job->cancel();
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
}

void BackupDialog::updateState()
{
    const Found f = lookUp(m_udisks, m_object);
    QString problem;
    QString info;
    if (!f.disk) {
        problem = tr("It isn't there anymore.");
    } else {
        const quint64 size = f.volume ? f.volume->size : f.disk->size;
        bool mounted = false;
        for (const Volume &v : f.disk->volumes) {
            if (!f.volume || v.objectPath == f.volume->objectPath)
                mounted = mounted || !v.mounts().isEmpty();
        }
        info = tr("The file holds all %1, but empty space packs down to almost nothing, so it's usually much smaller. "
                  "Takes %2.")
                   .arg(formatSize(size), durationText(copySeconds(size, *f.disk)));
        if (mounted)
            info += QLatin1Char(' ') + tr("It's unmounted while it's saved, so nothing changes halfway.");

        const QFileInfo file(m_file->text().trimmed());
        QString folder = file.absolutePath();
        while (!QFileInfo::exists(folder) && folder.size() > 1)
            folder = QFileInfo(folder).absolutePath();
        const QStorageInfo storage(folder);
        if (f.disk->isSystem && (!f.volume || f.volume->isSystem || mounted)) {
            problem = tr("This is part of the drive that runs the system, so it can't be unmounted to back it up. For the system "
                         "itself, use snapshots (see Btrfs Snapshots), or back it up from a live USB.");
        } else if (m_file->text().trimmed().isEmpty() || !m_file->text().trimmed().endsWith(QLatin1String(".img.zst"))) {
            problem = tr("Pick a file name ending in .img.zst.");
        } else if (blockio::pathIsOnDisk(folder, f.volume ? f.volume->device : f.disk->device)) {
            problem = tr("That folder is on what's being backed up. Save it on another drive.");
        } else {
            info += QStringLiteral("\n") + tr("Free space there: %1.").arg(formatSize(quint64(storage.bytesAvailable())));
        }
    }
    m_info->setText(info);
    m_problem->setText(problem.isEmpty() ? QString() : redText(problem));
    m_problem->setVisible(!problem.isEmpty());
    m_start->setEnabled(problem.isEmpty() && !m_running);
    fitHeight(this);
}

void BackupDialog::setRunning(bool running)
{
    m_running = running;
    m_file->setEnabled(!running);
    m_start->setEnabled(!running);
}

void BackupDialog::reject()
{
    if (m_job) {
        if (QMessageBox::question(this, windowTitle(), tr("Stop the backup? The unfinished file is deleted.")) == QMessageBox::Yes)
            m_job->cancel();
        return;
    }
    if (!m_running)
        QDialog::reject();
}

void BackupDialog::start()
{
    const Found f = lookUp(m_udisks, m_object);
    if (!f.disk)
        return updateState();
    const QString file = m_file->text().trimmed();
    if (QFileInfo::exists(file)
        && QMessageBox::question(this, windowTitle(), tr("%1 already exists. Replace it?").arg(QFileInfo(file).fileName())) != QMessageBox::Yes)
        return;
    if (!QDir().mkpath(QFileInfo(file).absolutePath())) {
        m_problem->setText(redText(tr("Couldn't create the folder %1.").arg(QFileInfo(file).absolutePath())));
        m_problem->setVisible(true);
        return;
    }
    const BackupInfo info = imagebackup::infoFor(*f.disk, f.volume);
    setRunning(true);
    m_phase->setText(tr("Waiting for permission…"));
    openBlockThen(m_udisks, this, m_object, UDisks::OpenMode::Read, [this, file, info](int fd) {
        if (fd < 0) {
            setRunning(false);
            m_phase->setText(tr("Couldn't open it."));
            return updateState();
        }
        m_job = new BackupJob(fd, file, info);
        connect(m_job, &BackupJob::progress, this, [this](const QString &phase, quint64 done, quint64 total) {
            m_meter.update(phase, done, total);
        });
        connect(m_job, &BackupJob::finished, this, [this](bool ok, const QString &message) {
            qCInfo(lcOps).noquote() << "Back Up" << (ok ? "finished:" : "failed:") << message;
            m_thread->quit();
            m_thread->wait();
            m_thread = nullptr;
            m_job = nullptr;
            setRunning(false);
            m_udisks->refresh();
            if (ok) {
                QMessageBox::information(this, windowTitle(), message);
                QDialog::accept();
            } else {
                QMessageBox::warning(this, windowTitle(), message);
                m_progress->setVisible(false);
                m_phase->clear();
                updateState();
            }
        });
        m_thread = startOnThread(this, m_job);
    });
}

// --- Restore ------------------------------------------------------------------

RestoreDialog::RestoreDialog(UDisks *udisks, const QString &objectPath, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_object(objectPath)
    , m_file(new QLineEdit)
    , m_info(new QLabel)
    , m_checkFirst(new QCheckBox(tr("Check the whole backup before writing anything (recommended)")))
    , m_verify(new QCheckBox(tr("Check the drive after restoring (takes about as long again)")))
    , m_warning(new QLabel)
    , m_confirm(new QLineEdit)
    , m_progress(new QProgressBar)
    , m_phase(new QLabel)
    , m_meter(m_progress, m_phase)
{
    setWindowTitle(tr("Restore Backup"));
    const Found f = lookUp(udisks, objectPath);
    auto *browse = new QPushButton(tr("Browse…"));
    browse->setAutoDefault(false);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString start = m_file->text().isEmpty() ? QDir::homePath() + QStringLiteral("/Backups") : m_file->text();
        const QString file = QFileDialog::getOpenFileName(this, tr("Choose a Backup"), start,
                                                          tr("Disk backups (*.img.zst *.img);;All files (*)"));
        if (!file.isEmpty())
            m_file->setText(file);
    });
    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(m_file, 1);
    fileRow->addWidget(browse);
    m_file->setPlaceholderText(tr("A .img.zst file made by Back Up, or a raw .img"));
    m_info->setWordWrap(true);
    m_info->setTextFormat(Qt::RichText);
    m_checkFirst->setChecked(true);
    m_warning->setWordWrap(true);
    m_warning->setTextFormat(Qt::RichText);
    m_progress->setVisible(false);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("<p>Puts a backup back onto <b>%1</b>.</p>").arg(whatTitle(f).toHtmlEscaped())));
    auto *form = new QFormLayout;
    form->addRow(tr("Backup:"), fileRow);
    layout->addLayout(form);
    layout->addWidget(m_info);
    layout->addWidget(m_checkFirst);
    layout->addWidget(m_verify);
    layout->addWidget(m_warning);
    layout->addWidget(m_confirm);
    layout->addWidget(m_phase);
    layout->addWidget(m_progress);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_start = box->addButton(tr("Restore"), QDialogButtonBox::ActionRole);
    m_start->setAutoDefault(false);
    box->button(QDialogButtonBox::Cancel)->setDefault(true);
    connect(m_start, &QPushButton::clicked, this, &RestoreDialog::start);
    connect(box, &QDialogButtonBox::rejected, this, &RestoreDialog::reject);
    layout->addWidget(box);

    connect(m_file, &QLineEdit::textChanged, this, &RestoreDialog::updateState);
    connect(m_confirm, &QLineEdit::textChanged, this, &RestoreDialog::updateState);
    updateState();
    resize(600, sizeHint().height());
}

RestoreDialog::~RestoreDialog()
{
    if (m_job)
        m_job->cancel();
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
}

void RestoreDialog::updateState()
{
    if (m_running)
        return;
    bool asked = false; // the box is shown or hidden once, at the end (see CloneDialog)
    const Found f = lookUp(m_udisks, m_object);
    const QString file = m_file->text().trimmed();
    QString problem, info;
    bool ok = false;
    m_backup = BackupInfo();
    if (!f.disk) {
        problem = tr("The drive isn't there anymore.");
    } else if (f.disk->isSystem) {
        problem = tr("%1 runs this system, so DiskForge won't write over it.").arg(shortDevice(f.disk->device));
    } else if (!file.isEmpty() && !QFileInfo(file).isFile()) {
        problem = tr("File not found.");
    } else if (!file.isEmpty()) {
        m_backup = imagebackup::describe(file);
        const quint64 room = f.volume ? f.volume->size : f.disk->size;
        if (!m_backup.error.isEmpty()) {
            problem = m_backup.error;
        } else {
            if (!m_backup.kind.isEmpty()) {
                const QString what = m_backup.kind == QLatin1String("disk") ? tr("the whole drive %1").arg(m_backup.model)
                                                                            : tr("%1 on %2").arg(m_backup.label.isEmpty() ? shortDevice(m_backup.device) : m_backup.label, m_backup.model);
                info = tr("Backup of %1, %2, made %3 with %4.")
                           .arg(what.toHtmlEscaped(), formatSize(m_backup.size),
                                QLocale().toString(m_backup.created.toLocalTime(), QLocale::ShortFormat), m_backup.app.toHtmlEscaped());
            } else {
                info = tr("%1 of data. There's no description file next to it, so only the file's own checksum can be checked.")
                           .arg(formatSize(m_backup.size));
            }
            if (m_backup.kind == QLatin1String("disk") && f.volume)
                problem = tr("This is a backup of a whole drive. Restore it onto a whole drive, not a partition.");
            else if (m_backup.size > room)
                problem = tr("The backup (%1) doesn't fit: there's only %2.").arg(formatSize(m_backup.size), formatSize(room));
            else if (m_backup.kind == QLatin1String("disk") && m_backup.sectorSize != f.disk->sectorSize)
                problem = tr("The backup was made on a drive with %1-byte sectors and this one uses %2, so it wouldn't work here.")
                              .arg(m_backup.sectorSize)
                              .arg(f.disk->sectorSize);
            else if (blockio::pathIsOnDisk(file, f.volume ? f.volume->device : f.disk->device))
                problem = tr("The backup file is on the drive it would be restored onto.");
            else if (f.volume && m_backup.size < room)
                info += QLatin1Char(' ') + tr("The partition is bigger than the backup; afterwards, use Resize to let the file system use the rest.");
            if (problem.isEmpty()) {
                const QString name = shortDevice(f.volume ? f.volume->device : f.disk->device);
                ok = confirmMatches(m_warning, m_confirm, tr("Everything on %1 will be replaced by the backup.").arg(name), *f.disk, name);
                asked = true;
            }
        }
    }
    m_warning->setVisible(asked);
    m_confirm->setVisible(asked);
    m_info->setText(problem.isEmpty() ? info : (info.isEmpty() ? QString() : info + QStringLiteral("<br>")) + redText(problem));
    m_checkFirst->setEnabled(!m_backup.sha256.isEmpty() || m_backup.compressed);
    m_start->setEnabled(ok);
    fitHeight(this);
}

void RestoreDialog::setRunning(bool running)
{
    m_running = running;
    for (QWidget *w : std::initializer_list<QWidget *>{m_file, m_checkFirst, m_verify, m_confirm, m_start})
        w->setEnabled(!running);
}

void RestoreDialog::reject()
{
    if (m_job) {
        if (QMessageBox::warning(this, windowTitle(), tr("Stop restoring? If writing has started, what's there now will be incomplete."),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            == QMessageBox::Yes)
            m_job->cancel();
        return;
    }
    if (!m_running)
        QDialog::reject();
}

void RestoreDialog::start()
{
    const Found f = lookUp(m_udisks, m_object);
    if (!f.disk || m_backup.size == 0)
        return updateState();
    const QString file = m_file->text().trimmed();
    const BackupInfo info = m_backup;
    const bool checkFirst = m_checkFirst->isChecked() && m_checkFirst->isEnabled();
    const bool verify = m_verify->isChecked();
    const QString diskPath = f.disk->blockPath;
    const bool wholeDisk = f.volume == nullptr;
    setRunning(true);
    m_phase->setText(tr("Waiting for permission…"));
    openBlockThen(m_udisks, this, m_object, UDisks::OpenMode::ReadWrite, [=, this](int fd) {
        if (fd < 0) {
            setRunning(false);
            m_phase->setText(tr("Couldn't open it."));
            return updateState();
        }
        m_job = new RestoreJob(file, info, fd, checkFirst, verify);
        connect(m_job, &RestoreJob::progress, this, [this](const QString &phase, quint64 done, quint64 total) {
            m_meter.update(phase, done, total);
        });
        connect(m_job, &RestoreJob::finished, this, [=, this](bool ok, const QString &message, bool wrote) {
            qCInfo(lcOps).noquote() << "Restore" << (ok ? "finished:" : "failed:") << message;
            m_thread->quit();
            m_thread->wait();
            m_thread = nullptr;
            m_job = nullptr;
            setRunning(false);
            if (wrote && wholeDisk)
                m_udisks->rescan(diskPath); // its partitions changed under the kernel
            m_udisks->refresh();
            if (ok) {
                QMessageBox::information(this, windowTitle(), message);
                QDialog::accept();
            } else {
                QMessageBox::warning(this, windowTitle(), message);
                if (wrote) {
                    QDialog::reject();
                } else {
                    m_progress->setVisible(false);
                    m_phase->clear();
                    updateState();
                }
            }
        });
        m_thread = startOnThread(this, m_job);
    });
}

// --- Rescue -------------------------------------------------------------------

RescueDialog::RescueDialog(UDisks *udisks, const QString &sourceBlockPath, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_source(sourceBlockPath)
    , m_toFile(new QRadioButton(tr("An image file:")))
    , m_toDrive(new QRadioButton(tr("Another drive:")))
    , m_file(new QLineEdit)
    , m_browse(new QPushButton(tr("Browse…")))
    , m_targets(new QComboBox)
    , m_info(new QLabel)
    , m_resume(new QLabel)
    , m_map(new BlockMapWidget)
    , m_stats(new QLabel)
    , m_progress(new QProgressBar)
    , m_warning(new QLabel)
    , m_confirm(new QLineEdit)
{
    setWindowTitle(tr("Rescue Copy"));
    const Disk *source = udisks->diskByPath(sourceBlockPath);
    m_file->setText(QDir::homePath() + QStringLiteral("/Rescue/") + safeName(source ? source->model : QString()) + QStringLiteral(".img"));
    m_browse->setAutoDefault(false);
    connect(m_browse, &QPushButton::clicked, this, [this] {
        QDir().mkpath(QFileInfo(m_file->text()).absolutePath());
        const QString file = QFileDialog::getSaveFileName(this, tr("Save the Copy As"), m_file->text(), tr("Disk images (*.img)"),
                                                          nullptr, QFileDialog::DontConfirmOverwrite);
        if (!file.isEmpty())
            m_file->setText(file);
    });
    m_toFile->setChecked(true);
    for (QLabel *l : {m_info, m_resume, m_stats, m_warning}) {
        l->setWordWrap(true);
        l->setTextFormat(Qt::RichText);
    }
    m_progress->setRange(0, 1000);
    m_progress->setVisible(false);
    m_map->setShowPending(true);
    if (source) {
        m_map->reset(source->size, source->rotationRate > 0);
        QVector<BlockMapWidget::Area> areas;
        for (const Volume &v : source->volumes) {
            if (!v.isContainer)
                areas.append({v.offset, v.offset + v.size, volumeTitle(v)});
        }
        m_map->setAreas(areas);
    }

    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(m_toFile);
    fileRow->addWidget(m_file, 1);
    fileRow->addWidget(m_browse);
    auto *driveRow = new QHBoxLayout;
    driveRow->addWidget(m_toDrive);
    driveRow->addWidget(m_targets, 1);

    // Going easy on the drive: all off to start with, and they can change while it runs.
    auto spin = [](int low, int high, int value, const QString &suffix) {
        auto *s = new QSpinBox;
        s->setRange(low, high);
        s->setValue(value);
        s->setSuffix(suffix);
        return s;
    };
    m_heat = new QCheckBox(tr("Pause when it's hotter than"));
    m_hot = spin(40, 70, 55, QStringLiteral(" °C"));
    m_cool = spin(30, 69, 50, QStringLiteral(" °C"));
    m_rest = new QCheckBox(tr("Rest for"));
    m_restSeconds = spin(5, 600, 60, tr(" s"));
    m_restErrors = spin(1, 1000, 10, QString());
    m_limit = new QCheckBox(tr("Read at most"));
    m_rate = spin(1, 2000, 50, tr(" MB/s"));
    m_heatStatus = new QLabel;
    m_heatTimer = new QTimer(this);
    m_heatTimer->setInterval(60000);
    const bool hasTemperature = source && source->health.temperatureC > 0;
    if (!hasTemperature) {
        m_heat->setEnabled(false);
        m_heat->setToolTip(tr("This drive doesn't report its temperature (often the case through USB adapters)."));
    }
    auto row = [](std::initializer_list<QWidget *> parts) {
        auto *r = new QHBoxLayout;
        for (QWidget *w : parts)
            r->addWidget(w);
        r->addStretch();
        return r;
    };
    auto *easy = new QGroupBox(tr("Go easy on the drive"));
    auto *easyLayout = new QVBoxLayout(easy);
    easyLayout->addLayout(row({m_heat, m_hot, new QLabel(tr("and carry on at")), m_cool, m_heatStatus}));
    easyLayout->addLayout(row({m_rest, m_restSeconds, new QLabel(tr("after")), m_restErrors, new QLabel(tr("read errors in a row"))}));
    easyLayout->addLayout(row({m_limit, m_rate}));
    for (QCheckBox *c : {m_heat, m_rest, m_limit})
        connect(c, &QCheckBox::toggled, this, &RescueDialog::applyCoolDown);
    for (QSpinBox *s : {m_hot, m_cool, m_restSeconds, m_restErrors, m_rate})
        connect(s, &QSpinBox::valueChanged, this, &RescueDialog::applyCoolDown);
    connect(m_hot, &QSpinBox::valueChanged, this, [this](int hot) { m_cool->setMaximum(hot - 1); }); // carrying on needs cooler
    connect(m_heatTimer, &QTimer::timeout, this, [this] {
        if (const Disk *d = m_udisks->diskByPath(m_source))
            m_udisks->refreshHealthQuietly(*d);
    });
    connect(m_udisks, &UDisks::changed, this, &RescueDialog::checkHeat);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(
        tr("<p>Copies everything that can still be read off <b>%1</b>: the easy parts first, then it goes back for the hard ones. "
           "Use it when a drive is failing. Get the data off first; don't run repairs on it before that.</p>")
            .arg(source ? diskTitle(*source).toHtmlEscaped() : QString())));
    layout->addWidget(new QLabel(tr("Copy to:")));
    layout->addLayout(fileRow);
    layout->addLayout(driveRow);
    layout->addWidget(m_info);
    layout->addWidget(m_resume);
    layout->addWidget(easy);
    layout->addWidget(m_map, 1);
    layout->addWidget(m_stats);
    layout->addWidget(m_progress);
    layout->addWidget(m_warning);
    layout->addWidget(m_confirm);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    m_start = box->addButton(tr("Start"), QDialogButtonBox::ActionRole);
    connect(m_start, &QPushButton::clicked, this, [this] {
        if (m_job)
            m_job->cancel();
        else
            start();
    });
    connect(box, &QDialogButtonBox::rejected, this, &RescueDialog::reject);
    layout->addWidget(box);

    fillTargets();
    for (QRadioButton *r : {m_toFile, m_toDrive})
        connect(r, &QRadioButton::toggled, this, &RescueDialog::updateState);
    connect(m_file, &QLineEdit::textChanged, this, &RescueDialog::updateState);
    connect(m_targets, &QComboBox::currentIndexChanged, this, &RescueDialog::updateState);
    connect(m_confirm, &QLineEdit::textChanged, this, &RescueDialog::updateState);
    updateState();
    resize(680, 640);
}

RescueDialog::~RescueDialog()
{
    if (m_job)
        m_job->cancel();
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
}

void RescueDialog::applyCoolDown()
{
    if (m_job) {
        m_job->setSpeedLimit(m_limit->isChecked() ? quint64(m_rate->value()) * 1000 * 1000 : 0);
        m_job->setErrorRest(m_rest->isChecked() ? m_restErrors->value() : 0, m_restSeconds->value());
    }
    if (m_job && m_heat->isChecked())
        m_heatTimer->start();
    else
        m_heatTimer->stop();
    checkHeat();
}

// Pauses the copy when the drive gets too hot, and carries on once it's cooled down.
void RescueDialog::checkHeat()
{
    const Disk *d = m_udisks->diskByPath(m_source);
    const double temperature = d ? d->health.temperatureC : -1;
    const bool watch = m_job && m_heat->isChecked() && temperature > 0;
    if (!watch) {
        if (m_job && m_paused)
            m_job->setPaused(false);
        m_paused = false;
        m_heatStatus->clear();
        return;
    }
    if (!m_paused && temperature >= m_hot->value())
        m_paused = true;
    else if (m_paused && temperature <= m_cool->value())
        m_paused = false;
    m_job->setPaused(m_paused);
    m_heatStatus->setText(m_paused ? tr("Resting: %1 °C, waiting for %2 °C").arg(qRound(temperature)).arg(m_cool->value())
                                   : tr("now %1 °C").arg(qRound(temperature)));
}

void RescueDialog::fillTargets()
{
    const Disk *source = m_udisks->diskByPath(m_source);
    const QVector<Disk> &disks = m_udisks->disks();
    for (int i = 0; i < disks.size(); ++i) {
        const Disk &d = disks[i];
        if (d.blockPath == m_source || d.isSystem || d.readOnly || (source && d.size < source->size))
            continue;
        m_targets->addItem(tr("Disk %1: %2").arg(i).arg(diskTitle(d)), d.blockPath);
    }
    if (m_targets->count() == 0) {
        m_targets->addItem(tr("No other drive big enough"));
        m_toDrive->setEnabled(false);
    }
}

QString RescueDialog::mapPath() const
{
    if (m_toFile->isChecked())
        return m_file->text().trimmed() + QStringLiteral(".map");
    // Named after both drives' models and serials, so a resume finds it even if the
    // device names changed.
    const Disk *source = m_udisks->diskByPath(m_source);
    const Disk *target = m_udisks->diskByPath(m_targets->currentData().toString());
    if (!source || !target)
        return {};
    return QDir::homePath() + QStringLiteral("/Rescue/") + safeName(source->model + QLatin1Char('-') + source->serial) + QStringLiteral("-to-")
         + safeName(target->model + QLatin1Char('-') + target->serial) + QStringLiteral(".map");
}

void RescueDialog::showBlocks(const QVector<RescueMap::Block> &blocks)
{
    BlockMapData &data = m_map->data();
    data.clearStates();
    for (const RescueMap::Block &b : blocks) {
        switch (b.status) {
        case RescueMap::Finished:
            data.mark(b.pos, b.size, BlockMapData::State::Good);
            break;
        case RescueMap::Bad:
            data.mark(b.pos, b.size, BlockMapData::State::Bad);
            break;
        case RescueMap::NonTrimmed:
        case RescueMap::NonScraped:
            data.mark(b.pos, b.size, BlockMapData::State::Pending);
            break;
        default:
            break;
        }
    }
    m_map->refresh();
}

void RescueDialog::showSavedMap()
{
    const Disk *source = m_udisks->diskByPath(m_source);
    RescueMap saved;
    QString error;
    const QString path = mapPath();
    if (!source || path.isEmpty() || !QFileInfo::exists(path)) {
        m_resume->setText(tr("Progress is saved as it goes, so a stopped rescue can carry on later."));
        if (source)
            showBlocks({});
        return;
    }
    if (!saved.load(path, source->size, &error)) {
        m_resume->setText(redText(tr("There's a map file from another rescue at %1 (%2). Pick another name, or remove it.")
                                      .arg(path, error)
                                      .toHtmlEscaped()));
        return;
    }
    showBlocks(saved.blocks());
    m_resume->setText(saved.done() ? tr("This rescue already finished. Starting again retries nothing; the copy is complete as far as it goes.")
                                   : tr("A rescue was started before and got %1. It carries on where it stopped.")
                                         .arg(formatSize(saved.total(RescueMap::Finished))));
}

void RescueDialog::updateState()
{
    if (m_running)
        return;
    m_file->setEnabled(m_toFile->isChecked());
    m_browse->setEnabled(m_toFile->isChecked());
    m_targets->setEnabled(m_toDrive->isChecked());
    bool asked = false; // the box is shown or hidden once, at the end (see CloneDialog)
    const Disk *source = m_udisks->diskByPath(m_source);
    QString problem, info;
    bool ok = false;
    if (!source) {
        problem = tr("The drive isn't there anymore.");
    } else if (source->isSystem) {
        problem = tr("This drive runs the system. Rescue it from a live USB instead.");
    } else if (m_toFile->isChecked()) {
        const QString file = m_file->text().trimmed();
        QString folder = QFileInfo(file).absolutePath();
        while (!QFileInfo::exists(folder) && folder.size() > 1)
            folder = QFileInfo(folder).absolutePath();
        const quint64 existing = QFileInfo(file).isFile() ? quint64(QFileInfo(file).size()) : 0;
        const quint64 free = quint64(QStorageInfo(folder).bytesAvailable());
        if (file.isEmpty()) {
            problem = tr("Pick where to save the copy.");
        } else if (QFileInfo::exists(file) && !QFileInfo::exists(mapPath())) {
            // Without its map it's not an earlier rescue, just some file: don't write over it.
            problem = tr("%1 already exists. Pick a new name.").arg(QFileInfo(file).fileName());
        } else if (blockio::pathIsOnDisk(folder, source->device)) {
            problem = tr("That folder is on the failing drive itself. Save the copy on another drive.");
        } else {
            ok = true;
            info = tr("Free space there: %1. The copy needs up to %2.").arg(formatSize(free), formatSize(source->size));
            if (free + existing < source->size)
                info += QLatin1Char(' ') + redText(tr("It stops when the space runs out; what's copied by then is kept."));
        }
    } else {
        const Disk *target = m_udisks->diskByPath(m_targets->currentData().toString());
        if (target) {
            ok = confirmMatches(m_warning, m_confirm, tr("Everything on %1 will be erased.").arg(diskTitle(*target)), *target,
                                shortDevice(target->device));
            asked = true;
        }
    }
    m_warning->setVisible(asked);
    m_confirm->setVisible(asked);
    if (source && !source->isSystem) {
        for (const Volume &v : source->volumes) {
            if (!v.mounts().isEmpty()) {
                info += (info.isEmpty() ? QString() : QStringLiteral("<br>"))
                      + tr("Some of its partitions are mounted. Unmount them first if you can, so nothing changes while it's copied.");
                break;
            }
        }
    }
    m_info->setText(problem.isEmpty() ? info : redText(problem));
    showSavedMap();
    m_start->setEnabled(ok && problem.isEmpty());
    fitHeight(this);
}

void RescueDialog::setRunning(bool running)
{
    m_running = running;
    for (QWidget *w : std::initializer_list<QWidget *>{m_toFile, m_toDrive, m_file, m_browse, m_targets, m_confirm})
        w->setEnabled(!running);
    m_start->setText(running ? tr("Stop") : tr("Start"));
}

void RescueDialog::reject()
{
    if (m_job) {
        if (QMessageBox::question(this, windowTitle(), tr("Stop the rescue? Progress is saved; start it again later to carry on."))
            == QMessageBox::Yes)
            m_job->cancel();
        return;
    }
    if (!m_running)
        QDialog::reject();
}

void RescueDialog::start()
{
    const Disk *s = m_udisks->diskByPath(m_source);
    if (!s)
        return updateState();
    const quint64 size = s->size;
    const QString map = mapPath();
    const bool toFile = m_toFile->isChecked();
    const QString file = m_file->text().trimmed();
    const QString target = m_targets->currentData().toString();
    QDir().mkpath(QFileInfo(map).absolutePath());

    setRunning(true);
    m_stats->setText(tr("Waiting for permission…"));
    auto failed = [this](const QString &text) {
        setRunning(false);
        m_stats->setText(text);
        updateState();
    };
    auto run = [=, this](int in, int out) {
        m_job = new RescueCopy(in, size, out, map);
        auto *clock = new QElapsedTimer;
        clock->start();
        auto *startedAt = new quint64(0);
        connect(m_job, &RescueCopy::mapChanged, this, &RescueDialog::showBlocks);
        connect(m_job, &RescueCopy::progress, this, [=, this](quint64 rescued, quint64 bad, quint64 total, const QString &phase) {
            if (*startedAt == 0)
                *startedAt = rescued + 1; // what an earlier run had already done doesn't count for the speed
            const double seconds = clock->nsecsElapsed() / 1e9;
            const double rate = seconds > 2 ? double(rescued + 1 - *startedAt) / seconds : 0;
            m_progress->setVisible(true);
            m_progress->setValue(int((rescued + bad) * 1000 / std::max<quint64>(total, 1)));
            m_stats->setText(tr("%1. Rescued %2 of %3, unreadable so far: %4.")
                                 .arg(phase, formatSize(rescued), formatSize(total), formatSize(bad))
                             + (rate > 0 ? QLatin1Char(' ') + tr("%1/s.").arg(formatSize(quint64(rate))) : QString()));
        });
        connect(m_job, &RescueCopy::finished, this, [=, this](bool completed, const QString &message) {
            qCInfo(lcOps).noquote() << "Rescue copy" << (completed ? "finished:" : "stopped:") << message;
            delete clock;
            delete startedAt;
            m_thread->quit();
            m_thread->wait();
            m_thread = nullptr;
            m_job = nullptr;
            applyCoolDown(); // stops watching the heat
            setRunning(false);
            m_stats->setText(message);
            m_progress->setVisible(false);
            m_udisks->refresh();
            if (completed)
                QMessageBox::information(this, windowTitle(), message);
            updateState();
        });
        m_thread = startOnThread(this, m_job);
        applyCoolDown();
    };
    openBlockThen(m_udisks, this, m_source, UDisks::OpenMode::Benchmark, [=, this](int in) {
        if (in < 0)
            return failed(tr("Couldn't open the drive."));
        if (toFile) {
            const int out = blockio::openFile(file, blockio::FileMode::Update);
            if (out < 0) {
                blockio::close(in);
                return failed(tr("Couldn't open %1: %2").arg(file, QString::fromLocal8Bit(std::strerror(errno))));
            }
            return run(in, out);
        }
        openBlockThen(m_udisks, this, target, UDisks::OpenMode::ReadWrite, [=](int out) {
            if (out < 0) {
                blockio::close(in);
                return failed(tr("Couldn't open the drive to copy to."));
            }
            run(in, out);
        });
    });
}
