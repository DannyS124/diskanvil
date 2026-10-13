// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "writeprotectdialog.h"

#include "format.h"
#include "hostprocess.h"
#include "jobui.h"
#include "rescuestick.h"
#include "udisks.h"
#include "writeblock.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

#include "blockio.h"

using writeprotect::Finding;
using writeprotect::Fix;
using writeprotect::State;

WriteProtectDialog::WriteProtectDialog(UDisks *udisks, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
{
    setWindowTitle(tr("Write-Protected Drive"));
    resize(640, 560);
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("A stick, memory card or drive that won't let anything be saved to it? Pick it: DiskAnvil looks "
                                "at the drive, the system and its file systems, says what's stopping it, and fixes what a "
                                "program can fix. Nothing on it is changed to look."));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *row = new QHBoxLayout;
    m_drives = new QComboBox;
    m_drives->setObjectName(QStringLiteral("drives"));
    m_look = new QPushButton(tr("Look"));
    m_look->setObjectName(QStringLiteral("look"));
    m_look->setDefault(true);
    row->addWidget(m_drives, 1);
    row->addWidget(m_look);
    layout->addLayout(row);
    m_status = new QLabel;
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_resultsBox = new QWidget;
    m_results = new QVBoxLayout(m_resultsBox);
    m_results->setContentsMargins(0, 0, 0, 0);
    m_results->addStretch(1);
    scroll->setWidget(m_resultsBox);
    layout->addWidget(scroll, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_look, &QPushButton::clicked, this, &WriteProtectDialog::look);
    connect(m_udisks, &UDisks::changed, this, &WriteProtectDialog::fillDrives);
    connect(m_udisks, &UDisks::operationFinished, this, [this](bool ok, const QString &message) {
        if (m_afterOperation) {
            auto then = std::move(m_afterOperation);
            m_afterOperation = nullptr;
            then(ok, message);
        }
    });
    connect(m_udisks, &UDisks::checkFinished, this, [this](const QString &objectPath, bool clean) {
        if (objectPath != m_waitingFor)
            return;
        m_waitingFor.clear();
        m_status->setText(clean ? tr("Repaired. Opening it again…") : tr("The repair didn't finish cleanly. Opening it again anyway…"));
        for (const Disk &d : m_udisks->disks())
            for (const Volume &v : d.volumes)
                if (v.objectPath == objectPath) {
                    m_afterOperation = [this](bool, const QString &message) {
                        m_status->setText(message);
                        look();
                    };
                    m_udisks->mount(v);
                    return;
                }
    });
    fillDrives();

    runAsRoot = [](const QStringList &command, QString *said) {
        // DiskAnvil Live's sudo, or the admin password through pkexec.
        QStringList c = command;
#ifndef Q_OS_WIN
        if (rescue::runningInRescue())
            c = QStringList{QStringLiteral("sudo"), QStringLiteral("-n")} + command;
        else if (::geteuid() != 0)
            c = host::command(QStringList{QStringLiteral("pkexec")} + command);
#endif
        QApplication::setOverrideCursor(Qt::WaitCursor);
        QProcess run;
        run.setProcessChannelMode(QProcess::MergedChannels);
        run.start(c.first(), c.mid(1));
        const bool ok = run.waitForFinished(120000) && run.exitStatus() == QProcess::NormalExit && run.exitCode() == 0;
        QApplication::restoreOverrideCursor();
        *said = QString::fromLocal8Bit(run.readAll()).trimmed();
        if (said->isEmpty() && !ok)
            *said = run.errorString();
        return ok;
    };
}

