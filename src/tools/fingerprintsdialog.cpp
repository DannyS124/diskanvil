// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fingerprintsdialog.h"

#include "dialogs.h"
#include "format.h"
#include "jobui.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

using fingerprints::State;

FingerprintsDialog::FingerprintsDialog(const QString &folder, QWidget *parent)
    : QDialog(parent)
    , m_folder(new QLineEdit(folder))
    , m_state(new QLabel)
    , m_record(new QPushButton(tr("Record Fingerprints")))
    , m_check(new QPushButton(tr("Check Now")))
    , m_stop(new QPushButton(tr("Stop")))
    , m_save(new QPushButton(tr("Save Report…")))
    , m_progress(new QProgressBar)
    , m_results(new QTreeWidget)
{
    qRegisterMetaType<QVector<fingerprints::Result>>();
    setWindowTitle(tr("Are My Files Still OK?"));
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("Photos, videos and papers kept for years can go bad without anyone touching them: a failing "
                                       "drive, a bad copy, \"bit rot\". The file's still there with the same date, but what's in it "
                                       "changed. Record a fingerprint of every file in a folder now, and check it again now and then: "
                                       "you'll know which files went bad while a good copy still exists somewhere.")));
    auto *row = new QHBoxLayout;
    m_folder->setObjectName(QStringLiteral("folder"));
    m_folder->setPlaceholderText(tr("A folder: Pictures, a backup drive…"));
    auto *browse = new QPushButton(tr("Choose…"));
    row->addWidget(new QLabel(tr("Folder:")));
    row->addWidget(m_folder, 1);
    row->addWidget(browse);
    layout->addLayout(row);
    m_state->setObjectName(QStringLiteral("state"));
    m_state->setWordWrap(true);
    layout->addWidget(m_state);
    m_progress->hide();
    layout->addWidget(m_progress);
    m_results->setObjectName(QStringLiteral("results"));
    m_results->setHeaderLabels({tr("How it is"), tr("File"), tr("Its date now")});
    m_results->setRootIsDecorated(false);
    m_results->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    layout->addWidget(m_results, 1);
    auto *buttons = new QHBoxLayout;
    m_record->setObjectName(QStringLiteral("record"));
    m_check->setObjectName(QStringLiteral("check"));
    m_save->setEnabled(false);
    m_stop->hide();
    auto *close = new QPushButton(tr("Close"));
    buttons->addWidget(m_save);
    buttons->addStretch();
    buttons->addWidget(m_stop);
    buttons->addWidget(m_record);
    buttons->addWidget(m_check);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Which Folder?"), m_folder->text().isEmpty() ? QDir::homePath() : m_folder->text());
        if (!dir.isEmpty())
            m_folder->setText(dir);
    });
    connect(m_folder, &QLineEdit::textChanged, this, &FingerprintsDialog::folderChanged);
    connect(m_record, &QPushButton::clicked, this, &FingerprintsDialog::record);
    connect(m_check, &QPushButton::clicked, this, &FingerprintsDialog::check);
    connect(m_stop, &QPushButton::clicked, this, [this] {
        if (m_job)
            m_job->cancel();
    });
    connect(m_save, &QPushButton::clicked, this, &FingerprintsDialog::saveReport);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    resize(820, 560);
    folderChanged();
}

FingerprintsDialog::~FingerprintsDialog()
{
    if (m_job)
        m_job->cancel();
}

void FingerprintsDialog::folderChanged()
{
    const QString folder = m_folder->text().trimmed();
    const bool ok = !folder.isEmpty() && QFileInfo(folder).isDir();
    bool exists = false;
    const QString book = ok ? fingerprints::bookFor(folder, &exists) : QString();
    fingerprints::Book b;
    QString error;
    if (!ok) {
        m_state->setText(folder.isEmpty() ? tr("Choose a folder.") : tr("There's no folder there."));
    } else if (exists && fingerprints::read(book, &b, &error)) {
        m_state->setText(tr("Fingerprints of %n file(s) were recorded on %1. Check Now compares them with the files as they are.", nullptr, int(b.files.size()))
                             .arg(QLocale().toString(b.made.toLocalTime(), QLocale::ShortFormat)));
    } else {
        m_state->setText(tr("No fingerprints for this folder yet: Record Fingerprints makes them (in the folder, as \"%1\"; it's read "
                            "every file once).").arg(QString::fromLatin1(fingerprints::kFileName)));
    }
    m_record->setEnabled(ok && !m_job);
    m_check->setEnabled(ok && exists && !m_job);
    m_record->setText(exists ? tr("Record Again") : tr("Record Fingerprints"));
}

void FingerprintsDialog::record()
{
    bool exists = false;
    fingerprints::bookFor(m_folder->text().trimmed(), &exists);
    if (exists && !m_last.isEmpty()) {
        int bad = 0;
        for (const fingerprints::Result &r : std::as_const(m_last))
            bad += r.state == State::Damaged || r.state == State::Unreadable;
        if (bad > 0
            && QMessageBox::question(this, windowTitle(),
                                     tr("%n file(s) went bad. Recording again takes them as they are now, damage and all: put good "
                                        "copies back first. Record again anyway?", nullptr, bad))
                   != QMessageBox::Yes)
            return;
    }
    start(fingerprints::Job::Mode::Record);
}

