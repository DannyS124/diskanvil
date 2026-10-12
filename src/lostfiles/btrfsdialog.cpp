// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "btrfsdialog.h"

#include "blockio.h"
#include "hostprocess.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVBoxLayout>

BtrfsDialog::BtrfsDialog(const QString &device, const QString &diskDevice, const QString &title, QWidget *parent, bool privileged, quint64 offset,
                         quint64 size)
    : QDialog(parent)
    , m_device(device)
    , m_disk(diskDevice)
    , m_privileged(privileged)
    , m_trees(new QComboBox)
    , m_list(new QPushButton(tr("Show What It Holds")))
    , m_onlyGone(new QCheckBox(tr("Only what isn't there now")))
    , m_files(new QTreeWidget)
    , m_copyTicked(new QPushButton(tr("Copy the Ticked Ones To…")))
    , m_copyAll(new QPushButton(tr("Copy All of It To…")))
    , m_status(new QLabel)
{
    setWindowTitle(tr("Older Copies of %1 (btrfs)").arg(title));
    setObjectName(QStringLiteral("btrfs"));
    resize(760, 560);
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("btrfs writes changes to new places, and keeps the older copies of its list of files until the room is "
                                "needed. An older copy can still hold files deleted since. They're read with btrfs-progs; nothing is "
                                "written to the drive."));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *pick = new QHBoxLayout;
    m_trees->setObjectName(QStringLiteral("trees"));
    pick->addWidget(m_trees, 1);
    m_list->setObjectName(QStringLiteral("showTree"));
    pick->addWidget(m_list);
    layout->addLayout(pick);
    m_onlyGone->setObjectName(QStringLiteral("onlyGone"));
    m_onlyGone->setChecked(true);
    layout->addWidget(m_onlyGone);
    m_files->setObjectName(QStringLiteral("treeFiles"));
    m_files->setHeaderLabels({tr("File"), tr("Now")});
    m_files->setRootIsDecorated(false);
    m_files->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(m_files, 1);
    m_status->setObjectName(QStringLiteral("btrfsStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    for (QPushButton *b : {m_copyTicked, m_copyAll})
        b->setAutoDefault(false);
    m_copyTicked->setObjectName(QStringLiteral("copyTicked"));
    m_copyAll->setObjectName(QStringLiteral("copyAll"));
    auto *close = new QPushButton(tr("Close"));
    buttons->addWidget(m_copyTicked);
    buttons->addWidget(m_copyAll);
    buttons->addWidget(close);
    layout->addLayout(buttons);
    m_list->setEnabled(false);
    m_copyTicked->setEnabled(false);
    m_copyAll->setEnabled(false);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_list, &QPushButton::clicked, this, &BtrfsDialog::listTree);
    connect(m_onlyGone, &QCheckBox::toggled, this, &BtrfsDialog::showList);
    connect(m_copyTicked, &QPushButton::clicked, this, [this] { copy(true); });
    connect(m_copyAll, &QPushButton::clicked, this, [this] { copy(false); });
    if (!m_privileged) {
        findTrees();
        return;
    }
    // On DiskAnvil Live: a read-only loop device over just the file system, read as root.
    m_status->setText(tr("Getting the drive ready to read…"));
    QStringList losetup = {QStringLiteral("losetup"), QStringLiteral("--find"), QStringLiteral("--show"), QStringLiteral("--read-only")};
    if (offset)
        losetup << QStringLiteral("--offset") << QString::number(offset);
    if (size)
        losetup << QStringLiteral("--sizelimit") << QString::number(size);
    run(losetup << device, [this](int code, const QString &output) {
        m_loop = output.trimmed().section(QLatin1Char('\n'), -1);
        if (code != 0 || !m_loop.startsWith(QLatin1String("/dev/loop"))) {
            m_status->setText(tr("The drive couldn't be got ready: %1").arg(output.trimmed()));
            m_loop.clear();
            return;
        }
        m_device = m_loop;
        findTrees();
    });
}

BtrfsDialog::~BtrfsDialog()
{
    if (m_process) {
        m_process->kill();
        m_process->waitForFinished(3000);
    }
    if (!m_loop.isEmpty())
        QProcess::execute(QStringLiteral("sudo"), {QStringLiteral("-n"), QStringLiteral("losetup"), QStringLiteral("-d"), m_loop});
}

void BtrfsDialog::run(const QStringList &command, const std::function<void(int, const QString &)> &then)
{
    const QStringList full = m_privileged ? QStringList{QStringLiteral("sudo"), QStringLiteral("-n")} + command : host::command(command);
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::finished, this, [this, then](int code) {
        const QString output = QString::fromUtf8(m_process->readAll());
        m_process->deleteLater();
        m_process = nullptr;
        then(code, output);
    });
    m_process->start(full.first(), full.mid(1));
}

