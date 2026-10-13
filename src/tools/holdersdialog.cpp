// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "holdersdialog.h"

#include "hostprocess.h"
#include "rescuestick.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#ifndef Q_OS_WIN
#include <csignal>
#include <unistd.h>
#endif

HoldersDialog::HoldersDialog(UDisks *udisks, const Volume &volume, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_volume(volume)
{
    setWindowTitle(tr("Who's Using It?"));
    resize(760, 460);
    auto *layout = new QVBoxLayout(this);
    m_summary = new QLabel;
    m_summary->setWordWrap(true);
    m_summary->setObjectName(QStringLiteral("summary"));
    layout->addWidget(m_summary);
    m_list = new QTreeWidget;
    m_list->setObjectName(QStringLiteral("holders"));
    m_list->setHeaderLabels({tr("Program"), tr("Number"), tr("User"), tr("What it has open there")});
    m_list->setRootIsDecorated(false);
    m_list->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    layout->addWidget(m_list, 1);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    m_close = box->addButton(tr("Close It"), QDialogButtonBox::ActionRole);
    m_close->setObjectName(QStringLiteral("closeIt"));
    m_close->setToolTip(tr("Asks the program to close, the way logging out does: it can save first"));
    m_kill = box->addButton(tr("Stop It"), QDialogButtonBox::ActionRole);
    m_kill->setObjectName(QStringLiteral("stopIt"));
    m_kill->setToolTip(tr("Stops the program straight away: anything it hasn't saved is lost"));
    m_admin = box->addButton(tr("Look as Admin"), QDialogButtonBox::ActionRole);
    m_admin->setToolTip(tr("Other users' and the system's programs can only be seen with the admin password"));
    m_again = box->addButton(tr("Try Again"), QDialogButtonBox::ActionRole);
    m_again->setObjectName(QStringLiteral("tryAgain"));
    m_again->setToolTip(tr("Unmounts it now"));
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_close, &QPushButton::clicked, this, [this] { stop(false); });
    connect(m_kill, &QPushButton::clicked, this, [this] { stop(true); });
    connect(m_admin, &QPushButton::clicked, this, [this] { look(true); });
    connect(m_again, &QPushButton::clicked, this, [this] {
        m_udisks->unmount(m_volume);
        accept();
    });
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, [this] {
        const bool any = !m_list->selectedItems().isEmpty();
        m_close->setEnabled(any);
        m_kill->setEnabled(any);
    });
    layout->addWidget(box);

    runAsRoot = [](const QStringList &command, QString *said) {
        QStringList c = command;
#ifndef Q_OS_WIN
        if (rescue::runningInRescue())
            c = QStringList{QStringLiteral("sudo"), QStringLiteral("-n")} + command;
        else if (::geteuid() != 0)
            c = host::command(QStringList{QStringLiteral("pkexec")} + command);
#endif
        QApplication::setOverrideCursor(Qt::WaitCursor);
        QProcess run;
        run.start(c.first(), c.mid(1));
        const bool ok = run.waitForFinished(60000) && run.exitStatus() == QProcess::NormalExit && run.exitCode() == 0;
        QApplication::restoreOverrideCursor();
        *said = QString::fromLocal8Bit(run.readAllStandardOutput());
        if (!ok && said->isEmpty())
            *said = QString::fromLocal8Bit(run.readAllStandardError()) + run.errorString();
        return ok;
    };
    sendSignal = [](qint64 pid, int signal) {
#ifndef Q_OS_WIN
        return ::kill(pid_t(pid), signal) == 0;
#else
        Q_UNUSED(pid)
        Q_UNUSED(signal)
        return false;
#endif
    };
    look();
}

void HoldersDialog::look(bool asAdmin)
{
    const QStringList mounts = m_volume.mounts();
    QStringList devices{m_volume.device};
    if (m_volume.encrypted && !m_volume.cleartextDevice.isEmpty())
        devices << m_volume.cleartextDevice;
    if (asAdmin) {
        QString said;
        QStringList c{QCoreApplication::applicationFilePath(), QString::fromLatin1(holders::kOption)};
        c << mounts.value(0) << devices;
        if (!runAsRoot(c, &said)) {
            QMessageBox::warning(this, windowTitle(), tr("Couldn't look as admin.\n\n%1").arg(said.trimmed().right(800)));
            return;
        }
        m_report = holders::fromLines(said);
    } else {
        m_report = holders::find(mounts, devices);
    }
    showReport();
}

void HoldersDialog::showReport()
{
    m_list->clear();
    for (const holders::Holder &h : m_report.programs) {
        auto *item = new QTreeWidgetItem({h.program, QString::number(h.pid), h.user, h.what.join(QStringLiteral("; "))});
        item->setToolTip(0, h.command);
        item->setToolTip(3, h.what.join(QLatin1Char('\n')));
        item->setData(0, Qt::UserRole, h.pid);
        m_list->addTopLevelItem(item);
    }
    for (int c = 0; c < 3; ++c)
        m_list->resizeColumnToContents(c);
    const QString name = m_volume.label.isEmpty() ? m_volume.device : m_volume.label;
    QStringList text;
    if (m_report.programs.isEmpty() && m_report.others.isEmpty())
        text << tr("No program has anything open on %1 now. Try Again should unmount it.").arg(name);
    else if (!m_report.programs.isEmpty())
        text << tr("These programs have something open on %1. Close them (save your work first), or pick one and Close It.").arg(name);
    for (const QString &o : m_report.others)
        text << QStringLiteral("• ") + o;
    if (m_report.unreadable > 0)
        text << tr("Programs run by other users or by the system (%1) can only be looked at with the admin password: Look as Admin.").arg(m_report.unreadable);
    m_summary->setText(text.join(QLatin1Char('\n')));
    m_admin->setVisible(m_report.unreadable > 0);
    m_close->setEnabled(false);
    m_kill->setEnabled(false);
    if (m_list->topLevelItemCount() > 0)
        m_list->setCurrentItem(m_list->topLevelItem(0));
}

void HoldersDialog::stop(bool force)
{
    const QList<QTreeWidgetItem *> picked = m_list->selectedItems();
    if (picked.isEmpty())
        return;
    const qint64 pid = picked.first()->data(0, Qt::UserRole).toLongLong();
    const QString program = picked.first()->text(0);
    if (force && QMessageBox::question(this, windowTitle(), tr("Stop %1 straight away? Anything it hasn't saved is lost.").arg(program)) != QMessageBox::Yes)
        return;
#ifndef Q_OS_WIN
    const int signal = force ? SIGKILL : SIGTERM;
#else
    const int signal = force ? 9 : 15;
#endif
    // Its own user's program directly; anyone else's as admin.
    bool ok = sendSignal(pid, signal);
    if (!ok) {
        QString said;
        ok = runAsRoot({QStringLiteral("kill"), force ? QStringLiteral("-KILL") : QStringLiteral("-TERM"), QString::number(pid)}, &said);
        if (!ok) {
            QMessageBox::warning(this, windowTitle(), tr("Couldn't stop %1.\n\n%2").arg(program, said.trimmed().right(800)));
            return;
        }
    }
    // It takes a moment to go.
    QTimer::singleShot(force ? 300 : 1500, this, [this] { look(); });
}
