// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "savefilesdialog.h"

#include "applog.h"
#include "blockio.h"
#include "dialogs.h"
#include "format.h"
#include "jobui.h"
#include "udisks.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QThread>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <memory>

namespace {

enum Role { kFolder = Qt::UserRole, kBrowser, kPath, kDisk };

bool hasPeople(const QString &root)
{
    return QDir(root + QStringLiteral("/Users")).exists() || QDir(root + QStringLiteral("/home")).exists();
}

} // namespace

SaveFilesDialog::SaveFilesDialog(UDisks *udisks, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_pages(new QStackedWidget)
    , m_drives(new QListWidget)
    , m_driveNote(new QLabel)
    , m_tree(new QTreeWidget)
    , m_to(new QLineEdit)
    , m_toNote(new QLabel)
    , m_start(new QPushButton(tr("Save")))
    , m_bar(new QProgressBar)
    , m_progress(new QLabel)
    , m_summary(new QLabel)
    , m_stop(new QPushButton(tr("Stop")))
    , m_openFolder(new QPushButton(tr("Open the Folder")))
{
    setWindowTitle(tr("Save My Files"));

    // 1: which drive.
    auto *pick = new QWidget;
    auto *pickLayout = new QVBoxLayout(pick);
    pickLayout->addWidget(wrappingLabel(tr("Copies each person's Desktop, Documents, Pictures, Music, Videos and Downloads, and their "
                                           "browsers' bookmarks, onto another drive: before reinstalling, or when a PC won't start. "
                                           "Which drive are they on?")));
    m_drives->setObjectName(QStringLiteral("drives"));
    pickLayout->addWidget(m_drives, 1);
    m_driveNote->setWordWrap(true);
    pickLayout->addWidget(m_driveNote);
    auto *next = new QPushButton(tr("Next"));
    next->setObjectName(QStringLiteral("next"));
    next->setEnabled(false);
    connect(m_drives, &QListWidget::currentRowChanged, next, [next](int row) { next->setEnabled(row >= 0); });
    connect(next, &QPushButton::clicked, this, &SaveFilesDialog::chooseDrive);
    connect(m_drives, &QListWidget::itemDoubleClicked, this, &SaveFilesDialog::chooseDrive);
    pickLayout->addWidget(next, 0, Qt::AlignRight);

    // 2: what, and where to.
    auto *what = new QWidget;
    auto *whatLayout = new QVBoxLayout(what);
    m_tree->setHeaderLabels({tr("What"), tr("Size"), tr("Files")});
    m_tree->setObjectName(QStringLiteral("found"));
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    whatLayout->addWidget(m_tree, 1);
    auto *toRow = new QHBoxLayout;
    toRow->addWidget(new QLabel(tr("Save to:")));
    m_to->setObjectName(QStringLiteral("to"));
    toRow->addWidget(m_to, 1);
    auto *browse = new QPushButton(tr("Browse…"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString folder = QFileDialog::getExistingDirectory(this, tr("Save to"), m_to->text().isEmpty() ? QDir::homePath() : m_to->text());
        if (!folder.isEmpty())
            m_to->setText(QDir::toNativeSeparators(folder));
    });
    toRow->addWidget(browse);
    whatLayout->addLayout(toRow);
    m_toNote->setWordWrap(true);
    whatLayout->addWidget(m_toNote);
    m_start->setObjectName(QStringLiteral("save"));
    whatLayout->addWidget(m_start, 0, Qt::AlignRight);
    connect(m_to, &QLineEdit::textChanged, this, &SaveFilesDialog::updateTarget);
    connect(m_tree, &QTreeWidget::itemChanged, this, &SaveFilesDialog::updateTarget);
    connect(m_start, &QPushButton::clicked, this, &SaveFilesDialog::start);

    // 3: copying, then what was done.
    auto *copying = new QWidget;
    auto *copyLayout = new QVBoxLayout(copying);
    m_progress->setWordWrap(true);
    m_summary->setWordWrap(true);
    m_summary->setObjectName(QStringLiteral("summary"));
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    copyLayout->addWidget(m_bar);
    copyLayout->addWidget(m_progress);
    copyLayout->addWidget(m_summary, 1);
    auto *copyButtons = new QHBoxLayout;
    copyButtons->addStretch();
    copyButtons->addWidget(m_openFolder);
    copyButtons->addWidget(m_stop);
    copyLayout->addLayout(copyButtons);
    m_openFolder->hide();
    connect(m_openFolder, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(m_savedTo)); });
    connect(m_stop, &QPushButton::clicked, this, [this] {
        if (m_saver)
            m_saver->cancel();
        else
            accept();
    });

    m_pages->addWidget(pick);
    m_pages->addWidget(what);
    m_pages->addWidget(copying);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_pages);
    fillDrives();
    resize(680, 520);
}

SaveFilesDialog::~SaveFilesDialog()
{
    m_stopLooking = true;
    if (m_saver)
        m_saver->cancel();
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
}

