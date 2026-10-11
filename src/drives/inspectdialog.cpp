// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "inspectdialog.h"

#include "blockio.h"
#include "dialogs.h"
#include "format.h"
#include "jobui.h"
#include "theme.h"

#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <unistd.h>

TableInspectorDialog::TableInspectorDialog(UDisks *udisks, const Disk &disk, QWidget *parent)
    : QDialog(parent)
    , m_device(shortDevice(disk.device))
    , m_summary(new QLabel(tr("Reading the drive…")))
    , m_tree(new QTreeWidget)
    , m_lba(new QLineEdit)
    , m_hex(new QPlainTextEdit)
{
    build(tr("Partition Table of %1").arg(diskTitle(disk)));
    // Read-only, like a benchmark: nothing on the drive gets unmounted.
    openBlockThen(udisks, this, disk.blockPath, UDisks::OpenMode::Benchmark, [this](int fd) { load(fd); });
}

TableInspectorDialog::TableInspectorDialog(int fd, const QString &title, QWidget *parent)
    : QDialog(parent)
    , m_summary(new QLabel(tr("Reading…")))
    , m_tree(new QTreeWidget)
    , m_lba(new QLineEdit)
    , m_hex(new QPlainTextEdit)
{
    build(tr("Partition Table of %1").arg(title));
    load(fd);
}

void TableInspectorDialog::build(const QString &title)
{
    setWindowTitle(title);
    m_summary->setWordWrap(true);
    m_summary->setTextFormat(Qt::RichText);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({tr("Field"), tr("Value")});
    m_tree->setAlternatingRowColors(true);
    m_hex->setReadOnly(true);
    m_hex->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_hex->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_hex->setMinimumHeight(180);

    auto *go = new QPushButton(tr("Show"));
    m_lba->setPlaceholderText(tr("sector number"));
    m_lba->setMaximumWidth(160);
    auto *sectorRow = new QHBoxLayout;
    sectorRow->addWidget(new QLabel(tr("Sector:")));
    sectorRow->addWidget(m_lba);
    sectorRow->addWidget(go);
    for (const QString &name : {tr("MBR"), tr("GPT Header"), tr("Partition List"), tr("Backup Header")}) {
        auto *jump = new QPushButton(name);
        jump->setEnabled(false);
        m_jumps << jump;
        sectorRow->addWidget(jump);
    }
    sectorRow->addStretch();
    connect(go, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const quint64 lba = m_lba->text().trimmed().toULongLong(&ok);
        if (ok)
            showSector(lba);
    });
    connect(m_lba, &QLineEdit::returnPressed, go, &QPushButton::click);
    connect(m_jumps[0], &QPushButton::clicked, this, [this] { showSector(0); });
    connect(m_jumps[1], &QPushButton::clicked, this, [this] { showSector(1); });
    connect(m_jumps[2], &QPushButton::clicked, this, [this] { showSector(m_report.primary.entriesLba ? m_report.primary.entriesLba : 2); });
    connect(m_jumps[3], &QPushButton::clicked, this, [this] { showSector(m_report.lastLba); });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_summary);
    layout->addWidget(m_tree, 3);
    layout->addLayout(sectorRow);
    layout->addWidget(m_hex, 2);
    layout->addWidget(buttons);
    resize(860, 720);
}

void TableInspectorDialog::load(int fd)
{
    if (fd < 0) {
        m_summary->setText(redText(tr("Couldn't open the drive.")));
        return;
    }
    m_fd = fd;
    m_report = gpt::inspect(fd);
    fill();
    showSector(0);
    for (QPushButton *jump : std::as_const(m_jumps))
        jump->setEnabled(true);
    emit ready();
}

TableInspectorDialog::~TableInspectorDialog()
{
    if (m_fd >= 0)
        blockio::close(m_fd);
}

