// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "whathappened.h"

#include "format.h"
#include "health.h"

#include <QCommandLinkButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {

QLabel *paragraph(const QString &text = {})
{
    auto *label = new QLabel(text);
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    return label;
}

} // namespace

WhatHappenedDialog::WhatHappenedDialog(QVector<Disk> disks, bool runningFromStick, QWidget *parent)
    : QDialog(parent)
    , m_disks(std::move(disks))
    , m_fromStick(runningFromStick)
    , m_pages(new QStackedWidget)
    , m_drives(new QListWidget)
    , m_odds(paragraph())
    , m_dont(paragraph())
    , m_steps(paragraph())
    , m_toolButtons(new QVBoxLayout)
{
    setWindowTitle(tr("What Happened?"));
    setObjectName(QStringLiteral("whatHappened"));
    resize(640, 620);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_pages, 1);

    // What happened.
    auto *symptoms = new QWidget;
    auto *symptomsLayout = new QVBoxLayout(symptoms);
    symptomsLayout->addWidget(paragraph(tr("<b>What happened?</b> Pick the closest. The sooner something is done, and the less the drive "
                                           "is used meanwhile, the more comes back.")));
    struct Choice {
        triage::Symptom symptom;
        const char *id;
        QString title, more;
    };
    const QVector<Choice> choices = {
        {triage::Symptom::Deleted, "deleted", tr("I deleted files, or emptied the Recycle Bin"), tr("By mistake, or a program did")},
        {triage::Symptom::Formatted, "formatted", tr("I formatted a drive or a memory card"), tr("And it had files on it I want back")},
        {triage::Symptom::WontOpen, "wontOpen", tr("A drive or card won't open, or asks to be formatted"), tr("Windows says it's RAW, or it's empty")},
        {triage::Symptom::Failing, "failing", tr("A drive clicks, is very slow, or keeps disappearing"), tr("Or Disk Health says it's failing")},
        {triage::Symptom::PartitionGone, "partitionGone", tr("A partition or a drive letter has gone"), tr("The space shows as unallocated")},
        {triage::Symptom::Ransomware, "ransomware", tr("My files were locked by ransomware"), tr("Their names end in something new, and there's a note")},
        {triage::Symptom::Damaged, "damaged", tr("Photos, films or documents won't open"), tr("They're there, but damaged or cut short")},
        {triage::Symptom::WindowsWontStart, "windows", tr("Windows won't start"), tr("And my files are on it")},
    };
    for (const Choice &c : choices) {
        auto *button = new QCommandLinkButton(c.title, c.more);
        button->setObjectName(QString::fromLatin1(c.id));
        connect(button, &QCommandLinkButton::clicked, this, [this, s = c.symptom] { pickSymptom(s); });
        symptomsLayout->addWidget(button);
    }
    symptomsLayout->addStretch();
    m_pages->addWidget(symptoms);

    // Which drive.
    auto *drivePage = new QWidget;
    auto *driveLayout = new QVBoxLayout(drivePage);
    driveLayout->addWidget(paragraph(tr("<b>Which drive was it on?</b>")));
    m_drives->setObjectName(QStringLiteral("drives"));
    for (int i = 0; i < m_disks.size(); ++i) {
        const Disk &d = m_disks[i];
        QStringList kind;
        kind << (d.removable || d.bus == QLatin1String("usb") ? tr("USB stick or card") : d.rotationRate == 0 ? tr("SSD") : tr("hard drive"));
        if (health::hasUnreadableSpots(d.health))
            kind << tr("failing");
        if (d.isSystem && !m_fromStick)
            kind << tr("this computer's system is on it");
        auto *item = new QListWidgetItem(QStringLiteral("%1, %2 (%3)").arg(d.model.isEmpty() ? d.device : d.model, formatSize(d.size), kind.join(QStringLiteral(", "))),
                                         m_drives);
        item->setData(Qt::UserRole, i);
    }
    auto *unknown = new QListWidgetItem(tr("I don't know, or it isn't connected"), m_drives);
    unknown->setData(Qt::UserRole, -1);
    m_drives->setCurrentRow(0);
    driveLayout->addWidget(m_drives, 1);
    auto *driveButtons = new QHBoxLayout;
    auto *back = new QPushButton(tr("Back"));
    auto *next = new QPushButton(tr("Next"));
    next->setObjectName(QStringLiteral("next"));
    next->setDefault(true);
    driveButtons->addWidget(back);
    driveButtons->addStretch();
    driveButtons->addWidget(next);
    driveLayout->addLayout(driveButtons);
    connect(back, &QPushButton::clicked, this, [this] { m_pages->setCurrentIndex(0); });
    connect(next, &QPushButton::clicked, this, &WhatHappenedDialog::showAdvice);
    connect(m_drives, &QListWidget::itemDoubleClicked, this, &WhatHappenedDialog::showAdvice);
    m_pages->addWidget(drivePage);

    // What to do.
    auto *advicePage = new QWidget;
    auto *adviceLayout = new QVBoxLayout(advicePage);
    m_odds->setObjectName(QStringLiteral("odds"));
    m_dont->setObjectName(QStringLiteral("dont"));
    m_steps->setObjectName(QStringLiteral("steps"));
    adviceLayout->addWidget(m_odds);
    adviceLayout->addWidget(m_dont);
    adviceLayout->addWidget(m_steps);
    adviceLayout->addLayout(m_toolButtons);
    adviceLayout->addStretch();
    auto *adviceButtons = new QHBoxLayout;
    auto *again = new QPushButton(tr("Back"));
    auto *close = new QPushButton(tr("Close"));
    adviceButtons->addWidget(again);
    adviceButtons->addStretch();
    adviceButtons->addWidget(close);
    adviceLayout->addLayout(adviceButtons);
    connect(again, &QPushButton::clicked, this, [this] { m_pages->setCurrentIndex(1); });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    m_pages->addWidget(advicePage);
}