void SaveFilesDialog::reject()
{
    if (m_saver) {
        m_saver->cancel(); // it finishes the file it's on, then says so
        return;
    }
    QDialog::reject();
}

void SaveFilesDialog::fillDrives()
{
    m_drives->clear();
    for (const Disk &d : m_udisks->disks()) {
        for (const Volume &v : d.volumes) {
            const QString fs = v.effectiveFsType();
            const QStringList mounts = v.mounts();
            QString text;
            if (!mounts.isEmpty() && std::any_of(mounts.cbegin(), mounts.cend(), hasPeople))
                text = tr("%1, at %2").arg(volumeTitle(v), QDir::toNativeSeparators(mounts.first()));
            else if (mounts.isEmpty() && v.canMount()
                     && QStringList{QStringLiteral("ntfs"), QStringLiteral("ext4"), QStringLiteral("btrfs"), QStringLiteral("xfs"), QStringLiteral("exfat"),
                                    QStringLiteral("vfat"), QStringLiteral("hfsplus"), QStringLiteral("apfs"), QStringLiteral("f2fs"), QStringLiteral("ext3")}
                            .contains(fs))
                text = tr("%1: not mounted (it's mounted read-only to look)").arg(volumeTitle(v));
            if (text.isEmpty())
                continue;
            auto *item = new QListWidgetItem(text, m_drives);
            item->setData(kPath, v.objectPath);
            item->setData(kDisk, d.device);
        }
    }
    m_driveNote->setText(m_drives->count() == 0 ? tr("No drive with people's folders on it was found. One locked with BitLocker or "
                                                     "LUKS has to be unlocked first.")
                                                : QString());
}

void SaveFilesDialog::chooseDrive()
{
    QListWidgetItem *item = m_drives->currentItem();
    if (!item)
        return;
    const QString path = item->data(kPath).toString(), disk = item->data(kDisk).toString();
    const auto volume = [this, path]() -> const Volume * {
        for (const Disk &d : m_udisks->disks()) {
            for (const Volume &v : d.volumes) {
                if (v.objectPath == path)
                    return &v;
            }
        }
        return nullptr;
    };
    const Volume *v = volume();
    if (!v)
        return fillDrives();
    for (const QString &mount : v->mounts()) {
        if (hasPeople(mount))
            return lookAt(mount, disk);
    }
    if (!v->mounts().isEmpty()) {
        m_driveNote->setText(redText(tr("There are no people's folders on it.")));
        return;
    }
    // Read-only: nothing on it changes, and a drive Windows left hibernated can still be read.
    m_driveNote->setText(tr("Mounting it, read-only…"));
    auto mounted = std::make_shared<QMetaObject::Connection>();
    auto failed = std::make_shared<QMetaObject::Connection>();
    *mounted = connect(m_udisks, &UDisks::changed, this, [this, volume, disk, mounted, failed] {
        const Volume *now = volume();
        if (!now || now->mounts().isEmpty())
            return;
        disconnect(*mounted);
        disconnect(*failed);
        if (!hasPeople(now->mounts().first())) {
            m_driveNote->setText(redText(tr("There are no people's folders on it.")));
            return;
        }
        lookAt(now->mounts().first(), disk);
    });
    *failed = connect(m_udisks, &UDisks::operationFinished, this, [this, mounted, failed](bool ok, const QString &message) {
        if (ok)
            return;
        disconnect(*mounted);
        disconnect(*failed);
        m_driveNote->setText(redText(message.toHtmlEscaped()));
    });
    m_udisks->mount(*v, true);
}

void SaveFilesDialog::lookAt(const QString &root, const QString &disk)
{
    m_root = root;
    m_disk = disk;
    m_driveNote->setText(tr("Looking for people's folders and counting what's in them…"));
    m_drives->setEnabled(false);
    m_thread = QThread::create([this, root] {
        const savefiles::Found found = savefiles::look(root, &m_stopLooking);
        QMetaObject::invokeMethod(this, [this, found] {
            m_thread->wait();
            delete m_thread;
            m_thread = nullptr;
            m_found = found;
            m_drives->setEnabled(true);
            showFound();
        });
    });
    m_thread->setParent(this); // gone with the window, if it's closed first
    m_thread->start();
}

