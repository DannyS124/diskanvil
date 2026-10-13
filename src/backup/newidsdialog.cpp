// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "newidsdialog.h"

#include "applog.h"
#include "clone.h"
#include "dialogs.h"
#include "format.h"
#include "jobui.h"
#include "newids.h"
#include "udisks.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#ifndef Q_OS_WIN
#include <unistd.h>
#else
#include <io.h>
#endif

namespace {

// Where a Linux keeps what names its drives: its own file system, and a /boot or EFI one.
bool mayHoldLinuxSettings(const QString &fsType)
{
    static const QStringList kinds = {QStringLiteral("ext2"), QStringLiteral("ext3"), QStringLiteral("ext4"), QStringLiteral("xfs"),
                                      QStringLiteral("btrfs"), QStringLiteral("f2fs"), QStringLiteral("vfat")};
    return kinds.contains(fsType);
}

const Volume *byNumber(const Disk *d, int number)
{
    for (const Volume &v : d->volumes) {
        if (v.number == number)
            return &v;
    }
    return nullptr;
}

} // namespace

NewIdsDialog::NewIdsDialog(UDisks *udisks, const QString &diskPath, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_disk(diskPath)
    , m_list(new QTreeWidget)
    , m_linux(new QCheckBox(tr("Change them in a Linux on it too (/etc/fstab and its boot entries), so it still starts")))
    , m_confirm(new QLineEdit)
    , m_phase(new QLabel)
    , m_progress(new QProgressBar)
    , m_start(new QPushButton(tr("Give New IDs")))
{
    setWindowTitle(tr("Give New IDs"));
    setObjectName(QStringLiteral("newIds"));
    resize(720, 520);
    const Disk *d = m_udisks->diskByPath(diskPath);
    m_device = d ? d->device : QString();
    const QString name = d ? QStringLiteral("%1 (%2)").arg(d->model.isEmpty() ? tr("The drive") : d->model, shortDevice(d->device)) : diskPath;

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("%1 gets new IDs, so it can be used next to the drive it was copied from. What's on it stays "
                                       "as it is; only its IDs change.")
                                        .arg(name.toHtmlEscaped())));
    m_list->setObjectName(QStringLiteral("ids"));
    m_list->setHeaderLabels({tr("What"), tr("Its ID now"), tr("After")});
    m_list->setRootIsDecorated(false);
    m_list->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    if (d) {
        if (!d->tableId.isEmpty())
            new QTreeWidgetItem(m_list, {tr("The disk"), d->tableId, tr("a new one")});
        for (const Volume &v : d->volumes) {
            if (v.number > 0 && !v.partUuid.isEmpty())
                new QTreeWidgetItem(m_list, {tr("Partition %1").arg(v.number), v.partUuid, tr("a new one")});
        }
        for (const Volume &v : d->volumes) {
            const QString fs = v.effectiveFsType();
            if (v.encrypted && v.cleartextPath.isEmpty())
                new QTreeWidgetItem(m_list, {tr("%1 (encrypted, locked)").arg(shortDevice(v.device)), v.uuid, tr("stays: unlock it first")});
            else if (!v.uuid.isEmpty() && !diskclone::newUuid(fs).isEmpty())
                new QTreeWidgetItem(m_list, {tr("%1's file system (%2)").arg(shortDevice(v.device), fs), v.uuid, tr("a new one")});
        }
    }
    m_list->resizeColumnToContents(0);
    layout->addWidget(m_list, 1);
    m_linux->setObjectName(QStringLiteral("linux"));
    m_linux->setChecked(true);
    layout->addWidget(m_linux);
    layout->addWidget(wrappingLabel(tr("If it has Windows on it, that Windows may not start from it afterwards, as its boot settings name "
                                       "the old IDs; Windows' installation USB can repair that. For a spare copy, or a drive that "
                                       "holds only files, that doesn't matter.")));
    if (d && d->isSystem) {
        layout->addWidget(wrappingLabel(redText(tr("This is the drive the running system is on, so its IDs can't be changed: give the copy "
                                                   "new IDs instead."))));
    }
    auto *confirmRow = new QHBoxLayout;
    confirmRow->addWidget(new QLabel(tr("To go ahead, type %1:").arg(shortDevice(m_device))));
    m_confirm->setObjectName(QStringLiteral("confirm"));
    confirmRow->addWidget(m_confirm, 1);
    layout->addLayout(confirmRow);
    m_phase->setWordWrap(true);
    layout->addWidget(m_phase);
    m_progress->setRange(0, 0);
    m_progress->hide();
    layout->addWidget(m_progress);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    box->addButton(m_start, QDialogButtonBox::AcceptRole);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_start, &QPushButton::clicked, this, &NewIdsDialog::start);
    connect(m_confirm, &QLineEdit::textChanged, this, &NewIdsDialog::updateState);
    layout->addWidget(box);
    updateState();
}