void FingerprintsDialog::check()
{
    start(fingerprints::Job::Mode::Check);
}

void FingerprintsDialog::start(fingerprints::Job::Mode mode)
{
    if (m_job)
        return;
    m_mode = mode;
    m_results->clear();
    m_save->setEnabled(false);
    m_record->setEnabled(false);
    m_check->setEnabled(false);
    m_folder->setEnabled(false);
    m_stop->show();
    m_progress->show();
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    auto *job = new fingerprints::Job(m_folder->text().trimmed(), mode);
    m_job = job;
    connect(job, &fingerprints::Job::progress, this, [this](quint64 done, quint64 total, const QString &file) {
        m_progress->setValue(total ? int(done * 1000 / total) : 0);
        if (!file.isEmpty())
            m_state->setText(tr("Reading %1 (%2 of %3)…").arg(file, formatSize(done), formatSize(total)));
    });
    connect(job, &fingerprints::Job::finished, this, &FingerprintsDialog::finished);
    QThread *thread = startOnThread(this, job);
    connect(job, &fingerprints::Job::finished, thread, &QThread::quit);
}

void FingerprintsDialog::finished(bool ok, const QVector<fingerprints::Result> &results, const QString &book, const QString &error)
{
    m_job = nullptr;
    m_stop->hide();
    m_progress->hide();
    m_folder->setEnabled(true);
    folderChanged();
    if (!ok) {
        m_state->setText(error);
        emit ran(false);
        return;
    }
    QVector<fingerprints::Result> sorted = results;
    // Worst first.
    auto rank = [](State s) {
        switch (s) {
        case State::Damaged:
            return 0;
        case State::Unreadable:
            return 1;
        case State::Missing:
            return 2;
        case State::Edited:
            return 3;
        case State::New:
            return 4;
        case State::Same:
            return 5;
        }
        return 6;
    };
    std::stable_sort(sorted.begin(), sorted.end(), [&](const auto &a, const auto &b) { return rank(a.state) < rank(b.state); });
    m_last = sorted;
    int counts[6] = {};
    for (const fingerprints::Result &r : std::as_const(sorted)) {
        ++counts[rank(r.state)];
        if (r.state == State::Same)
            continue; // only what's worth a look is listed
        auto *item = new QTreeWidgetItem({fingerprints::stateName(r.state), r.path,
                                          r.modified.isValid() ? QLocale().toString(r.modified, QLocale::ShortFormat) : QString()});
        if (r.state == State::Damaged || r.state == State::Unreadable)
            item->setForeground(0, QColor(0xd0, 0x30, 0x40));
        if (!r.detail.isEmpty())
            item->setToolTip(0, r.detail);
        m_results->addTopLevelItem(item);
    }
    m_results->resizeColumnToContents(0);
    if (m_mode == fingerprints::Job::Mode::Record) {
        QString text = tr("Fingerprints recorded, kept in %1.").arg(QDir::toNativeSeparators(book));
        if (counts[1] > 0)
            text += QStringLiteral(" ") + tr("%n file(s) couldn't be read, so they have none: they're listed below.", nullptr, counts[1]);
        m_state->setText(text);
    } else {
        QStringList parts;
        parts << tr("%n file(s) the same", nullptr, counts[5]);
        if (counts[0])
            parts << tr("%n damaged", nullptr, counts[0]);
        if (counts[1])
            parts << tr("%n can't be read", nullptr, counts[1]);
        if (counts[2])
            parts << tr("%n missing", nullptr, counts[2]);
        if (counts[3])
            parts << tr("%n changed by being saved since", nullptr, counts[3]);
        if (counts[4])
            parts << tr("%n new", nullptr, counts[4]);
        QString text = parts.join(QStringLiteral(", ")) + QLatin1Char('.');
        if (counts[0] || counts[1])
            text += QStringLiteral(" ") + tr("Damaged files changed though nobody saved them: put them back from a backup or another copy. "
                                             "Several at once usually means the drive is failing: look at Disk Health, and copy what's "
                                             "on it off soon.");
        else if (!counts[2])
            text += QStringLiteral(" ") + tr("Nothing went bad.");
        m_state->setText(text);
        m_save->setEnabled(true);
    }
    emit ran(true);
}

void FingerprintsDialog::saveReport()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Save the Report"), QDir::homePath() + QStringLiteral("/Are my files still OK.txt"), tr("Text (*.txt)"));
    if (path.isEmpty())
        return;
    QStringList lines{tr("Are My Files Still OK? %1").arg(QDir::toNativeSeparators(m_folder->text())), m_state->text(), QString()};
    for (const fingerprints::Result &r : std::as_const(m_last))
        if (r.state != State::Same)
            lines << fingerprints::stateName(r.state) + QStringLiteral(": ") + r.path;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write((lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8()) < 0)
        QMessageBox::warning(this, windowTitle(), tr("Couldn't save it: %1").arg(f.errorString()));
}