void SaveFilesDialog::showFound()
{
    const QSignalBlocker block(m_tree);
    m_tree->clear();
    for (const QString &user : std::as_const(m_found.users)) {
        auto *person = new QTreeWidgetItem(m_tree, {user});
        person->setFlags(person->flags() | Qt::ItemIsAutoTristate | Qt::ItemIsUserCheckable);
        person->setCheckState(0, Qt::Checked);
        for (int i = 0; i < m_found.folders.size(); ++i) {
            const savefiles::Folder &f = m_found.folders.at(i);
            if (f.user != user)
                continue;
            auto *item = new QTreeWidgetItem(person, {f.name, formatSize(f.size), QLocale().toString(f.files)});
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(0, Qt::Checked);
            item->setData(0, kFolder, i);
            item->setToolTip(0, QDir::toNativeSeparators(f.path));
        }
        for (int i = 0; i < m_found.browsers.size(); ++i) {
            const savefiles::Browser &b = m_found.browsers.at(i);
            if (b.user != user)
                continue;
            auto *item = new QTreeWidgetItem(person, {b.profile.isEmpty() ? tr("%1's bookmarks").arg(b.name) : tr("%1's bookmarks (%2)").arg(b.name, b.profile)});
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(0, Qt::Checked);
            item->setData(0, kBrowser, i);
            item->setData(0, kFolder, -1);
        }
        person->setExpanded(true);
    }
    for (int c = 1; c < m_tree->columnCount(); ++c)
        m_tree->resizeColumnToContents(c);
    if (m_found.users.isEmpty()) {
        m_driveNote->setText(redText(tr("There are no people's folders with anything in them on it.")));
        return;
    }
    m_pages->setCurrentIndex(1);
    updateTarget();
}

void SaveFilesDialog::updateTarget()
{
    quint64 bytes = 0;
    int picked = 0;
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        if ((*it)->checkState(0) != Qt::Checked || !(*it)->parent())
            continue;
        ++picked;
        const int folder = (*it)->data(0, kFolder).toInt();
        if (folder >= 0 && (*it)->data(0, kBrowser).isNull())
            bytes += m_found.folders.at(folder).size;
    }
    const QString to = QDir::fromNativeSeparators(m_to->text().trimmed());
    QString note;
    bool ok = picked > 0 && !to.isEmpty();
    if (!to.isEmpty()) {
        const QStorageInfo where(to);
        if (!QDir(to).exists()) {
            note = redText(tr("That folder isn't there."));
            ok = false;
        } else if (!m_disk.isEmpty() && blockio::pathIsOnDisk(to, m_disk)) {
            note = redText(tr("That's on the drive the files are on: pick another drive (a USB drive, say)."));
            ok = false;
        } else if (where.isValid() && quint64(where.bytesAvailable()) < bytes) {
            note = redText(tr("It needs %1, and there's %2 free there.").arg(formatSize(bytes), formatSize(quint64(where.bytesAvailable()))));
            ok = false;
        } else {
            note = tr("%1 to copy; %2 free there.").arg(formatSize(bytes), formatSize(quint64(where.bytesAvailable())));
        }
    }
    m_toNote->setText(note);
    m_start->setEnabled(ok);
}

void SaveFilesDialog::start()
{
    QVector<savefiles::Folder> folders;
    QVector<savefiles::Browser> browsers;
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        if ((*it)->checkState(0) != Qt::Checked || !(*it)->parent())
            continue;
        if (!(*it)->data(0, kBrowser).isNull())
            browsers << m_found.browsers.at((*it)->data(0, kBrowser).toInt());
        else
            folders << m_found.folders.at((*it)->data(0, kFolder).toInt());
    }
    const QString to = QDir::fromNativeSeparators(m_to->text().trimmed());
    qCInfo(lcOps).noquote() << "Save My Files:" << folders.size() << "folders," << browsers.size() << "bookmark files from" << m_root << "to" << to;
    m_pages->setCurrentIndex(2);
    m_bar->setRange(0, 1000);
    m_bar->setValue(0);
    m_summary->clear();
    m_saver = new savefiles::Saver(folders, browsers, to);
    connect(m_saver, &savefiles::Saver::progress, this, [this](const QString &file, quint64 done, quint64 total) {
        m_bar->setValue(total ? int(done * 1000 / total) : 0);
        m_progress->setText(tr("Copying %1").arg(QDir::toNativeSeparators(file)));
    });
    connect(m_saver, &savefiles::Saver::finished, this, [this](const savefiles::Result &r) {
        qCInfo(lcOps).noquote() << "Save My Files:" << r.files << "files," << r.failed.size() << "not saved, to" << r.folder;
        m_thread->quit();
        m_thread->wait();
        m_thread = nullptr;
        m_saver = nullptr;
        m_savedTo = r.folder;
        m_bar->setValue(1000);
        m_progress->setText(QString());
        QString text = (r.stopped ? tr("Stopped. ") : QString()) + tr("Saved %n file(s), %1, to %2.", nullptr, r.files).arg(formatSize(r.bytes), QDir::toNativeSeparators(r.folder));
        if (!r.bookmarks.isEmpty())
            text += QStringLiteral("\n\n") + tr("Bookmarks are in each person's Bookmarks folder, as .html files: in any browser, Import "
                                                 "bookmarks from an HTML file.");
        if (!r.failed.isEmpty())
            text += QStringLiteral("\n\n") + tr("Not saved (\"What was saved.txt\" lists them too):") + QLatin1Char('\n') + r.failed.mid(0, 30).join(QLatin1Char('\n'))
                  + (r.failed.size() > 30 ? QStringLiteral("\n") + tr("…and %n more", nullptr, int(r.failed.size() - 30)) : QString());
        m_summary->setText(text);
        m_openFolder->show();
        m_stop->setText(tr("Close"));
    });
    m_thread = startOnThread(this, m_saver);
}