void WhatHappenedDialog::pickSymptom(triage::Symptom symptom)
{
    m_symptom = symptom;
    m_pages->setCurrentIndex(1);
}

void WhatHappenedDialog::showAdvice()
{
    const int i = m_drives->currentItem() ? m_drives->currentItem()->data(Qt::UserRole).toInt() : -1;
    triage::Drive drive;
    QString blockPath;
    if (i >= 0 && i < m_disks.size()) {
        drive = triage::facts(m_disks[i], m_fromStick);
        if (m_testTrim)
            drive.trim = *m_testTrim;
        blockPath = m_disks[i].blockPath;
    }
    const triage::Advice a = triage::advise(m_symptom, drive);
    const QString colour = a.odds == triage::Advice::Odds::Good ? QStringLiteral("#2bee8a")
                         : a.odds == triage::Advice::Odds::Fair ? QStringLiteral("#ffb300")
                         : a.odds == triage::Advice::Odds::Poor ? QStringLiteral("#ff3355")
                                                                : QStringLiteral("#9ad8ff");
    m_odds->setText(QStringLiteral("<p><b>%1</b> <span style=\"color:%2\">●</span> %3</p><p>%4</p>")
                        .arg(tr("The odds:"), colour, triage::oddsWords(a.odds).toHtmlEscaped(), a.odds_why.toHtmlEscaped()));
    QString dont = QStringLiteral("<p><b>%1</b></p><ul>").arg(tr("Don't:"));
    for (const QString &d : a.dont)
        dont += QStringLiteral("<li>%1</li>").arg(d.toHtmlEscaped());
    m_dont->setText(dont + QStringLiteral("</ul>"));
    QString steps = QStringLiteral("<p><b>%1</b></p><ol>").arg(tr("Do:"));
    for (const QString &s : a.steps)
        steps += QStringLiteral("<li>%1</li>").arg(s.toHtmlEscaped());
    m_steps->setText(steps + QStringLiteral("</ol>"));
    while (QLayoutItem *old = m_toolButtons->takeAt(0)) {
        delete old->widget();
        delete old;
    }
    const QHash<QString, QString> names = {{QStringLiteral("lost-files"), tr("Find Lost Files")},
                                           {QStringLiteral("rescue-copy"), tr("Make a Rescue Copy")},
                                           {QStringLiteral("health"), tr("Disk Health")},
                                           {QStringLiteral("recover-partitions"), tr("Recover Partitions")},
                                           {QStringLiteral("rescue-usb"), tr("Make a DiskForge Live Stick")},
                                           {QStringLiteral("virus-scan"), tr("Scan for Viruses")},
                                           {QStringLiteral("drivers"), tr("Windows Drivers and Services")},
                                           {QStringLiteral("save-files"), tr("Save My Files")}};
    for (const QString &tool : a.tools) {
        auto *button = new QPushButton(names.value(tool, tool));
        button->setObjectName(QStringLiteral("tool-") + tool);
        // Tools that work on a drive need one picked.
        const bool needsDrive = tool == QLatin1String("rescue-copy") || tool == QLatin1String("health") || tool == QLatin1String("recover-partitions");
        button->setEnabled(!needsDrive || !blockPath.isEmpty());
        connect(button, &QPushButton::clicked, this, [this, tool, blockPath] { Q_EMIT openTool(tool, blockPath); });
        m_toolButtons->addWidget(button);
    }
    m_pages->setCurrentIndex(2);
}
