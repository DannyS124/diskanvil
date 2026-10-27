// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "convertdialog.h"

#include "applog.h"
#include "blockio.h"
#include "dialogs.h"
#include "format.h"
#include "jobui.h"
#include "recoverdialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

ConvertTableDialog::ConvertTableDialog(UDisks *udisks, const Disk &disk, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_blockPath(disk.blockPath)
    , m_device(shortDevice(disk.device))
    , m_driveKey(disk.health.key)
    , m_before(recover::fromDisk(disk))
    , m_changes(new QLabel)
    , m_status(new QLabel)
    , m_confirm(new QLineEdit)
    , m_change(new QPushButton)
    , m_undo(new QPushButton(tr("Undo")))
{
    const bool toGpt = disk.tableType == QLatin1String("dos");
    // Started the BIOS way: a partition marked to start from.
    bool bootable = false;
    for (const Volume &v : disk.volumes)
        bootable = bootable || (toGpt && (v.partFlags & 0x80));
    m_plan = toGpt ? tableconvert::toGpt(m_before, disk.size, disk.sectorSize, bootable)
                   : tableconvert::toMbr(m_before, disk.size, disk.sectorSize);
    const QString to = toGpt ? QStringLiteral("GPT") : QStringLiteral("MBR");
    setWindowTitle(tr("Change to %1").arg(to));
    m_change->setText(tr("Change to %1").arg(to));

    auto *intro = new QLabel(tr("<p><b>%1</b></p><p>Changes its partition table from %2 to %3. Every partition keeps its place, its size "
                                "and everything in it: only the table changes. The table it has now is kept, and Undo puts it back.</p>")
                                 .arg(diskTitle(disk).toHtmlEscaped(), toGpt ? QStringLiteral("MBR") : QStringLiteral("GPT"), to));
    intro->setWordWrap(true);
    QStringList lines;
    for (const QString &c : std::as_const(m_plan.changes))
        lines << QStringLiteral("• ") + c.toHtmlEscaped();
    for (const QString &w : std::as_const(m_plan.warnings))
        lines << QStringLiteral("<span style=\"color:#b07800\">") + w.toHtmlEscaped() + QStringLiteral("</span>");
    QString refusal = RecoverDialog::refusal(disk);
    if (refusal.isEmpty())
        refusal = m_plan.refusal;
    if (!refusal.isEmpty())
        lines << redText(refusal);
    m_changes->setText(lines.join(QStringLiteral("<br>")));
    m_changes->setWordWrap(true);
    m_changes->setObjectName(QStringLiteral("changes"));
    m_status->setWordWrap(true);
    m_confirm->setPlaceholderText(m_device);
    m_confirm->setVisible(refusal.isEmpty());
    m_change->setObjectName(QStringLiteral("change"));
    m_undo->setObjectName(QStringLiteral("undo"));
    m_undo->setEnabled(false);
    m_change->setProperty("refused", !refusal.isEmpty());

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->addButton(m_undo, QDialogButtonBox::ActionRole);
    buttons->addButton(m_change, QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_change, &QPushButton::clicked, this, &ConvertTableDialog::change);
    connect(m_undo, &QPushButton::clicked, this, &ConvertTableDialog::undo);
    connect(m_confirm, &QLineEdit::textChanged, this, &ConvertTableDialog::updateState);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(m_changes);
    if (refusal.isEmpty())
        layout->addWidget(new QLabel(tr("Type <b>%1</b> to go ahead:").arg(m_device)));
    layout->addWidget(m_confirm);
    layout->addWidget(m_status);
    layout->addStretch();
    layout->addWidget(buttons);
    resize(640, 420);
    updateState();
}

void ConvertTableDialog::updateState()
{
    m_change->setEnabled(!m_busy && !m_changed && !m_change->property("refused").toBool() && m_confirm->text().trimmed() == m_device);
    m_undo->setEnabled(!m_busy && m_changed);
}

void ConvertTableDialog::change()
{
    if (m_busy || m_changed || m_change->property("refused").toBool())
        return;
    write(m_plan.to, false);
}

void ConvertTableDialog::undo()
{
    if (m_busy || !m_changed)
        return;
    // Once the new table is read, the system can mount its partitions by itself (Windows always
    // does, a desktop that mounts drives may): those were unmounted to change it, so they're
    // unmounted again to put it back. Anything locked or swapped on meanwhile is left to the user.
    // Read fresh: Windows only looks at drive letters now and then.
    m_udisks->refresh();
    const Disk *disk = m_udisks->diskByPath(m_blockPath);
    QVector<Volume> mounted;
    bool other = false;
    for (const Volume &v : disk ? disk->volumes : QVector<Volume>{}) {
        if (v.swapActive || !v.cleartextPath.isEmpty())
            other = true;
        else if (!v.mounts().isEmpty())
            mounted << v;
    }
    if (mounted.isEmpty() || other)
        return write(m_before, true);
    m_busy = true;
    updateState();
    m_status->setText(tr("Unmounting the partitions the system mounted again…"));
    m_udisks->unmountThen(mounted, tr("Couldn't unmount them to put the old table back"), [this] {
        m_busy = false;
        m_udisks->refresh();
        write(m_before, true);
    }, false, [this] {
        m_busy = false;
        m_status->setText(redText(tr("Couldn't unmount the drive's partitions to put the old table back. Close what's using them and try Undo again.")));
        updateState();
    });
}

void ConvertTableDialog::write(const recover::Layout &layout, bool undoing)
{
    const Disk *disk = m_udisks->diskByPath(m_blockPath);
    if (!disk || !RecoverDialog::refusal(*disk).isEmpty()) {
        m_status->setText(redText(disk ? RecoverDialog::refusal(*disk) : tr("The drive isn't there anymore.")));
        return;
    }
    // The table it has now goes on Recover Partitions' list first, either way.
    recover::remember(m_driveKey, recover::fromDisk(*disk));
    m_busy = true;
    updateState();
    m_status->setText(undoing ? tr("Putting the old table back…") : tr("Writing the new table…"));
    openBlockThen(m_udisks, this, m_blockPath, UDisks::OpenMode::ReadWrite, [this, layout, undoing](int fd) {
        if (fd < 0) {
            m_busy = false;
            m_status->setText(redText(tr("Couldn't open the drive for writing.")));
            updateState();
            return;
        }
        const gpt::Result result = recover::write(fd, layout);
        blockio::close(fd);
        qCInfo(lcOps).noquote() << "Partition table of" << m_blockPath << (undoing ? "put back to" : "changed to") << layout.table
                                << (result.ok ? QStringLiteral("done") : QStringLiteral("failed: ") + result.error);
        if (!result.ok) {
            m_busy = false;
            m_status->setText(redText(result.error));
            updateState();
            emit done(false, result.error);
            return;
        }
        m_udisks->rescan(m_blockPath);
        waitFor(layout.table, int(std::count_if(layout.parts.cbegin(), layout.parts.cend(), [](const recover::Part &p) { return !p.container; })),
                undoing);
    });
}

void ConvertTableDialog::waitFor(const QString &table, int partitions, bool undoing)
{
    // The drive is read again once it's closed: done when it shows the table and its partitions.
    auto *tries = new int(0);
    auto *timer = new QTimer(this);
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, this, [this, timer, tries, table, partitions, undoing] {
        m_udisks->refresh();
        const Disk *d = m_udisks->diskByPath(m_blockPath);
        int found = 0;
        if (d) {
            for (const Volume &v : d->volumes)
                found += v.number > 0 && !v.isContainer ? 1 : 0;
        }
        const bool there = d && d->tableType == table && found >= partitions;
        if (!there && ++*tries < 40)
            return;
        timer->stop();
        timer->deleteLater();
        delete tries;
        m_busy = false;
        m_changed = !undoing && there;
        const QString name = table == QLatin1String("gpt") ? QStringLiteral("GPT") : QStringLiteral("MBR");
        const QString message = !there ? tr("The table was written, but %1 doesn't show it yet: unplug it and plug it back in.").arg(m_device)
                              : undoing ? tr("%1 has its old %2 table back.").arg(m_device, name)
                                        : tr("%1 has a %2 table now, with all its partitions. Undo puts the old one back.").arg(m_device, name);
        m_status->setText(message.toHtmlEscaped());
        updateState();
        emit done(there, message);
    });
    timer->start();
}
