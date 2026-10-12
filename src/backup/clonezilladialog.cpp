// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "clonezilladialog.h"

#include "applog.h"
#include "blockio.h"
#include "dialogs.h"
#include "format.h"
#include "jobui.h"

#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

#include <memory>

ClonezillaDialog::ClonezillaDialog(UDisks *udisks, const Volume &target, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_target(target)
    , m_folder(new QLineEdit)
    , m_list(new QListWidget)
    , m_note(new QLabel)
    , m_confirm(new QLineEdit)
    , m_restore(new QPushButton(tr("Restore")))
    , m_bar(new QProgressBar)
{
    setWindowTitle(tr("Restore from Clonezilla onto %1").arg(volumeTitle(target)));
    m_folder->setObjectName(QStringLiteral("folder"));
    m_list->setObjectName(QStringLiteral("images"));
    m_note->setWordWrap(true);
    m_note->setObjectName(QStringLiteral("note"));
    m_confirm->setObjectName(QStringLiteral("confirm"));
    m_confirm->setPlaceholderText(shortDevice(target.device));
    m_restore->setObjectName(QStringLiteral("restore"));
    m_restore->setEnabled(false);
    m_bar->setRange(0, 1000);
    m_bar->hide();
    auto *browse = new QPushButton(tr("Browse…"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString folder = QFileDialog::getExistingDirectory(this, tr("Clonezilla's Backup Folder"), QDir::homePath());
        if (!folder.isEmpty())
            openFolder(folder);
    });
    // Looked at again only when it's another folder (losing focus finishes editing too).
    connect(m_folder, &QLineEdit::editingFinished, this, [this] {
        if (QDir::fromNativeSeparators(m_folder->text()) != m_opened)
            openFolder(m_folder->text());
    });
    connect(m_list, &QListWidget::currentRowChanged, this, &ClonezillaDialog::updateState);
    connect(m_confirm, &QLineEdit::textChanged, this, &ClonezillaDialog::updateState);
    connect(m_restore, &QPushButton::clicked, this, &ClonezillaDialog::start);
    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &ClonezillaDialog::reject);
    auto *folderRow = new QHBoxLayout;
    folderRow->addWidget(new QLabel(tr("Folder:")));
    folderRow->addWidget(m_folder, 1);
    folderRow->addWidget(browse);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(m_restore);
    buttons->addWidget(close);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("Clonezilla keeps a backup as a folder with an image for each partition (sda1.ext4-ptcl-img.gz.aa, "
                                       "and so on). Pick the folder, then the image to put back on <b>%1</b>.")
                                        .arg(volumeTitle(target).toHtmlEscaped())));
    layout->addLayout(folderRow);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_note);
    layout->addWidget(new QLabel(tr("Everything on %1 is replaced. Type <b>%2</b> to confirm:").arg(volumeTitle(target).toHtmlEscaped(), shortDevice(target.device))));
    layout->addWidget(m_confirm);
    layout->addWidget(m_bar);
    layout->addLayout(buttons);
    resize(620, 480);
}

ClonezillaDialog::~ClonezillaDialog()
{
    if (m_job)
        m_job->cancel();
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
}

void ClonezillaDialog::reject()
{
    if (m_job) {
        m_job->cancel();
        return;
    }
    QDialog::reject();
}

void ClonezillaDialog::openFolder(const QString &folder)
{
    m_folder->setText(QDir::toNativeSeparators(folder));
    m_opened = QDir::fromNativeSeparators(folder);
    m_images = partclone::scanFolder(m_opened);
    m_list->clear();
    for (const partclone::Image &i : std::as_const(m_images)) {
        QString text = i.raw ? tr("%1: a raw image").arg(i.partition)
                             : tr("%1: %2, %3 (%4 used)").arg(i.partition, i.fs.isEmpty() ? i.header.fs : i.fs, formatSize(i.header.deviceSize),
                                                              formatSize(i.header.usedBlocks * i.header.blockSize));
        if (!i.raw && !i.header.problem.isEmpty())
            text += QStringLiteral(" (") + i.header.problem + QLatin1Char(')');
        new QListWidgetItem(text, m_list);
    }
    m_note->setText(m_images.isEmpty() ? redText(tr("There are no Clonezilla images in that folder.")) : QString());
    updateState();
}

void ClonezillaDialog::updateState()
{
    const int row = m_list->currentRow();
    bool ok = row >= 0 && !m_job;
    if (row >= 0) {
        const partclone::Image &i = m_images.at(row);
        if (!i.raw && !i.header.problem.isEmpty()) {
            m_note->setText(redText(tr("It can't be restored: %1.").arg(i.header.problem).toHtmlEscaped()));
            ok = false;
        } else if (!i.raw && i.header.deviceSize > m_target.size) {
            m_note->setText(redText(tr("It came from a partition of %1, and %2 is smaller: make it at least that big first.")
                                        .arg(formatSize(i.header.deviceSize), volumeTitle(m_target))
                                        .toHtmlEscaped()));
            ok = false;
        } else {
            m_note->setText(i.raw ? tr("A raw image: it's written as it is.") : tr("It fits."));
        }
    }
    m_restore->setEnabled(ok && m_confirm->text().trimmed() == shortDevice(m_target.device) && !m_target.isSystem);
}

void ClonezillaDialog::start()
{
    const int row = m_list->currentRow();
    if (row < 0)
        return;
    const partclone::Image image = m_images.at(row);
    m_restore->setEnabled(false);
    m_confirm->setEnabled(false);
    m_note->setText(tr("Waiting for permission…"));
    // Opening it for writing unmounts it first.
    openBlockThen(m_udisks, this, m_target.objectPath, UDisks::OpenMode::ReadWrite, [this, image](int fd) {
        if (fd < 0) {
            m_note->setText(redText(tr("The partition couldn't be opened for writing.")));
            m_confirm->setEnabled(true);
            return;
        }
        qCInfo(lcOps).noquote() << "Clonezilla restore of" << image.files.value(0) << "onto" << m_target.device;
        m_bar->show();
        m_bar->setValue(0);
        m_note->setText(tr("Restoring…"));
        m_job = new partclone::Restore(image, fd, m_target.size);
        connect(m_job, &partclone::Restore::progress, this, [this](quint64 done, quint64 total) { m_bar->setValue(total ? int(done * 1000 / total) : 0); });
        connect(m_job, &partclone::Restore::finished, this, [this, fd](const partclone::Result &r) {
            blockio::close(fd);
            qCInfo(lcOps).noquote() << "Clonezilla restore" << (r.ok ? "done:" : "failed:") << r.written << r.error;
            m_thread->quit();
            m_thread->wait();
            m_thread = nullptr;
            m_job = nullptr;
            m_bar->setValue(1000);
            m_udisks->rescan(m_target.objectPath);
            m_note->setText(r.ok ? tr("Done: %1 written, every checksum right.").arg(formatSize(r.written)) : redText(r.error.toHtmlEscaped()));
            if (r.ok)
                QMessageBox::information(this, windowTitle(), tr("Restored onto %1.").arg(volumeTitle(m_target)));
            m_confirm->setEnabled(true);
        });
        m_thread = startOnThread(this, m_job);
    });
}
