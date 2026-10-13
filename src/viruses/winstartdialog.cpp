// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "winstartdialog.h"

#include "applog.h"
#include "dialogs.h"
#include "jobui.h"
#include "lostsource.h"
#include "udisks.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QThreadPool>
#include <QTreeWidget>
#include <QVBoxLayout>

WinStartDialog::WinStartDialog(UDisks *udisks, const QString &root, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_root(root)
    , m_update(new QLabel(tr("Looking…")))
    , m_undo(new QPushButton(tr("Undo the Stuck Update")))
    , m_hives(new QTreeWidget)
    , m_copies(new QListWidget)
    , m_copiesNote(new QLabel)
    , m_putBack(new QPushButton(tr("Put This One Back")))
    , m_log(new QPlainTextEdit)
{
    setWindowTitle(tr("Windows Won't Start"));
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("Two common reasons Windows stops starting, fixed from outside it. Anything changed is kept "
                                       "next to itself, renamed, so it can be put back. Save your files first if you haven't "
                                       "(Save My Files).")));

    auto *update = new QGroupBox(tr("A stuck update (\"Undoing changes\", \"Working on updates\" for ever)"));
    auto *u = new QVBoxLayout(update);
    m_update->setWordWrap(true);
    m_update->setObjectName(QStringLiteral("update"));
    m_undo->setObjectName(QStringLiteral("undo"));
    m_undo->setEnabled(false);
    auto *ur = new QHBoxLayout;
    ur->addWidget(m_update, 1);
    ur->addWidget(m_undo, 0, Qt::AlignTop);
    u->addLayout(ur);
    layout->addWidget(update);

    auto *registry = new QGroupBox(tr("Windows' registry files (\"the registry file is missing or contains errors\")"));
    auto *r = new QVBoxLayout(registry);
    m_hives->setObjectName(QStringLiteral("hives"));
    m_hives->setHeaderLabels({tr("File"), tr("How it is")});
    m_hives->setRootIsDecorated(false);
    m_hives->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_hives->setMaximumHeight(150);
    r->addWidget(m_hives);
    r->addWidget(new QLabel(tr("Earlier copies of the one picked:")));
    m_copies->setObjectName(QStringLiteral("copies"));
    m_copies->setMaximumHeight(130);
    r->addWidget(m_copies);
    m_copiesNote->setWordWrap(true);
    m_copiesNote->setObjectName(QStringLiteral("copiesNote"));
    r->addWidget(m_copiesNote);
    m_putBack->setObjectName(QStringLiteral("putBack"));
    m_putBack->setEnabled(false);
    auto *rr = new QHBoxLayout;
    rr->addStretch();
    rr->addWidget(m_putBack);
    r->addLayout(rr);
    layout->addWidget(registry, 1);

    m_log->setReadOnly(true);
    m_log->setObjectName(QStringLiteral("log"));
    m_log->setMaximumHeight(110);
    m_log->hide();
    layout->addWidget(m_log);
    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    auto *br = new QHBoxLayout;
    br->addStretch();
    br->addWidget(close);
    layout->addLayout(br);

    connect(m_undo, &QPushButton::clicked, this, &WinStartDialog::undoUpdate);
    connect(m_putBack, &QPushButton::clicked, this, &WinStartDialog::putBack);
    connect(m_hives, &QTreeWidget::currentItemChanged, this, &WinStartDialog::showCopies);
    connect(m_copies, &QListWidget::currentRowChanged, this, [this](int row) { m_putBack->setEnabled(row >= 0); });
    resize(760, 640);
    look();
}

WinStartDialog::~WinStartDialog()
{
    *m_cancel = true;
}

void WinStartDialog::look()
{
    m_undo->setEnabled(false);
    m_putBack->setEnabled(false);
    m_hives->clear();
    m_copies->clear();
    QPointer<WinStartDialog> self(this);
    const QString root = m_root;
    const auto cancel = m_cancel;
    QThreadPool::globalInstance()->start([self, root, cancel] {
        const winstart::StuckUpdate stuck = winstart::findStuckUpdate(root);
        const QVector<winstart::HiveCheck> checks = winstart::checkHives(root, cancel.get());
        const QVector<winstart::EarlierCopy> regBack = winstart::regBackCopies(root);
        QMetaObject::invokeMethod(qApp, [self, stuck, checks, regBack] {
            if (!self)
                return;
            self->m_stuck = stuck;
            self->m_checks = checks;
            // RegBack's first; shadow copies join them once they're read.
            QVector<winstart::EarlierCopy> kept;
            for (const auto &c : std::as_const(self->m_earlier))
                if (c.file.isEmpty())
                    kept << c;
            self->m_earlier = regBack + kept;
            self->showUpdate();
            self->showHives();
            if (!self->m_shadowRead)
                self->readShadowCopies();
            else
                emit self->looked();
        }, Qt::QueuedConnection);
    });
}

