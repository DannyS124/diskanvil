// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "linuxbootdialog.h"

#include "applog.h"
#include "dialogs.h"
#include "efivars.h"
#include "format.h"
#include "rescuestick.h"
#include "udisks.h"

#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QVBoxLayout>

#include <memory>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

namespace {

const QStringList kLinuxFs = {QStringLiteral("ext4"), QStringLiteral("ext3"), QStringLiteral("btrfs"), QStringLiteral("xfs"), QStringLiteral("f2fs")};

const Volume *byPath(UDisks *udisks, const QString &path)
{
    for (const Disk &d : udisks->disks()) {
        for (const Volume &v : d.volumes) {
            if (v.objectPath == path)
                return &v;
        }
    }
    return nullptr;
}

bool canRun()
{
#ifdef Q_OS_WIN
    return false;
#else
    return rescue::runningInRescue() || ::geteuid() == 0;
#endif
}

} // namespace

LinuxBootDialog::LinuxBootDialog(UDisks *udisks, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_list(new QListWidget)
    , m_plan(new QLabel)
    , m_output(new QPlainTextEdit)
    , m_repair(new QPushButton(tr("Repair")))
{
    setWindowTitle(tr("Repair Linux Boot"));
    m_list->setObjectName(QStringLiteral("installs"));
    m_plan->setWordWrap(true);
    m_plan->setObjectName(QStringLiteral("plan"));
    m_output->setReadOnly(true);
    m_output->setObjectName(QStringLiteral("output"));
    m_output->hide();
    m_repair->setEnabled(false);
    m_repair->setObjectName(QStringLiteral("repair"));
    connect(m_list, &QListWidget::currentRowChanged, this, &LinuxBootDialog::showInstall);
    connect(m_repair, &QPushButton::clicked, this, &LinuxBootDialog::repair);
    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &LinuxBootDialog::reject);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(m_repair);
    buttons->addWidget(close);
    auto *layout = new QVBoxLayout(this);
    QString intro = tr("For a Linux that doesn't start any more because its boot loader is gone or broken: after Windows "
                       "overwrote it, a firmware reset dropped its entry, or an update was cut short. Its own tools put it back.");
    if (!canRun())
        intro += QStringLiteral("<br><br>") + redText(tr("A Linux's boot is repaired from outside it: start this PC from DiskAnvil Live "
                                                          "(this window can still look at what's here)."));
    layout->addWidget(wrappingLabel(intro));
    layout->addWidget(m_list, 1);
    layout->addWidget(m_plan);
    layout->addWidget(m_output, 2);
    layout->addLayout(buttons);
    resize(720, 560);

    // Every Linux file system is looked at, mounted read-only when it isn't yet.
    for (const Disk &d : m_udisks->disks()) {
        for (const Volume &v : d.volumes) {
            if (kLinuxFs.contains(v.effectiveFsType()) && v.canMount())
                m_toLook << v.objectPath;
        }
    }
    m_plan->setText(tr("Looking for Linux installs…"));
    lookNext();
}

LinuxBootDialog::~LinuxBootDialog()
{
    if (m_run)
        m_run->waitForFinished(-1); // it unmounts what it mounted on its way out
    // What was mounted only to look goes back the way it was.
    for (const QString &path : std::as_const(m_mountedHere)) {
        if (const Volume *v = byPath(m_udisks, path); v && !v->mounts().isEmpty())
            m_udisks->unmount(*v);
    }
}

void LinuxBootDialog::reject()
{
    if (m_run)
        return; // a repair isn't left half done
    QDialog::reject();
}

void LinuxBootDialog::lookNext()
{
    while (!m_toLook.isEmpty()) {
        const Volume *v = byPath(m_udisks, m_toLook.takeFirst());
        if (!v)
            continue;
        if (!v->mounts().isEmpty()) {
            lookAt(*v);
            continue;
        }
        // Mounted read-only to look, then on to the next.
        const QString path = v->objectPath;
        auto mounted = std::make_shared<QMetaObject::Connection>();
        auto failed = std::make_shared<QMetaObject::Connection>();
        *mounted = connect(m_udisks, &UDisks::changed, this, [this, path, mounted, failed] {
            const Volume *now = byPath(m_udisks, path);
            if (!now || now->mounts().isEmpty())
                return;
            disconnect(*mounted);
            disconnect(*failed);
            m_mountedHere << path;
            lookAt(*now);
            lookNext();
        });
        *failed = connect(m_udisks, &UDisks::operationFinished, this, [this, mounted, failed](bool ok, const QString &) {
            if (ok)
                return;
            disconnect(*mounted);
            disconnect(*failed);
            lookNext();
        });
        m_udisks->mount(*v, true);
        return;
    }
    m_plan->setText(m_installs.isEmpty() ? tr("No Linux install was found on these drives.") : tr("Pick the Linux to repair."));
}

