// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "opaldialog.h"

#include "applog.h"
#include "dialogs.h"
#include "format.h"
#include "hostprocess.h"
#include "jobui.h"
#include "rescuestick.h"
#include "udisks.h"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QVBoxLayout>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

std::function<QString(const QStringList &, const QByteArray &, bool *)> OpalDialog::runForTest;
bool OpalDialog::allowLoopDevicesForTest = false;

OpalDialog::OpalDialog(UDisks *udisks, const QString &preferredDisk, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_targets(new QComboBox)
    , m_ask(new QPushButton(tr("Ask the Drive")))
    , m_state(new QLabel)
    , m_resetBox(new QGroupBox(tr("Reset it with the PSID on its label (erases everything)")))
    , m_psid(new QLineEdit)
    , m_warning(new QLabel)
    , m_confirm(new QLineEdit)
    , m_reset(new QPushButton(tr("Reset to Factory and Erase")))
{
    setWindowTitle(tr("Self-Encrypting Drive"));
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("Many SSDs (and some hard drives) encrypt everything in the drive itself, and can be locked with a "
                                       "password. A second-hand one still locked with a password nobody knows shows up empty or tiny, "
                                       "or can't be partitioned. Ask the drive what it is and how it's set; a locked one can be reset "
                                       "with the 32-character PSID printed on its label, which erases it for good.")));
    auto *row = new QHBoxLayout;
    m_targets->setObjectName(QStringLiteral("targets"));
    m_ask->setObjectName(QStringLiteral("ask"));
    row->addWidget(new QLabel(tr("Drive:")));
    row->addWidget(m_targets, 1);
    row->addWidget(m_ask);
    layout->addLayout(row);
    m_state->setObjectName(QStringLiteral("state"));
    m_state->setWordWrap(true);
    m_state->setTextFormat(Qt::RichText);
    m_state->setMinimumHeight(80);
    layout->addWidget(m_state);

    auto *form = new QFormLayout(m_resetBox);
    m_psid->setObjectName(QStringLiteral("psid"));
    m_psid->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_psid->setPlaceholderText(tr("32 letters and digits, from the drive's label"));
    form->addRow(tr("PSID:"), m_psid);
    m_warning->setWordWrap(true);
    m_warning->setTextFormat(Qt::RichText);
    form->addRow(m_warning);
    m_confirm->setObjectName(QStringLiteral("confirm"));
    form->addRow(m_confirm);
    m_reset->setObjectName(QStringLiteral("reset"));
    auto *resetRow = new QHBoxLayout;
    resetRow->addStretch();
    resetRow->addWidget(m_reset);
    form->addRow(resetRow);
    m_resetBox->hide();
    layout->addWidget(m_resetBox);
    layout->addStretch();
    auto *close = new QPushButton(tr("Close"));
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_ask, &QPushButton::clicked, this, &OpalDialog::ask);
    connect(m_reset, &QPushButton::clicked, this, &OpalDialog::revert);
    connect(m_targets, &QComboBox::currentIndexChanged, this, [this] {
        m_found = opal::Discovery();
        m_askedFor.clear();
        m_state->clear();
        updateState();
    });
    connect(m_psid, &QLineEdit::textChanged, this, &OpalDialog::updateState);
    connect(m_confirm, &QLineEdit::textChanged, this, &OpalDialog::updateState);
    fillTargets(preferredDisk);
    updateState();
    resize(640, 480);
}

void OpalDialog::fillTargets(const QString &preferred)
{
    const QSignalBlocker block(m_targets);
    m_targets->clear();
    const QVector<Disk> &disks = m_udisks->disks();
    for (int i = 0; i < disks.size(); ++i) {
        const Disk &d = disks[i];
        if (d.isRaid || (d.isLoop ? !allowLoopDevicesForTest : d.drivePath.isEmpty()))
            continue;
        m_targets->addItem(tr("Disk %1: %2").arg(i).arg(diskTitle(d)), d.blockPath);
        if (d.blockPath == preferred)
            m_targets->setCurrentIndex(m_targets->count() - 1);
    }
    if (m_targets->count() == 0)
        m_targets->addItem(tr("No drives"));
}

const Disk *OpalDialog::target() const
{
    return m_udisks->diskByPath(m_targets->currentData().toString());
}