void BtrfsDialog::findTrees()
{
    m_status->setText(tr("Looking for the copies of its file tree…"));
    run(btrfsrestore::findTreesCommand(m_device), [this](int, const QString &output) {
        m_found = btrfsrestore::parseTrees(output);
        if (m_found.isEmpty()) {
            m_status->setText(tr("btrfs-progs found no copies of its file tree: %1").arg(output.trimmed().section(QLatin1Char('\n'), -1)));
            return;
        }
        for (const btrfsrestore::Tree &t : std::as_const(m_found))
            m_trees->addItem(t.at == 0 ? tr("As it is now (copy %1)").arg(t.generation) : tr("An older copy (%1)").arg(t.generation));
        m_trees->setCurrentIndex(m_found.size() > 1 ? 1 : 0); // the newest older one: what was deleted last is in it
        // What the one in use has, to tell what isn't there any more.
        run(btrfsrestore::restoreCommand(m_device, 0, m_nowhere.path(), true), [this](int, const QString &output) {
            m_now = btrfsrestore::parseRestored(output, m_nowhere.path());
            m_status->setText(QCoreApplication::translate("BtrfsDialog", "%n copy(ies) of its file tree found. Pick one and show what it holds.",
                                                          nullptr, int(m_found.size())));
            m_list->setEnabled(true);
            if (m_found.size() > 1)
                listTree();
        });
    });
}

void BtrfsDialog::listTree()
{
    const int i = m_trees->currentIndex();
    if (i < 0 || i >= m_found.size() || m_process)
        return;
    m_list->setEnabled(false);
    m_status->setText(tr("Reading what that copy holds…"));
    run(btrfsrestore::restoreCommand(m_device, m_found[i].at, m_nowhere.path(), true), [this](int, const QString &output) {
        m_list->setEnabled(true);
        m_listed = btrfsrestore::parseRestored(output, m_nowhere.path());
        // Folders are the ones it says it's done searching.
        static const QRegularExpression done(QStringLiteral("Done searching (/[^\\n]*)"));
        m_folders.clear();
        for (auto it = done.globalMatch(output); it.hasNext();)
            m_folders << it.next().captured(1).trimmed();
        showList();
    });
}

void BtrfsDialog::showList()
{
    m_files->clear();
    const QSet<QString> now(m_now.cbegin(), m_now.cend()), folders(m_folders.cbegin(), m_folders.cend());
    int files = 0, gone = 0;
    for (const QString &path : std::as_const(m_listed)) {
        if (folders.contains(path))
            continue;
        ++files;
        const bool there = now.contains(path);
        gone += !there;
        if (m_onlyGone->isChecked() && there)
            continue;
        auto *item = new QTreeWidgetItem(m_files, {path, there ? tr("there") : tr("gone")});
        item->setCheckState(0, there ? Qt::Unchecked : Qt::Checked);
        if (!there) {
            QFont bold = item->font(1);
            bold.setBold(true);
            item->setFont(1, bold);
        }
    }
    m_status->setText(QCoreApplication::translate("BtrfsDialog", "That copy holds %n file(s)", nullptr, files) + QStringLiteral(", ")
                      + QCoreApplication::translate("BtrfsDialog", "%n of them not there now (deleted, or changed since).", nullptr, gone));
    m_copyTicked->setEnabled(m_files->topLevelItemCount() > 0);
    m_copyAll->setEnabled(files > 0);
}

void BtrfsDialog::copy(bool tickedOnly)
{
    const int i = m_trees->currentIndex();
    if (i < 0 || i >= m_found.size() || m_process)
        return;
    QStringList ticked;
    for (int k = 0; k < m_files->topLevelItemCount(); ++k) {
        if (m_files->topLevelItem(k)->checkState(0) == Qt::Checked)
            ticked << m_files->topLevelItem(k)->text(0);
    }
    if (tickedOnly && ticked.isEmpty())
        return;
    const QString folder = !m_testTarget.isEmpty() ? m_testTarget
                                                   : QFileDialog::getExistingDirectory(this, tr("Copy Them Where?"),
                                                                                       QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
    if (folder.isEmpty())
        return;
    if (!m_disk.isEmpty() && blockio::pathIsOnDisk(folder, m_disk)) {
        m_status->setText(tr("That folder is on the drive the files are being got back from: copying there could overwrite them. Pick one on "
                             "another drive."));
        return;
    }
    const QString target = folder + QLatin1Char('/') + tr("From btrfs copy %1").arg(m_found[i].generation);
    QDir().mkpath(target);
    m_copyTicked->setEnabled(false);
    m_copyAll->setEnabled(false);
    m_status->setText(tr("Copying…"));
    run(btrfsrestore::restoreCommand(m_device, m_found[i].at, target, false, tickedOnly ? ticked : QStringList()),
        [this, target](int code, const QString &output) {
            const QSet<QString> folders(m_folders.cbegin(), m_folders.cend());
            int copied = 0;
            for (const QString &path : btrfsrestore::parseRestored(output, target))
                copied += !folders.contains(path);
            m_status->setText(code == 0 ? QCoreApplication::translate("BtrfsDialog", "Copied %n file(s) to %1.", nullptr, copied).arg(QDir::toNativeSeparators(target))
                                        : tr("btrfs restore stopped with a problem: %1").arg(output.trimmed().section(QLatin1Char('\n'), -1)));
            m_copyTicked->setEnabled(true);
            m_copyAll->setEnabled(true);
        });
}