void NewIdsDialog::updateState()
{
    const Disk *d = m_udisks->diskByPath(m_disk);
    m_start->setEnabled(!m_running && d && !d->isSystem && m_confirm->text().trimmed() == shortDevice(m_device));
}

void NewIdsDialog::start()
{
    const Disk *d = m_udisks->diskByPath(m_disk);
    if (!d || d->isSystem || m_running || m_confirm->text().trimmed() != shortDevice(m_device))
        return;
    m_running = true;
    updateState();
    m_confirm->setEnabled(false);
    m_progress->show();
    m_oldTableId = d->tableId;
    for (const Volume &v : d->volumes) {
        m_oldPartIds.insert(v.number, v.partUuid);
        m_oldUuids.insert(v.number, v.uuid);
    }
    const bool gpt = d->tableType == QLatin1String("gpt");
    const bool table = gpt || d->tableType == QLatin1String("dos");
    const int sectorSize = d->sectorSize;
    qCInfo(lcOps).noquote() << "Give New IDs:" << m_device;
    if (!table) {
        // A file system on the whole drive: no disk or partition IDs to change.
        waitForNewTable();
        return;
    }
#ifdef Q_OS_WIN
    // Through Windows' own drive layout call.
    Q_UNUSED(gpt)
    Q_UNUSED(sectorSize)
    m_phase->setText(tr("Giving the disk and its partitions new IDs…"));
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(m_udisks, &UDisks::operationFinished, this, [this, conn](bool ok, const QString &message) {
        disconnect(*conn);
        if (!ok)
            return finish(false, message);
        waitForNewTable();
    });
    m_udisks->giveNewDiskIds(*d);
    return;
#endif
    // Opening it to write unmounts what's on it first. When it can't be opened, UDisks says why.
    m_phase->setText(tr("Waiting for permission…"));
    auto why = std::make_shared<QString>();
    auto heard = std::make_shared<QMetaObject::Connection>();
    *heard = connect(m_udisks, &UDisks::operationFinished, this, [why](bool ok, const QString &message) {
        if (!ok)
            *why = message;
    });
    openBlockThen(m_udisks, this, m_disk, UDisks::OpenMode::ReadWrite, [this, gpt, sectorSize, why, heard](int fd) {
        disconnect(*heard);
        if (fd < 0)
            return finish(false, why->isEmpty() ? tr("Couldn't open %1.").arg(shortDevice(m_device)) : *why);
        m_phase->setText(tr("Giving the disk and its partitions new IDs…"));
        QString error;
        const bool ok = newids::giveDiskNewIds(fd, gpt, sectorSize, &error);
        ::close(fd);
        if (!ok)
            return finish(false, tr("The disk's IDs couldn't be changed: %1").arg(error));
        m_udisks->rescan(m_disk);
        waitForNewTable();
    });
}

