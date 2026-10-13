// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hitfilesdialog.h"

#include "format.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QThreadPool>
#include <QTreeWidget>
#include <QVBoxLayout>

HitFilesDialog::HitFilesDialog(std::shared_ptr<lost::Source> copy, const RescueMap &map, const QString &copyName, const QString &saveTo, QWidget *parent)
    : QDialog(parent)
    , m_copyName(copyName)
    , m_saveTo(saveTo)
{
    setWindowTitle(tr("Which Files Were Hit?"));
    resize(760, 520);
    auto *layout = new QVBoxLayout(this);
    m_summary = new QLabel(tr("Reading the copy's file systems…"));
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);
    m_list = new QTreeWidget;
    m_list->setObjectName(QStringLiteral("hits"));
    m_list->setHeaderLabels({tr("File"), tr("Size"), tr("Missing"), tr("Note")});
    m_list->setRootIsDecorated(false);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(m_list, 1);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    m_save = box->addButton(tr("Save List…"), QDialogButtonBox::ActionRole);
    m_save->setEnabled(false);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_save, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Save the List"), m_saveTo, tr("Text (*.txt)"));
        if (path.isEmpty())
            return;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(hitfiles::asText(m_report, m_copyName).toUtf8()) < 0)
            QMessageBox::warning(this, windowTitle(), tr("Couldn't save it: %1").arg(f.errorString()));
    });
    layout->addWidget(box);

    // Reading the file systems takes a while on a big copy: on a thread, its result back here.
    QPointer<HitFilesDialog> self(this);
    const auto cancel = m_cancel;
    QThreadPool::globalInstance()->start([self, copy, map, cancel] {
        const hitfiles::Report report = hitfiles::find(*copy, map, *cancel, [self](const QString &line) {
            QMetaObject::invokeMethod(qApp, [self, line] {
                if (self && !self->m_done)
                    self->m_summary->setText(line);
            }, Qt::QueuedConnection);
        });
        QMetaObject::invokeMethod(qApp, [self, report] {
            if (self)
                self->showReport(report);
        }, Qt::QueuedConnection);
    });
}

HitFilesDialog::~HitFilesDialog()
{
    *m_cancel = true;
}

void HitFilesDialog::showReport(const hitfiles::Report &r)
{
    m_report = r;
    m_done = true;
    int hit = 0;
    for (const hitfiles::Hit &h : r.hits) {
        if (!h.deleted)
            ++hit;
        QStringList notes;
        if (h.deleted)
            notes << tr("deleted");
        if (h.guessed)
            notes << tr("where its pieces are is a guess");
        if (h.packed)
            notes << tr("compressed by Windows: more of it may be lost");
        auto *item = new QTreeWidgetItem({h.path, formatSize(h.size), formatSize(h.missing), notes.join(QStringLiteral(", "))});
        item->setToolTip(0, h.path);
        m_list->addTopLevelItem(item);
    }
    m_list->resizeColumnToContents(1);
    m_list->resizeColumnToContents(2);
    QString text;
    if (r.missing == 0)
        text = tr("Everything was read: every file came off whole.");
    else if (r.volumes.isEmpty())
        text = tr("%1 couldn't be read, but no file system DiskAnvil can list (FAT, exFAT, NTFS) was found on the copy, so it can't "
                  "say which files those parts belonged to.")
                   .arg(formatSize(r.missing));
    else {
        text = tr("%1 couldn't be read.").arg(formatSize(r.missing));
        if (hit > 0)
            text += QLatin1Char(' ') + tr("Files with blank parts in the copy: %1 (%2 of what wasn't read).").arg(hit).arg(formatSize(r.inFiles));
        else
            text += QLatin1Char(' ') + tr("None of it was in a file.");
        text += QLatin1Char(' ') + tr("Files that came off whole: %1.").arg(std::max(0, r.files - hit));
        if (r.missing > r.inFiles)
            text += QLatin1Char(' ') + tr("The other %1 was in free space or the file system's own records.").arg(formatSize(r.missing - r.inFiles));
    }
    for (const QString &u : r.unread)
        text += QStringLiteral("\n") + tr("Not listed: %1.").arg(u);
    m_summary->setText(text);
    m_save->setEnabled(true);
    emit finished();
}
