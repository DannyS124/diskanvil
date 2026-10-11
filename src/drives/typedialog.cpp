// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "typedialog.h"

#include "dialogs.h"
#include "format.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

PartitionTypeDialog::PartitionTypeDialog(const Disk &disk, const Volume &volume, QWidget *parent)
    : QDialog(parent)
    , m_table(disk.tableType)
    , m_oldType(volume.partType.toLower())
    , m_oldFlags(volume.partFlags)
    , m_types(new QComboBox)
    , m_other(new QLineEdit)
    , m_problem(new QLabel)
    , m_warning(new QLabel)
{
    setWindowTitle(tr("Type and Flags of %1").arg(shortDevice(volume.device)));
    for (const Volume &v : disk.volumes)
        m_driveBoots = m_driveBoots || isBootPartitionType(v.partType.toLower());

    auto *intro = new QLabel(tr("The type tells other systems and installers what the partition is for. Changing it doesn't "
                                "change what's on the partition."));
    intro->setWordWrap(true);

    // Common types; the current one first when it isn't one of them.
    bool listed = false;
    for (const PartitionTypeChoice &c : partitionTypeChoices(m_table)) {
        m_types->addItem(c.name, c.value);
        m_types->setItemData(m_types->count() - 1, c.value, Qt::ToolTipRole);
        listed = listed || c.value == m_oldType;
    }
    if (!listed && !m_oldType.isEmpty())
        m_types->insertItem(0, tr("Now: %1").arg(partitionTypeName(m_oldType)), m_oldType);
    m_types->addItem(tr("Other…"), QString());
    m_types->setCurrentIndex(qMax(0, m_types->findData(m_oldType)));
    m_other->setPlaceholderText(m_table == QLatin1String("gpt") ? QStringLiteral("0fc63daf-8483-4772-8e79-3d69d8477de4") : QStringLiteral("0x83"));
    m_problem->setWordWrap(true);
    m_problem->setTextFormat(Qt::PlainText);

    auto *form = new QFormLayout;
    form->addRow(tr("Type:"), m_types);
    form->addRow(QString(), m_other);
    form->addRow(QString(), m_problem);

    auto *flagsBox = new QVBoxLayout;
    for (const PartitionFlagChoice &f : partitionFlagChoices(m_table)) {
        auto *check = new QCheckBox(f.name);
        check->setToolTip(f.hint);
        check->setChecked(m_oldFlags & f.bit);
        m_flags.insert(f.bit, check);
        flagsBox->addWidget(check);
        connect(check, &QCheckBox::toggled, this, &PartitionTypeDialog::refresh);
    }
    if (!m_flags.isEmpty())
        form->addRow(tr("Flags:"), flagsBox);

    m_warning->setWordWrap(true);
    m_warning->setTextFormat(Qt::RichText);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(form);
    layout->addWidget(m_warning);
    layout->addStretch();
    layout->addWidget(dialogButtons(this, tr("Change"), &m_ok));

    connect(m_types, &QComboBox::currentIndexChanged, this, &PartitionTypeDialog::refresh);
    connect(m_other, &QLineEdit::textChanged, this, &PartitionTypeDialog::refresh);
    refresh();
    resize(520, sizeHint().height());
}

QString PartitionTypeDialog::type() const
{
    const QString chosen = m_types->currentData().toString();
    return chosen.isEmpty() ? m_other->text().trimmed().toLower() : chosen;
}

quint64 PartitionTypeDialog::flags() const
{
    quint64 chosen = 0;
    for (auto it = m_flags.cbegin(); it != m_flags.cend(); ++it) {
        if (it.value()->isChecked())
            chosen |= it.key();
    }
    return mergedPartitionFlags(m_table, m_oldFlags, chosen);
}

bool PartitionTypeDialog::typeChanged() const
{
    return type() != m_oldType;
}

bool PartitionTypeDialog::flagsChanged() const
{
    return flags() != m_oldFlags;
}

void PartitionTypeDialog::refresh()
{
    const bool other = m_types->currentData().toString().isEmpty();
    m_other->setVisible(other);
    const QString problem = other && !m_other->text().trimmed().isEmpty() ? partitionTypeProblem(m_table, type()) : QString();
    m_problem->setText(problem);
    m_problem->setVisible(!problem.isEmpty());
    const bool valid = !type().isEmpty() && partitionTypeProblem(m_table, type()).isEmpty();
    m_ok->setEnabled(valid && (typeChanged() || flagsChanged()));

    // Booting goes by these: say so before it's changed.
    const bool bootType = typeChanged() && (isBootPartitionType(m_oldType) || isBootPartitionType(type()));
    const bool bootFlags = flagsChanged() && (m_driveBoots || m_table == QLatin1String("dos"));
    m_warning->setText(bootType || bootFlags ? redText(tr("This drive may not start the PC any more after this change, if it "
                                                         "boots from it."))
                                             : QString());
    m_warning->setVisible(bootType || bootFlags);
}