// The kernel reads the partition table again; then the file systems, one at a time.
void NewIdsDialog::waitForNewTable()
{
    m_phase->setText(tr("Waiting for the partitions to come back…"));
    auto settled = new QTimer(this), timeout = new QTimer(this);
    settled->setSingleShot(true);
    settled->setInterval(1500);
    timeout->setSingleShot(true);
    auto conn = std::make_shared<QMetaObject::Connection>();
    auto ready = [this] {
        const Disk *d = m_udisks->diskByPath(m_disk);
        if (!d || (!m_oldTableId.isEmpty() && d->tableId == m_oldTableId))
            return false;
        for (auto it = m_oldPartIds.cbegin(); it != m_oldPartIds.cend(); ++it) {
            if (it.key() > 0 && !byNumber(d, it.key()))
                return false;
        }
        return true;
    };
    auto go = [this, settled, timeout, conn] {
        disconnect(*conn);
        settled->deleteLater();
        timeout->deleteLater();
        const Disk *d = m_udisks->diskByPath(m_disk);
        if (!d)
            return finish(false, tr("The drive went away."));
        if (!m_oldTableId.isEmpty() && d->tableId == m_oldTableId)
            m_notes << tr("The partitions didn't come back with new IDs yet; unplug the drive and plug it back in to see them.");
        m_opConn = connect(m_udisks, &UDisks::operationFinished, this, [this](bool ok, const QString &message) {
            if (!ok)
                m_notes << message;
            QTimer::singleShot(0, this, &NewIdsDialog::nextStep);
        });
        for (const Volume &v : d->volumes) {
            const QString fs = v.effectiveFsType();
            if (v.encrypted && v.cleartextPath.isEmpty()) {
                m_notes << tr("%1 is encrypted and locked, so its file system keeps its ID.").arg(shortDevice(v.device));
                continue;
            }
            const QString uuid = diskclone::newUuid(fs);
            if (v.uuid.isEmpty() || uuid.isEmpty())
                continue;
            m_newUuids.insert(v.number, uuid);
            const QString path = v.objectPath;
            m_steps << [this, path, uuid] {
                for (const Disk &disk : m_udisks->disks()) {
                    for (const Volume &w : disk.volumes) {
                        if (w.objectPath == path) {
                            m_phase->setText(tr("A new ID for %1's file system…").arg(shortDevice(w.device)));
                            m_udisks->setUuid(w, uuid);
                            return true;
                        }
                    }
                }
                return false;
            };
        }
        if (m_linux->isChecked())
            m_steps << [this] {
                fixLinuxSettings();
                return true;
            };
        nextStep();
    };
    *conn = connect(m_udisks, &UDisks::changed, this, [ready, settled] {
        if (ready())
            settled->start();
    });
    connect(settled, &QTimer::timeout, this, go);
    connect(timeout, &QTimer::timeout, this, go);
    timeout->start(20000);
    m_udisks->refresh();
    if (ready())
        settled->start();
}

void NewIdsDialog::nextStep()
{
    while (!m_steps.isEmpty()) {
        if (m_steps.takeFirst()())
            return;
    }
    disconnect(m_opConn);
    finish(true, QString());
}

// What a Linux on the drive names by the old IDs: each file system that may hold its settings
// is mounted in turn, fixed, and unmounted again.
void NewIdsDialog::fixLinuxSettings()
{
    disconnect(m_opConn);
    m_udisks->refresh();
    const Disk *d = m_udisks->diskByPath(m_disk);
    if (!d)
        return finish(false, tr("The drive went away."));
    for (const Volume &v : d->volumes) {
        const QString oldPart = m_oldPartIds.value(v.number), oldFs = m_oldUuids.value(v.number);
        if (!oldPart.isEmpty() && !v.partUuid.isEmpty() && oldPart.compare(v.partUuid, Qt::CaseInsensitive) != 0)
            m_renamed.insert(oldPart, v.partUuid);
        if (!oldFs.isEmpty() && !v.uuid.isEmpty() && m_newUuids.contains(v.number) && oldFs.compare(v.uuid, Qt::CaseInsensitive) != 0)
            m_renamed.insert(oldFs, v.uuid);
        if (mayHoldLinuxSettings(v.effectiveFsType()))
            m_fixTodo << v.objectPath;
    }
    fixNext();
}