QString OpalDialog::run(const QStringList &args, const QByteArray &input, bool *ok)
{
    if (runForTest)
        return runForTest(args, input, ok);
    QStringList command = QStringList{QCoreApplication::applicationFilePath()} + args;
#ifndef Q_OS_WIN
    if (::geteuid() != 0)
        command = rescue::runningInRescue() ? QStringList{QStringLiteral("sudo"), QStringLiteral("-n")} + command
                                            : host::command(QStringList{QStringLiteral("pkexec")} + command);
#endif
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QProcess p;
    p.start(command.first(), command.mid(1));
    p.write(input);
    p.closeWriteChannel();
    const bool finished = p.waitForFinished(120000);
    QApplication::restoreOverrideCursor();
    *ok = finished && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
    if (out.isEmpty())
        out = QStringLiteral("error=") + (finished ? QString::fromLocal8Bit(p.readAllStandardError()).trimmed() : tr("it took too long"));
    return out;
}

void OpalDialog::ask()
{
    const Disk *d = target();
    if (!d)
        return;
    bool ok = false;
    const QString out = run({QString::fromLatin1(opal::kOption), d->device, QStringLiteral("status")}, {}, &ok);
    m_found = opal::discoveryFromLines(out);
    m_askedFor = d->blockPath;
    QString error;
    for (const QString &line : out.split(QLatin1Char('\n')))
        if (line.startsWith(QLatin1String("error=")))
            error = line.mid(6);
    QStringList text;
    if (!error.isEmpty() && !m_found.answered)
        text << tr("The drive couldn't be asked: %1").arg(error).toHtmlEscaped();
    else
        for (const QString &line : opal::describe(m_found))
            text << line.toHtmlEscaped();
    m_state->setText(text.join(QStringLiteral("<br>")));
    qCInfo(lcOps).noquote() << "Self-encrypting drive" << d->device << out.simplified();
    updateState();
    emit asked();
}

void OpalDialog::updateState()
{
    const Disk *d = target();
    const bool askedThis = d && m_askedFor == d->blockPath;
    m_resetBox->setVisible(askedThis && m_found.answered && !m_found.ssc.isEmpty());
    m_ask->setEnabled(d != nullptr);
    if (!d)
        return;
    const QString name = shortDevice(d->device);
    const QString psid = opal::cleanPsid(m_psid->text());
    QString warning = QStringLiteral("<b>%1</b> ").arg(tr("Everything on %1 is erased for good, and its locking is switched off.").arg(name).toHtmlEscaped());
    if (!m_found.psidPossible)
        warning += tr("This drive doesn't have a PSID reset: its maker's own tool is the way.").toHtmlEscaped();
    else if (d->isSystem)
        warning += tr("Not the drive the system runs from.").toHtmlEscaped();
    else
        warning += tr("Type <b>%1</b> to confirm:").arg(name.toHtmlEscaped());
    m_warning->setText(warning);
    m_confirm->setPlaceholderText(name);
    m_confirm->setVisible(m_found.psidPossible && !d->isSystem);
    m_psid->setEnabled(m_found.psidPossible && !d->isSystem);
    m_reset->setEnabled(m_found.psidPossible && !d->isSystem && psid.size() == 32 && m_confirm->text().trimmed() == name);
}

void OpalDialog::revert()
{
    const Disk *d = target();
    if (!d || d->isSystem)
        return;
    if (QMessageBox::warning(this, windowTitle(),
                             tr("Reset %1 to how it left the factory? Everything on it is erased for good, and can't be brought back by "
                                "anyone.").arg(diskTitle(*d)),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes)
        return;
    qCInfo(lcOps).noquote() << "Self-encrypting drive: PSID reset of" << d->device << d->model << d->serial;
    bool ok = false;
    const QString out = run({QString::fromLatin1(opal::kOption), d->device, QStringLiteral("revert")}, opal::cleanPsid(m_psid->text()).toLatin1() + '\n', &ok);
    QString error;
    for (const QString &line : out.split(QLatin1Char('\n')))
        if (line.startsWith(QLatin1String("error=")))
            error = line.mid(6);
    m_psid->clear();
    m_confirm->clear();
    if (ok) {
        qCInfo(lcOps).noquote() << "Self-encrypting drive: reset done";
        QMessageBox::information(this, windowTitle(), tr("Done: the drive is as it left the factory, empty, and its locking is off. Switch it off and "
                                                         "on again (or unplug it), then make a partition table on it."));
        m_udisks->refresh();
        ask();
    } else {
        qCInfo(lcOps).noquote() << "Self-encrypting drive: reset failed:" << error;
        QMessageBox::warning(this, windowTitle(), tr("The drive wasn't reset: %1").arg(error.isEmpty() ? tr("it gave no reason") : error));
    }
    emit reset(ok);
}