void WinStartDialog::showUpdate()
{
    const winstart::StuckUpdate &s = m_stuck;
    if (!s.stuck) {
        m_update->setText(tr("No update is waiting to be finished: this isn't what's stopping it."));
        return;
    }
    QString text = tr("An update is waiting to be finished at the next start");
    if (!s.pendingFile.isEmpty())
        text += tr(": %n step(s) listed since %1", nullptr, s.actions).arg(QLocale().toString(s.pendingWritten, QLocale::ShortFormat));
    text += QStringLiteral(". ");
    if (!s.packages.isEmpty())
        text += tr("Waiting: %1. ").arg(s.packages.mid(0, 3).join(QStringLiteral(", ")) + (s.packages.size() > 3 ? QStringLiteral("…") : QString()));
    text += tr("When Windows keeps trying it and failing (\"Undoing changes\" again and again), undoing it lets Windows start as it "
               "was before the update; Windows Update tries it again later.");
#ifdef Q_OS_WIN
    text += QStringLiteral(" ") + tr("It's done with Microsoft's own DISM.");
#endif
    if (!s.problem.isEmpty())
        text += QStringLiteral(" ") + s.problem;
    m_update->setText(text);
    m_undo->setEnabled(s.problem.isEmpty());
}

void WinStartDialog::showHives()
{
    m_hives->clear();
    for (const winstart::HiveCheck &c : std::as_const(m_checks)) {
        QString how;
        switch (c.state) {
        case winstart::HiveState::Fine:
            how = tr("Fine");
            break;
        default:
            how = c.problem;
        }
        auto *item = new QTreeWidgetItem({c.name, how});
        item->setToolTip(1, how);
        if (c.state == winstart::HiveState::Damaged || c.state == winstart::HiveState::Missing)
            item->setForeground(1, QColor(0xd0, 0x30, 0x40));
        m_hives->addTopLevelItem(item);
    }
    m_hives->resizeColumnToContents(0);
    // The first damaged one, so its copies show.
    for (int i = 0; i < m_hives->topLevelItemCount(); ++i) {
        if (m_checks[i].state == winstart::HiveState::Damaged || m_checks[i].state == winstart::HiveState::Missing) {
            m_hives->setCurrentItem(m_hives->topLevelItem(i));
            return;
        }
    }
    if (m_hives->topLevelItemCount() > 0)
        m_hives->setCurrentItem(m_hives->topLevelItem(0));
}

void WinStartDialog::showCopies()
{
    m_copies->clear();
    m_putBack->setEnabled(false);
    const QTreeWidgetItem *item = m_hives->currentItem();
    if (!item)
        return;
    const QString hive = item->text(0);
    for (int i = 0; i < m_earlier.size(); ++i) {
        const winstart::EarlierCopy &c = m_earlier[i];
        if (c.hive != hive)
            continue;
        auto *row = new QListWidgetItem(c.file.isEmpty() ? c.from : tr("%1, from %2").arg(c.from, QLocale().toString(c.when, QLocale::ShortFormat)), m_copies);
        row->setData(Qt::UserRole, i);
    }
    QStringList note;
    if (m_copies->count() == 0)
        note << tr("No earlier copies of %1 were found. Windows stopped keeping them in RegBack in 2018, and System Restore was "
                   "off or kept no copies.").arg(hive);
    if (!m_shadowRead)
        note << tr("Still looking in System Restore's copies…");
    else if (!m_shadowProblem.isEmpty())
        note << m_shadowProblem;
    note << tr("Putting one back undoes what's changed since then (programs installed, settings). Only do it for one that's damaged.");
    m_copiesNote->setText(note.join(QLatin1Char(' ')));
}

void WinStartDialog::say(const QStringList &lines)
{
    m_log->show();
    for (const QString &l : lines) {
        m_log->appendPlainText(l);
        qCInfo(lcOps).noquote() << "Windows Won't Start:" << l;
    }
}