void WriteProtectDialog::fillDrives()
{
    const QString was = m_drives->currentData().toString();
    m_drives->clear();
    // Sticks and cards first: they're what this is usually about.
    QVector<const Disk *> list;
    for (const Disk &d : m_udisks->disks())
        if (!d.isSystem)
            list << &d;
    std::stable_sort(list.begin(), list.end(), [](const Disk *a, const Disk *b) { return (a->removable || a->sdCard) > (b->removable || b->sdCard); });
    for (const Disk *d : list) {
        QString name = d->model.isEmpty() ? d->device : d->model;
        m_drives->addItem(QStringLiteral("%1 · %2 · %3%4").arg(name, formatSize(d->size), d->device, d->readOnly ? tr(" (write-protected)") : QString()),
                          d->blockPath);
    }
    const int again = m_drives->findData(was);
    if (again >= 0)
        m_drives->setCurrentIndex(again);
    else
        for (int i = 0; i < m_drives->count(); ++i)
            if (m_drives->itemText(i).endsWith(tr(" (write-protected)"))) {
                m_drives->setCurrentIndex(i);
                break;
            }
    m_look->setEnabled(m_drives->count() > 0 || m_testState);
    if (m_drives->count() == 0 && !m_testState)
        m_status->setText(tr("No drives to look at. Plug the stick or card in."));
}

void WriteProtectDialog::pick(const QString &blockPath)
{
    const int at = m_drives->findData(blockPath);
    if (at >= 0)
        m_drives->setCurrentIndex(at);
}

void WriteProtectDialog::look()
{
    if (m_testState) {
        showFindings(*m_testState);
        return;
    }
    const QString blockPath = m_drives->currentData().toString();
    const Disk *disk = nullptr;
    for (const Disk &d : m_udisks->disks())
        if (d.blockPath == blockPath)
            disk = &d;
    if (!disk)
        return;
    State s;
    s.device = disk->device;
    s.name = disk->model.isEmpty() ? disk->device : disk->model;
    s.sdCard = disk->sdCard;
    for (const Volume &v : disk->volumes) {
        if (v.isContainer)
            continue;
        writeprotect::Part p;
        p.device = v.device;
        p.objectPath = v.filesystemPath();
        p.name = v.label.isEmpty() ? QFileInfo(v.device).fileName() : v.label;
        p.fsType = v.effectiveFsType();
        p.number = v.number;
        if (!v.mounts().isEmpty())
            p.mountPoint = v.mounts().first();
        s.parts << p;
    }
    m_look->setEnabled(false);
    m_status->setText(tr("Looking…"));
#ifdef Q_OS_WIN
    s.device = disk->blockPath;
    s.diskNumber = s.device.section(QStringLiteral("PhysicalDrive"), 1).toInt();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    writeprotect::readWindows(s);
    QApplication::restoreOverrideCursor();
    showFindings(s);
#else
    writeprotect::readLinux(s);
    s.writeBlock = writeblock::available() && writeblock::isOn() && !writeblock::allowed().contains(QFileInfo(s.device).fileName());
    // The drive's own answer, through an fd that changes nothing (it isn't even unmounted).
    QPointer<WriteProtectDialog> self(this);
    openBlockThen(m_udisks, this, disk->blockPath, UDisks::OpenMode::Peek, [self, s](int fd) mutable {
        if (fd >= 0) {
            s.drivesays = writeprotect::askDrive(fd);
            blockio::close(fd);
        }
        if (self)
            self->showFindings(s);
    });
#endif
}

void WriteProtectDialog::showFindings(const State &state)
{
    m_state = state;
    m_findings = writeprotect::diagnose(state);
    m_look->setEnabled(true);
    m_look->setText(tr("Look Again"));
    while (m_results->count() > 1) {
        QLayoutItem *item = m_results->takeAt(0);
        delete item->widget();
        delete item;
    }
    const auto card = [this](const QString &title, const QString &text, QPushButton *button) {
        auto *frame = new QFrame;
        frame->setFrameShape(QFrame::StyledPanel);
        auto *box = new QVBoxLayout(frame);
        auto *heading = new QLabel(QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped()));
        heading->setWordWrap(true);
        auto *words = new QLabel(text);
        words->setWordWrap(true);
        words->setTextInteractionFlags(Qt::TextSelectableByMouse);
        box->addWidget(heading);
        box->addWidget(words);
        if (button) {
            auto *buttonRow = new QHBoxLayout;
            buttonRow->addStretch(1);
            buttonRow->addWidget(button);
            box->addLayout(buttonRow);
        }
        m_results->insertWidget(m_results->count() - 1, frame);
    };
    if (m_findings.isEmpty()) {
        m_status->setText(tr("%1 can be written to.").arg(state.name));
        card(tr("Nothing is stopping it"), writeprotect::nothingFound(state), nullptr);
    } else {
        m_status->setText(m_findings.size() == 1 ? tr("Found what stops writing to %1:").arg(state.name)
                                                 : tr("Found %1 things that stop writing to %2:").arg(m_findings.size()).arg(state.name));
        for (int i = 0; i < m_findings.size(); ++i) {
            const Finding &f = m_findings[i];
            QPushButton *button = nullptr;
            if (f.fix != Fix::None) {
                button = new QPushButton(f.fixLabel);
                button->setObjectName(QStringLiteral("fix%1").arg(i));
                connect(button, &QPushButton::clicked, this, [this, i] { fix(i); });
            }
            card(f.title, f.text, button);
        }
    }
    emit looked();
}