void NewIdsDialog::fixNext()
{
    const Volume *v = nullptr;
    while (!v && !m_fixTodo.isEmpty()) {
        const QString path = m_fixTodo.takeFirst();
        for (const Disk &disk : m_udisks->disks()) {
            for (const Volume &w : disk.volumes) {
                if (w.objectPath == path)
                    v = &w;
            }
        }
    }
    if (!v) {
        if (!m_fixed.isEmpty())
            m_notes << tr("Changed to the new IDs: %1 (each kept as it was next to it, ending in .diskanvil-<date>).")
                           .arg(m_fixed.join(QStringLiteral(", ")));
        return finish(true, QString());
    }
    m_phase->setText(tr("Looking at %1 for a Linux's settings…").arg(shortDevice(v->device)));
    if (!v->mounts().isEmpty())
        return fixIn(v->objectPath, v->mounts().first(), false);
    // Mounted for a moment: once it shows up mounted, or gives up after a while.
    const QString path = v->objectPath;
    auto conn = std::make_shared<QMetaObject::Connection>(), failed = std::make_shared<QMetaObject::Connection>();
    auto timer = new QTimer(this);
    timer->setSingleShot(true);
    auto done = [this, path, conn, failed, timer](bool mounted) {
        disconnect(*conn);
        disconnect(*failed);
        timer->stop();
        timer->deleteLater();
        for (const Disk &disk : m_udisks->disks()) {
            for (const Volume &w : disk.volumes) {
                if (w.objectPath == path && mounted && !w.mounts().isEmpty())
                    return fixIn(path, w.mounts().first(), true);
            }
        }
        QTimer::singleShot(0, this, &NewIdsDialog::fixNext);
    };
    *conn = connect(m_udisks, &UDisks::changed, this, [this, path, done] {
        for (const Disk &disk : m_udisks->disks()) {
            for (const Volume &w : disk.volumes) {
                if (w.objectPath == path && !w.mounts().isEmpty())
                    return done(true);
            }
        }
    });
    *failed = connect(m_udisks, &UDisks::operationFinished, this, [done](bool ok, const QString &) {
        if (!ok)
            done(false);
    });
    connect(timer, &QTimer::timeout, this, [done] { done(false); });
    timer->start(20000);
    m_udisks->mount(*v, false, false);
}

void NewIdsDialog::fixIn(const QString &objectPath, const QString &mount, bool mountedHere)
{
    QString error;
    for (const QString &file : newids::rewrite(newids::filesNaming(mount, m_renamed.keys()), m_renamed, &error))
        m_fixed << QString(file).mid(mount.size() + 1);
    if (!error.isEmpty())
        m_notes << error;
    if (!mountedHere)
        return fixNext();
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(m_udisks, &UDisks::operationFinished, this, [this, conn](bool, const QString &) {
        disconnect(*conn);
        QTimer::singleShot(0, this, &NewIdsDialog::fixNext);
    });
    for (const Disk &disk : m_udisks->disks()) {
        for (const Volume &w : disk.volumes) {
            if (w.objectPath == objectPath)
                return m_udisks->unmount(w);
        }
    }
    disconnect(*conn);
    fixNext();
}

void NewIdsDialog::finish(bool ok, const QString &message)
{
    m_running = false;
    m_progress->hide();
    m_udisks->refresh();
    qCInfo(lcOps).noquote() << "Give New IDs" << (ok ? "finished" : "failed:") << message << m_notes.join(QStringLiteral(" / "));
    QString text = ok ? tr("%1 has new IDs now.").arg(shortDevice(m_device)) : message;
    if (!m_notes.isEmpty())
        text += QStringLiteral("\n\n") + m_notes.join(QLatin1Char('\n'));
    if (text.isEmpty())
        text = tr("Nothing was changed.");
    m_phase->setText(text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    if (ok) {
        QMessageBox::information(this, windowTitle(), text);
        QDialog::accept();
    } else {
        QMessageBox::warning(this, windowTitle(), text);
        updateState();
    }
}
