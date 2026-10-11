// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "powerbox.h"

#include "drivepower.h"
#include "udisks.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

bool PowerBox::applies(const Disk &disk)
{
    // Spinning down and seek noise are a hard drive's; the rest an SSD has too.
    const bool spins = disk.rotationRate != 0;
    return !disk.isLoop && !disk.nvmeNamespace && ((spins && (disk.ataPm || disk.ataAam)) || disk.ataApm || disk.ataWriteCache || disk.ataLookahead);
}

PowerBox::PowerBox(UDisks *udisks, const Disk &disk, QWidget *parent)
    : QGroupBox(tr("Drive settings"), parent)
    , m_udisks(udisks)
    , m_blockPath(disk.blockPath)
    , m_current(disk.driveConfiguration)
    , m_cacheWas(disk.ataWriteCacheEnabled)
    , m_lookaheadWas(disk.ataLookaheadEnabled)
    , m_state(new QLabel)
    , m_sleep(new QPushButton(tr("Sleep Now")))
    , m_save(new QPushButton(tr("Save")))
{
    auto *form = new QFormLayout(this);

    // Spinning down is a hard drive's; an SSD has nothing to spin.
    const bool spins = disk.rotationRate != 0;
    auto *now = new QHBoxLayout;
    now->addWidget(m_state, 1);
    now->addWidget(m_sleep);
    if (spins && disk.ataPm) {
        form->addRow(tr("Now:"), now);
    } else {
        m_state->hide();
        m_sleep->hide();
    }
    bool inUse = false;
    for (const Volume &v : disk.volumes)
        inUse = inUse || !v.mounts().isEmpty() || v.swapActive;
    m_sleep->setEnabled(disk.ataPm && !inUse);
    m_sleep->setToolTip(inUse ? tr("Something on it is mounted. Unmount it first.") : tr("Spins it down now. It wakes up by itself when it's used."));
    connect(m_sleep, &QPushButton::clicked, this, [this] {
        if (const Disk *d = m_udisks->diskByPath(m_blockPath))
            m_udisks->sleepNow(*d);
        QTimer::singleShot(1500, this, &PowerBox::showState);
    });

    // Each starts at what UDisks keeps for the drive, or "the drive's own" when it keeps nothing.
    auto combo = [this](const QVector<drivepower::Choice> &choices, const QString &key) {
        auto *box = new QComboBox;
        box->addItem(tr("The drive's own setting"), drivepower::kDriveDefault);
        for (const drivepower::Choice &c : choices)
            box->addItem(c.label, c.value);
        if (m_current.contains(key)) {
            const int value = m_current.value(key).toInt();
            int index = box->findData(value);
            if (index < 0) {
                box->addItem(key == QLatin1String("ata-pm-standby") ? tr("After %1").arg(drivepower::standbyText(value)) : tr("Level %1").arg(value), value);
                index = box->count() - 1;
            }
            box->setCurrentIndex(index);
        }
        connect(box, &QComboBox::currentIndexChanged, this, &PowerBox::changed);
        return box;
    };
    if (disk.ataPm && spins) {
        m_standby = combo(drivepower::standbyChoices(), QStringLiteral("ata-pm-standby"));
        form->addRow(tr("Spin down:"), m_standby);
    }
    if (disk.ataApm) {
        m_apm = combo(drivepower::apmChoices(), QStringLiteral("ata-apm-level"));
        form->addRow(tr("Power saving:"), m_apm);
    }
    if (disk.ataAam && spins) {
        m_aam = combo(drivepower::aamChoices(), QStringLiteral("ata-aam-level"));
        form->addRow(tr("Seek noise:"), m_aam);
    }
    if (disk.ataWriteCache) {
        m_cache = new QCheckBox(tr("Write cache (faster; what's in it is lost if the power goes off)"));
        m_cache->setChecked(m_current.contains(QStringLiteral("ata-write-cache-enabled"))
                                ? m_current.value(QStringLiteral("ata-write-cache-enabled")).toBool()
                                : m_cacheWas);
        connect(m_cache, &QCheckBox::toggled, this, &PowerBox::changed);
        form->addRow(QString(), m_cache);
    }
    if (disk.ataLookahead) {
        m_lookahead = new QCheckBox(tr("Read ahead (faster for files read from start to end)"));
        m_lookahead->setChecked(m_current.contains(QStringLiteral("ata-read-lookahead-enabled"))
                                    ? m_current.value(QStringLiteral("ata-read-lookahead-enabled")).toBool()
                                    : m_lookaheadWas);
        connect(m_lookahead, &QCheckBox::toggled, this, &PowerBox::changed);
        form->addRow(QString(), m_lookahead);
    }
    auto *note = new QLabel(tr("Saved settings are set again every time the drive shows up. Spinning down often wears a drive "
                               "faster, and so do the lowest power-saving levels on laptop drives."));
    note->setWordWrap(true);
    form->addRow(note);
    auto *saveRow = new QHBoxLayout;
    saveRow->addStretch();
    saveRow->addWidget(m_save);
    form->addRow(saveRow);
    m_save->setEnabled(false);
    connect(m_save, &QPushButton::clicked, this, [this] {
        const QVariantMap next = configuration();
        if (const Disk *d = m_udisks->diskByPath(m_blockPath))
            m_udisks->setPowerSettings(*d, next);
        m_current = next;
        changed();
    });
    showState();
}

QVariantMap PowerBox::configuration() const
{
    // A switch left as it was is no new setting.
    auto toggled = [this](const QCheckBox *box, const QString &key, bool was) {
        if (!box)
            return drivepower::kKeep;
        const bool before = m_current.contains(key) ? m_current.value(key).toBool() : was;
        return box->isChecked() == before ? drivepower::kKeep : int(box->isChecked());
    };
    drivepower::Changes c;
    c.standby = m_standby ? m_standby->currentData().toInt() : drivepower::kKeep;
    c.apm = m_apm ? m_apm->currentData().toInt() : drivepower::kKeep;
    c.aam = m_aam ? m_aam->currentData().toInt() : drivepower::kKeep;
    c.writeCache = toggled(m_cache, QStringLiteral("ata-write-cache-enabled"), m_cacheWas);
    c.lookahead = toggled(m_lookahead, QStringLiteral("ata-read-lookahead-enabled"), m_lookaheadWas);
    return drivepower::configuration(m_current, c);
}

void PowerBox::changed()
{
    m_save->setEnabled(configuration() != m_current);
}

void PowerBox::showState()
{
    const Disk *d = m_udisks->diskByPath(m_blockPath);
    const int state = d ? m_udisks->powerState(*d) : -1;
    m_state->setText(state < 0 ? tr("Unknown") : drivepower::stateText(state));
}