void LinuxBootDialog::lookAt(const Volume &v)
{
    const QString top = v.mounts().first();
    QStringList roots;
    if (QFileInfo::exists(top + QStringLiteral("/etc/fstab")))
        roots << QString();
    if (v.effectiveFsType() == QLatin1String("btrfs"))
        roots << linuxboot::rootSubvolumes(top);
    for (const QString &sub : std::as_const(roots)) {
        const QString root = sub.isEmpty() ? top : top + QLatin1Char('/') + sub;
        if (root == QLatin1String("/"))
            continue; // the running system itself
        const linuxboot::Install in = linuxboot::inspect(root, v.encrypted ? v.cleartextDevice : v.device, sub, m_udisks->disks());
        if (in.name.isEmpty())
            continue;
        m_installs << in;
        new QListWidgetItem(tr("%1, on %2").arg(in.name, volumeTitle(v)) + (sub.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(sub)), m_list);
    }
}

void LinuxBootDialog::showInstall(int row)
{
    if (row < 0 || row >= m_installs.size())
        return;
    const linuxboot::Install &in = m_installs.at(row);
    const bool uefi = efi::available();
    QString text = QStringLiteral("<b>%1</b><br>").arg(in.name.toHtmlEscaped());
    if (!in.problems.isEmpty()) {
        text += redText(tr("It can't be repaired here: %1.").arg(in.problems.join(QStringLiteral("; "))));
    } else {
        QStringList steps;
        for (const QString &step : linuxboot::plan(in, uefi))
            steps << QStringLiteral("• ") + step.toHtmlEscaped();
        text += steps.join(QStringLiteral("<br>"));
        if (uefi && in.espDevice.isEmpty())
            text += QStringLiteral("<br>") + redText(tr("This PC started with UEFI, but this Linux has no EFI partition in its fstab: it was "
                                                        "installed the old BIOS way. Start DiskAnvil Live the same way (BIOS) to repair it."));
    }
    m_plan->setText(text);
    m_repair->setEnabled(canRun() && in.problems.isEmpty() && !(uefi && in.espDevice.isEmpty()));
}

void LinuxBootDialog::repair()
{
    const int row = m_list->currentRow();
    if (row < 0 || m_run)
        return;
    const linuxboot::Install in = m_installs.at(row);
    if (QMessageBox::question(this, windowTitle(),
                              tr("Repair %1's boot? Its own tools run inside it; nothing but its boot loader and its menu changes.").arg(in.name),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes)
        return;
    // What was mounted read-only to look is unmounted: the repair mounts it itself, writable.
    QVector<Volume> mounted;
    for (const Disk &d : m_udisks->disks()) {
        for (const Volume &v : d.volumes) {
            if (!v.mounts().isEmpty() && QStringList{in.rootDevice, in.bootDevice, in.espDevice}.contains(v.encrypted ? v.cleartextDevice : v.device))
                mounted << v;
        }
    }
    m_repair->setEnabled(false);
    m_list->setEnabled(false);
    m_output->show();
    m_output->setPlainText(tr("Unmounting what was looked at…") + QLatin1Char('\n'));
    const QString script = linuxboot::repairScript(in, efi::available());
    m_udisks->unmountThen(mounted, tr("It couldn't be unmounted"), [this, script, in] {
        m_run = new QProcess(this);
        m_run->setProcessChannelMode(QProcess::MergedChannels);
        connect(m_run, &QProcess::readyRead, this, [this] { m_output->appendPlainText(QString::fromLocal8Bit(m_run->readAll()).trimmed()); });
        connect(m_run, &QProcess::finished, this, [this, in](int code, QProcess::ExitStatus status) {
            const QString all = m_output->toPlainText();
            const bool ok = status == QProcess::NormalExit && code == 0 && all.contains(QLatin1String("DISKANVIL-REPAIR-DONE"));
            qCInfo(lcOps).noquote() << "Repair Linux boot of" << in.name << (ok ? "done" : "failed") << all.right(2000);
            m_run->deleteLater();
            m_run = nullptr;
            m_list->setEnabled(true);
            m_udisks->refresh();
            m_output->appendPlainText(QString());
            m_output->appendPlainText(ok ? tr("Done. Take DiskAnvil Live out and restart: %1 should start again.").arg(in.name)
                                         : tr("It didn't finish: what its tools said is above. Nothing else was changed."));
        });
        const QStringList command = rescue::runningInRescue() ? QStringList{QStringLiteral("sudo"), QStringLiteral("-n"), QStringLiteral("bash"), QStringLiteral("-c"), script}
                                                              : QStringList{QStringLiteral("bash"), QStringLiteral("-c"), script};
        m_run->start(command.first(), command.mid(1));
    }, false, [this] {
        m_output->appendPlainText(tr("It couldn't be unmounted: is something using it?"));
        m_list->setEnabled(true);
    });
}