void TableInspectorDialog::fill()
{
    const gpt::Report &r = m_report;
    if (r.problems.isEmpty()) {
        const bool gpt = r.primary.present;
        m_summary->setText(gpt ? tr("<b>Looks fine.</b> Both copies of the GPT are intact and match.")
                           : r.mbr.signature ? tr("<b>Looks fine.</b> An MBR partition table.")
                                             : tr("No partition table."));
    } else {
        QString html = QStringLiteral("<b>%1</b>").arg(tr("Problems found:").toHtmlEscaped());
        for (const QString &p : r.problems)
            html += QStringLiteral("<br>• ") + p.toHtmlEscaped();
        m_summary->setText(QStringLiteral("<span style=\"color:%1\">%2</span>").arg(Theme::instance().html(Theme::Role::Danger), html));
    }

    m_tree->clear();
    auto *mbr = new QTreeWidgetItem(m_tree, {tr("MBR (sector 0)"), r.mbr.signature ? tr("signature 55 aa") : tr("no signature")});
    for (const gpt::MbrEntry &e : r.mbr.entries) {
        const QString type = QStringLiteral("0x%1").arg(e.type, 2, 16, QLatin1Char('0'));
        new QTreeWidgetItem(mbr, {tr("Entry %1").arg(e.index),
                                  tr("%1, sectors %2 to %3%4")
                                      .arg(e.type == 0xEE ? tr("GPT protective (0xee)") : partitionTypeName(type))
                                      .arg(e.firstLba)
                                      .arg(quint64(e.firstLba) + e.sectors - (e.sectors ? 1 : 0))
                                      .arg(e.bootable ? tr(", bootable") : QString())});
    }
    mbr->setExpanded(true);
    addHeader(tr("GPT header (sector %1)").arg(r.primary.lba ? r.primary.lba : 1), r.primary);
    if (r.primary.present || r.backup.present)
        addHeader(tr("Backup GPT header (sector %1)").arg(r.backup.lba ? r.backup.lba : r.lastLba), r.backup);
    auto *disk = new QTreeWidgetItem(m_tree, {tr("Drive"), tr("%n sector(s) of %1 bytes", nullptr, int(qMin<quint64>(r.lastLba + 1, INT_MAX))).arg(r.sectorSize)});
    new QTreeWidgetItem(disk, {tr("Last sector"), QString::number(r.lastLba)});
    m_tree->resizeColumnToContents(0);
}

void TableInspectorDialog::addHeader(const QString &title, const gpt::Header &h)
{
    const quint64 sector = quint64(std::max(m_report.sectorSize, 512));
    auto ok = [this](bool good) { return good ? tr("right") : tr("WRONG"); };
    auto *item = new QTreeWidgetItem(m_tree, {title, !h.present ? tr("missing") : h.valid() ? tr("intact") : tr("damaged")});
    item->setExpanded(true);
    if (!h.present)
        return;
    auto row = [item](const QString &name, const QString &value) { new QTreeWidgetItem(item, {name, value}); };
    row(tr("Revision"), QStringLiteral("%1.%2").arg(h.revision >> 16).arg(h.revision & 0xFFFF));
    row(tr("Header size"), h.sizeOk ? tr("%1 bytes").arg(h.headerSize) : tr("%1 bytes (can't be right)").arg(h.headerSize));
    row(tr("Header checksum"), h.sizeOk ? ok(h.headerCrcOk) : tr("not checked"));
    row(tr("This copy says it's at"), tr("sector %1").arg(h.myLba));
    row(tr("Other copy at"), tr("sector %1").arg(h.alternateLba));
    row(tr("Usable sectors"), tr("%1 to %2").arg(h.firstUsable).arg(h.lastUsable));
    row(tr("Disk ID"), h.diskGuid);
    row(tr("Partition list"), tr("sector %1, %2 slots of %3 bytes").arg(h.entriesLba).arg(h.entryCount).arg(h.entrySize));
    row(tr("List checksum"), h.entriesRead ? ok(h.entriesCrcOk) : tr("not read (the sizes can't be right)"));
    auto *parts = new QTreeWidgetItem(item, {tr("Partitions"), tr("%n in use", nullptr, int(h.entries.size()))});
    parts->setExpanded(true);
    for (const gpt::Entry &e : h.entries) {
        const quint64 sectors = e.lastLba >= e.firstLba ? e.lastLba - e.firstLba + 1 : 0;
        auto *p = new QTreeWidgetItem(parts, {tr("%1. %2").arg(e.index).arg(partitionTypeName(e.type)),
                                              tr("sectors %1 to %2 (%3)%4")
                                                  .arg(e.firstLba)
                                                  .arg(e.lastLba)
                                                  .arg(formatSize(sectors * sector))
                                                  .arg(e.name.isEmpty() ? QString() : QStringLiteral(", \"%1\"").arg(e.name))});
        const QString details = tr("Partition ID: %1\nType: %2\nAttributes: 0x%3").arg(e.guid, e.type).arg(e.attributes, 16, 16, QLatin1Char('0'));
        p->setToolTip(0, details);
        p->setToolTip(1, details);
    }
}

void TableInspectorDialog::showSector(quint64 lba)
{
    if (m_fd < 0)
        return;
    if (lba > m_report.lastLba) {
        m_hex->setPlainText(tr("The last sector is %1.").arg(m_report.lastLba));
        return;
    }
    const quint64 sector = quint64(std::max(m_report.sectorSize, 512));
    blockio::Buffer buf = blockio::alignedBuffer(std::max<size_t>(sector, blockio::kAlign));
    if (!buf || !blockio::readAt(m_fd, buf.get(), sector, lba * sector)) {
        m_hex->setPlainText(tr("Couldn't read sector %1.").arg(lba));
        return;
    }
    m_lba->setText(QString::number(lba));
    m_hex->setPlainText(gpt::hexDump(QByteArray(buf.get(), qsizetype(sector)), lba * sector));
}