void WriteProtectDialog::fix(int index)
{
    if (index < 0 || index >= m_findings.size())
        return;
    const Finding f = m_findings[index];
    const writeprotect::Part part = f.part >= 0 && f.part < m_state.parts.size() ? m_state.parts[f.part] : writeprotect::Part();
    QString said;
    bool ok = true;
    switch (f.fix) {
    case Fix::None:
        return;
    case Fix::AllowWriteBlock: {
        const QStringList c = writeblock::command({QStringLiteral("allow"), QFileInfo(m_state.device).fileName()});
        QProcess run;
        run.setProcessChannelMode(QProcess::MergedChannels);
        run.start(c.first(), c.mid(1));
        ok = run.waitForFinished(60000) && run.exitStatus() == QProcess::NormalExit && run.exitCode() == 0;
        said = QString::fromLocal8Bit(run.readAll()).trimmed();
        break;
    }
    case Fix::ClearKernelFlag:
        ok = runAsRoot(writeprotect::clearKernelFlagScript(m_state), &said);
        break;
    case Fix::RepairAndReopen:
        reopen(part.objectPath, true);
        return;
    case Fix::Reopen:
        reopen(part.objectPath, false);
        return;
#ifdef Q_OS_WIN
    case Fix::ClearPolicy:
        said = writeprotect::clearPolicy();
        ok = said.isEmpty();
        if (ok)
            said = tr("Switched off. Unplug the drive and plug it in again, then look again.");
        break;
    case Fix::ClearDiskAttribute:
        said = writeprotect::clearDiskAttribute(m_state.diskNumber);
        ok = said.isEmpty();
        break;
    case Fix::ClearVolumeAttribute:
        said = writeprotect::clearVolumeAttribute(m_state.diskNumber, part.number);
        ok = said.isEmpty();
        break;
#else
    default:
        return;
#endif
    }
    m_status->setText(ok ? (said.isEmpty() ? tr("Done. Looking again…") : said) : tr("That didn't work: %1").arg(said.right(800)));
    if (!m_testState)
        m_udisks->refresh();
    if (ok)
        look();
}

void WriteProtectDialog::reopen(const QString &objectPath, bool repairFirst)
{
    const Volume *volume = nullptr;
    for (const Disk &d : m_udisks->disks())
        for (const Volume &v : d.volumes)
            if (v.filesystemPath() == objectPath || v.objectPath == objectPath)
                volume = &v;
    if (!volume) {
        m_status->setText(tr("That partition isn't there any more. Look again."));
        return;
    }
    const Volume v = *volume;
    if (repairFirst) {
        // UDisks' repair closes it first; it's opened again when the repair says it's done.
        m_status->setText(tr("Closing it and repairing its file system…"));
        m_waitingFor = v.objectPath;
        m_udisks->repair(v);
        return;
    }
    m_status->setText(tr("Closing it…"));
    m_afterOperation = [this, v](bool ok, const QString &message) {
        if (!ok) {
            m_status->setText(message);
            return;
        }
        m_status->setText(tr("Opening it again for writing…"));
        m_afterOperation = [this](bool, const QString &said) {
            m_status->setText(said);
            look();
        };
        m_udisks->mount(v);
    };
    m_udisks->unmount(v);
}
