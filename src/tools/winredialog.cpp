// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "winredialog.h"

#include "applog.h"
#include "dialogs.h"
#include "format.h"
#include "udisks.h"

#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include <memory>

WinReDialog::WinReDialog(UDisks *udisks, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_state(new QLabel)
    , m_output(new QPlainTextEdit)
    , m_fix(new QPushButton(tr("Make Room")))
{
    setWindowTitle(tr("Make Room for Windows Recovery"));
    m_state->setWordWrap(true);
    m_state->setObjectName(QStringLiteral("state"));
    m_output->setReadOnly(true);
    m_output->hide();
    m_fix->setEnabled(false);
    m_fix->setObjectName(QStringLiteral("makeRoom"));
    connect(m_fix, &QPushButton::clicked, this, &WinReDialog::makeRoom);
    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, [this] {
        if (!m_running)
            accept();
    });
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(m_fix);
    buttons->addWidget(close);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("Windows keeps a small recovery system (WinRE) in a partition of its own. When that partition is "
                                       "too small, Windows' updates to it fail with error 0x80070643, over and over. Microsoft's fix "
                                       "(KB5028997) gives it 250 MB from Windows' own partition: DiskAnvil does those steps for you.")));
    layout->addWidget(m_state);
    layout->addWidget(m_output, 1);
    layout->addLayout(buttons);
    resize(640, 420);
    look();
}

void WinReDialog::look()
{
    QProcess reagentc;
    reagentc.start(QStringLiteral("reagentc"), {QStringLiteral("/info")});
    reagentc.waitForFinished(60000);
    const QString info = QString::fromLocal8Bit(reagentc.readAll());
    m_udisks->refresh();
    QString system = qEnvironmentVariable("SystemDrive", QStringLiteral("C:")) + QLatin1Char('/');
    m_plan = winre::plan(winre::parse(info), m_udisks->disks(), system);
    QString text;
    if (m_plan.reSize > 0)
        text = tr("Its recovery partition (disk %1, partition %2): %3, with %4 free.")
                   .arg(m_plan.diskNumber)
                   .arg(m_plan.rePartition)
                   .arg(formatSize(m_plan.reSize), formatSize(m_plan.reFree));
    if (!m_plan.problem.isEmpty())
        text += QLatin1Char(' ') + redText(tr("It can't be done this way: %1.").arg(m_plan.problem).toHtmlEscaped());
    else if (!m_plan.needed)
        text += QLatin1Char(' ') + tr("That's room enough: nothing to do.");
    else
        text += QLatin1Char(' ') + tr("Its updates need 250 MB free there: this is what error 0x80070643 is about.");
    m_state->setText(text);
    m_fix->setEnabled(m_plan.needed && m_plan.problem.isEmpty() && !m_running);
}

void WinReDialog::runSteps(const QVector<QStringList> &steps, const std::function<void(const QVector<bool> &)> &then, QVector<bool> done)
{
    if (done.size() >= steps.size())
        return then(done);
    const QStringList step = steps.at(done.size());
    m_output->appendPlainText(QStringLiteral("> ") + step.join(QLatin1Char(' ')));
    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::readyRead, this, [this, process] { m_output->appendPlainText(QString::fromLocal8Bit(process->readAll()).trimmed()); });
    connect(process, &QProcess::finished, this, [this, process, steps, then, done](int code, QProcess::ExitStatus status) mutable {
        process->deleteLater();
        done << (status == QProcess::NormalExit && code == 0);
        runSteps(steps, then, done);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, steps, then, done](QProcess::ProcessError error) mutable {
        if (error != QProcess::FailedToStart)
            return;
        process->deleteLater();
        done << false;
        runSteps(steps, then, done);
    });
    process->start(step.first(), step.mid(1));
}

void WinReDialog::makeRoom()
{
    if (QMessageBox::question(this, windowTitle(),
                              tr("Make room for Windows' recovery? Windows' partition gets 250 MB smaller, and its recovery partition is "
                                 "made again that much bigger. Your files aren't touched, but a backup first is wise. If Windows' drive "
                                 "uses BitLocker, have its recovery key at hand."),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes)
        return;
    auto dir = std::make_shared<QTemporaryDir>();
    const QString script = dir->filePath(QStringLiteral("winre.txt"));
    QFile f(script);
    if (!f.open(QIODevice::WriteOnly) || f.write(winre::diskpartScript(m_plan).toLatin1()) <= 0) {
        m_state->setText(redText(tr("The steps couldn't be written down for diskpart.")));
        return;
    }
    f.close();
    m_running = true;
    m_fix->setEnabled(false);
    m_output->show();
    m_output->clear();
    qCInfo(lcOps).noquote() << "WinRE: making room on disk" << m_plan.diskNumber << "partitions" << m_plan.osPartition << m_plan.rePartition;
    // Recovery is switched off while its partition is made again, and back on whatever happened.
    runSteps({{QStringLiteral("reagentc"), QStringLiteral("/disable")}, {QStringLiteral("diskpart"), QStringLiteral("/s"), QDir::toNativeSeparators(script)},
              {QStringLiteral("reagentc"), QStringLiteral("/enable")}, {QStringLiteral("reagentc"), QStringLiteral("/info")}},
             [this, dir](const QVector<bool> &done) {
                 m_running = false;
                 const bool ok = done.value(1) && done.value(2);
                 qCInfo(lcOps).noquote() << "WinRE: diskpart" << done.value(1) << "reagentc /enable" << done.value(2);
                 look();
                 m_output->appendPlainText(QString());
                 m_output->appendPlainText(ok ? tr("Done: the recovery partition is bigger, and recovery is on again. Run Windows Update again.")
                                              : tr("It didn't all work: what the steps said is above. Recovery was switched back on either way."));
             });
}
