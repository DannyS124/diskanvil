// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tryrepairdialog.h"

#include "applog.h"
#include "dialogs.h"
#include "rescuestick.h"
#include "tryrepair.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

namespace {

QStringList asRoot(const QString &script)
{
#ifdef Q_OS_WIN
    Q_UNUSED(script)
    return {};
#else
    return rescue::runningInRescue() && ::geteuid() != 0 ? QStringList{QStringLiteral("sudo"), QStringLiteral("-n"), QStringLiteral("bash"), QStringLiteral("-c"), script}
                                                          : QStringList{QStringLiteral("bash"), QStringLiteral("-c"), script};
#endif
}

} // namespace

bool TryRepairDialog::possible(const QString &fsType)
{
#ifdef Q_OS_WIN
    Q_UNUSED(fsType)
    return false;
#else
    return (rescue::runningInRescue() || ::geteuid() == 0) && !tryrepair::repairCommand(fsType, QStringLiteral("x")).isEmpty();
#endif
}

TryRepairDialog::TryRepairDialog(const QString &device, const QString &fsType, const QString &title, QWidget *parent)
    : QDialog(parent)
    , m_device(device)
    , m_fsType(fsType)
    , m_status(new QLabel)
    , m_output(new QPlainTextEdit)
    , m_try(new QPushButton(tr("Try It")))
    , m_look(new QPushButton(tr("Look at the Result")))
    , m_real(new QPushButton(tr("Repair for Real")))
    , m_away(new QPushButton(tr("Throw It Away")))
{
    setWindowTitle(tr("Try the Repair First"));
    m_status->setWordWrap(true);
    m_status->setObjectName(QStringLiteral("status"));
    m_output->setReadOnly(true);
    m_output->setObjectName(QStringLiteral("output"));
    m_try->setObjectName(QStringLiteral("try"));
    m_look->setObjectName(QStringLiteral("look"));
    m_real->setObjectName(QStringLiteral("real"));
    m_look->setEnabled(false);
    m_real->setEnabled(false);
    connect(m_try, &QPushButton::clicked, this, [this] {
        m_try->setEnabled(false);
        m_status->setText(tr("Repairing the stand-in…"));
        // Room for the repair's changes: what it writes, kept in memory, up to 2 GB.
        run(tryrepair::startScript(m_device, m_fsType, 2ull * 1024 * 1024 * 1024), [this](const QString &out) {
            m_started = out.contains(QLatin1String("DISKANVIL-TRY-READY"));
            if (!m_started) {
                m_status->setText(redText(tr("The stand-in couldn't be made: what was said is above. The drive wasn't touched.")));
                return;
            }
            m_status->setText(tr("The repair ran on the stand-in: what it did is above. Look at the result, then repair the real "
                                 "partition the same way, or throw it away. The drive itself hasn't changed."));
            m_look->setEnabled(true);
            m_real->setEnabled(true);
        });
    });
    connect(m_look, &QPushButton::clicked, this, [this] {
        run(tryrepair::lookScript(m_fsType), [this](const QString &out) {
            if (out.contains(QLatin1String("DISKANVIL-TRY-MOUNTED")))
                QDesktopServices::openUrl(QUrl::fromLocalFile(QLatin1String(tryrepair::kMount)));
            else
                m_status->setText(redText(tr("The result couldn't be mounted to look at: the repair may have left it unreadable. "
                                             "Better not to repair the real partition this way.")));
        });
    });
    connect(m_real, &QPushButton::clicked, this, [this] {
        stop();
        accept();
    });
    connect(m_away, &QPushButton::clicked, this, &TryRepairDialog::reject);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_try);
    buttons->addWidget(m_look);
    buttons->addStretch();
    buttons->addWidget(m_real);
    buttons->addWidget(m_away);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("<b>%1</b><br>The repair runs on a stand-in first: it reads from the partition, but what the "
                                       "repair changes is kept in memory. Nothing on the drive changes until you choose to repair it "
                                       "for real.")
                                        .arg(title.toHtmlEscaped())));
    layout->addWidget(m_output, 1);
    layout->addWidget(m_status);
    layout->addLayout(buttons);
    resize(700, 480);
}

TryRepairDialog::~TryRepairDialog()
{
    if (m_process)
        m_process->waitForFinished(-1);
    stop();
}

void TryRepairDialog::reject()
{
    if (m_process)
        return;
    stop();
    QDialog::reject();
}

void TryRepairDialog::run(const QString &script, const std::function<void(const QString &output)> &then)
{
    const QStringList command = asRoot(script);
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    auto all = std::make_shared<QString>();
    connect(m_process, &QProcess::readyRead, this, [this, all] {
        const QString text = QString::fromLocal8Bit(m_process->readAll());
        *all += text;
        m_output->appendPlainText(text.trimmed());
    });
    connect(m_process, &QProcess::finished, this, [this, all, then] {
        *all += QString::fromLocal8Bit(m_process->readAll());
        m_process->deleteLater();
        m_process = nullptr;
        qCInfo(lcOps).noquote() << "Try a repair:" << all->right(1500);
        then(*all);
    });
    m_process->start(command.first(), command.mid(1));
}

void TryRepairDialog::stop()
{
    if (!m_started)
        return;
    m_started = false;
    // Synchronous: the stand-in has to be gone before the real repair, or before the window is.
    QProcess p;
    const QStringList command = asRoot(tryrepair::stopScript());
    p.start(command.first(), command.mid(1));
    p.waitForFinished(60000);
}