void WinStartDialog::undoUpdate()
{
    if (QMessageBox::question(this, windowTitle(),
                              tr("Undo the update Windows is stuck on? What it was going to change is set aside, and Windows starts as "
                                 "it was before it."))
        != QMessageBox::Yes)
        return;
#ifdef Q_OS_WIN
    // Microsoft's own way first.
    {
        QProcess dism;
        const QString program = QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"))).filePath(QStringLiteral("System32/dism.exe"));
        QApplication::setOverrideCursor(Qt::WaitCursor);
        dism.start(program, winstart::dismArguments(m_root));
        const bool finished = dism.waitForFinished(30 * 60 * 1000);
        QApplication::restoreOverrideCursor();
        const QString out = QString::fromLocal8Bit(dism.readAllStandardOutput()).trimmed();
        say({tr("DISM: %1").arg(out.section(QLatin1Char('\n'), -3).simplified())});
        if (finished && dism.exitCode() == 0) {
            say({tr("Done with DISM. Start that Windows again.")});
            look();
            return;
        }
        say({tr("DISM couldn't do it; doing it DiskAnvil's way.")});
    }
#endif
    QStringList done;
    QString error;
    const bool ok = winstart::undoStuckUpdate(m_root, &done, &error);
    say(done);
    if (!ok) {
        say({error});
        QMessageBox::warning(this, windowTitle(), error);
    } else {
        say({tr("Done. Start that Windows again: it starts without the update, and tries it again later. The first time, Windows "
                "may restart by itself once or twice while it tidies up what was waiting; then it starts as usual.")});
    }
    look();
}

void WinStartDialog::putBack()
{
    const QListWidgetItem *row = m_copies->currentItem();
    const QTreeWidgetItem *hiveItem = m_hives->currentItem();
    if (!row || !hiveItem)
        return;
    const winstart::EarlierCopy copy = m_earlier.value(row->data(Qt::UserRole).toInt());
    const QString hive = hiveItem->text(0);
    if (QMessageBox::question(this, windowTitle(),
                              tr("Put %1 back from %2? The one there now is kept next to it, renamed, with its logs.").arg(hive, row->text()))
        != QMessageBox::Yes)
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString why;
    const QByteArray bytes = copy.read ? copy.read(&why) : QByteArray();
    QStringList done;
    QString error;
    const bool ok = !bytes.isEmpty() && winstart::putBack(m_root, hive, bytes, &done, &error);
    QApplication::restoreOverrideCursor();
    if (bytes.isEmpty())
        error = tr("That copy couldn't be read: %1").arg(why);
    say(done);
    if (!ok) {
        say({error});
        QMessageBox::warning(this, windowTitle(), error);
    } else {
        say({tr("Done. Start that Windows again.")});
    }
    look();
}

void WinStartDialog::readShadowCopies()
{
    m_shadowRead = true; // once
    const Volume *volume = nullptr;
    if (m_udisks) {
        const QString root = QDir::cleanPath(m_root);
        for (const Disk &d : m_udisks->disks())
            for (const Volume &v : d.volumes)
                for (const QString &m : v.mounts())
                    if (QDir::cleanPath(m) == root && v.effectiveFsType() == QLatin1String("ntfs"))
                        volume = &v;
    }
    if (!volume) {
        m_shadowProblem.clear();
        showCopies();
        emit looked();
        return;
    }
    m_shadowRead = false;
    QHash<QString, quint64> records;
    const QString cfg = winstart::find(m_root, QStringLiteral("Windows/System32/config"));
    for (const QString &name : winstart::kHives)
        records.insert(name, cfg.isEmpty() ? 0 : winstart::ntfsRecord(winstart::find(cfg, name)));
    const QString device = volume->objectPath;
    openBlockThen(m_udisks, this, device, UDisks::OpenMode::Benchmark, [this, records](int fd) {
        if (fd < 0) {
            m_shadowRead = true;
            m_shadowProblem = tr("System Restore's copies couldn't be looked at: the drive couldn't be opened.");
            showCopies();
            emit looked();
            return;
        }
        const auto source = lost::Source::fromFd(fd, true);
        QPointer<WinStartDialog> self(this);
        QThreadPool::globalInstance()->start([self, source, records] {
            QString problem;
            const QVector<winstart::EarlierCopy> copies = winstart::shadowCopies(source, records, &problem);
            QMetaObject::invokeMethod(qApp, [self, copies, problem] {
                if (!self)
                    return;
                self->m_shadowRead = true;
                self->m_shadowProblem = problem;
                self->m_earlier += copies;
                self->showCopies();
                emit self->looked();
            }, Qt::QueuedConnection);
        });
    });
}
