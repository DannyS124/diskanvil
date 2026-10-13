// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "burnindialog.h"

#include "applog.h"
#include "dialogs.h"
#include "format.h"
#include "pdfreport.h"
#include "stickcheck.h"
#include "udisks.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

using burnin::Step;
using Outcome = burnin::StepResult::Outcome;

bool BurnInDialog::allowLoopDevicesForTest = false;

namespace {

// How long a self-test that never shows as running is waited for before its last result is taken.
constexpr qint64 kStartWaitMs = 90 * 1000;

} // namespace

BurnInDialog::BurnInDialog(UDisks *udisks, const QString &preferredDisk, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_targets(new QComboBox)
    , m_twice(new QCheckBox(tr("Write everything twice, the second time with every bit flipped (twice as long, finds more)")))
    , m_warning(new QLabel)
    , m_confirm(new QLineEdit)
    , m_start(new QPushButton(tr("Start Burn-In")))
    , m_pdf(new QPushButton(tr("Save as PDF…")))
    , m_steps(new QTreeWidget)
    , m_progress(new QProgressBar)
    , m_phase(new QLabel)
    , m_result(new QLabel)
    , m_meter(m_progress, m_phase)
    , m_poll(new QTimer(this))
{
    setWindowTitle(tr("Burn In a New Drive"));
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("Before trusting a new (or second-hand) drive with your files, work it hard once: its own short "
                                       "self-test, every byte written and read back, its long self-test, and its health counters "
                                       "compared from before to after. A weak drive usually shows itself in the first hours: better now, "
                                       "while it can still go back. It takes hours, a day or more for a big hard drive, and <b>erases "
                                       "everything on it</b>.")));
    auto *form = new QFormLayout;
    m_targets->setObjectName(QStringLiteral("targets"));
    form->addRow(tr("Drive:"), m_targets);
    layout->addLayout(form);
    m_twice->setObjectName(QStringLiteral("twice"));
    layout->addWidget(m_twice);
    m_warning->setWordWrap(true);
    m_warning->setTextFormat(Qt::RichText);
    layout->addWidget(m_warning);
    m_confirm->setObjectName(QStringLiteral("confirm"));
    layout->addWidget(m_confirm);
    m_steps->setObjectName(QStringLiteral("steps"));
    m_steps->setHeaderLabels({tr("Step"), tr("How it went")});
    m_steps->setRootIsDecorated(false);
    m_steps->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_steps->setMinimumHeight(140);
    layout->addWidget(m_steps, 1);
    m_progress->hide();
    layout->addWidget(m_progress);
    m_phase->setWordWrap(true);
    layout->addWidget(m_phase);
    m_result->setObjectName(QStringLiteral("result"));
    m_result->setWordWrap(true);
    layout->addWidget(m_result);
    auto *buttons = new QHBoxLayout;
    m_pdf->setEnabled(false);
    m_start->setObjectName(QStringLiteral("start"));
    auto *close = new QPushButton(tr("Close"));
    buttons->addWidget(m_pdf);
    buttons->addStretch();
    buttons->addWidget(m_start);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(close, &QPushButton::clicked, this, &BurnInDialog::reject);
    connect(m_start, &QPushButton::clicked, this, &BurnInDialog::start);
    connect(m_pdf, &QPushButton::clicked, this, [this] {
        pdfreport::saveAs(this, burnin::html(m_report), tr("Burn-in %1.pdf").arg(QDate::currentDate().toString(Qt::ISODate)), tr("Burn-in report"));
    });
    connect(m_targets, &QComboBox::currentIndexChanged, this, &BurnInDialog::updateState);
    connect(m_confirm, &QLineEdit::textChanged, this, &BurnInDialog::updateState);
    connect(m_udisks, &UDisks::changed, this, [this] {
        if (m_running)
            return;
        fillTargets(m_targets->currentData().toString());
        updateState();
    });
    m_poll->setInterval(10000);
    connect(m_poll, &QTimer::timeout, this, &BurnInDialog::pollSelftest);
    fillTargets(preferredDisk);
    showSteps();
    updateState();
    resize(700, 560);
}

BurnInDialog::~BurnInDialog()
{
    if (m_thread) {
        if (m_checker)
            m_checker->cancel();
        m_thread->quit();
        m_thread->wait();
    }
}

void BurnInDialog::fillTargets(const QString &preferred)
{
    const QSignalBlocker block(m_targets);
    m_targets->clear();
    const QVector<Disk> &disks = m_udisks->disks();
    for (int i = 0; i < disks.size(); ++i) {
        const Disk &d = disks[i];
        if (d.isSystem || d.isRaid || d.readOnly || (d.isLoop && !allowLoopDevicesForTest))
            continue;
        m_targets->addItem(tr("Disk %1: %2").arg(i).arg(diskTitle(d)), d.blockPath);
        if (d.blockPath == preferred)
            m_targets->setCurrentIndex(m_targets->count() - 1);
    }
    if (m_targets->count() == 0)
        m_targets->addItem(tr("No drive it can be done on (the drive the system runs from never is)"));
}

const Disk *BurnInDialog::target() const
{
    return m_udisks->diskByPath(m_targets->currentData().toString());
}

void BurnInDialog::updateState()
{
    const Disk *d = target();
    bool ok = d && !m_running;
    QString warning;
    if (d) {
        bool mounted = false;
        for (const Volume &v : d->volumes)
            mounted |= !v.mounts().isEmpty();
        const QString name = shortDevice(d->device);
        warning = QStringLiteral("<b>%1</b>").arg(tr("Everything on %1 (%2) will be erased.").arg(name, formatSize(d->size)).toHtmlEscaped());
        if (mounted)
            warning += QStringLiteral(" ") + tr("Some of it is open now: it's closed first.").toHtmlEscaped();
        if (d->health.state == Health::State::Unknown)
            warning += QStringLiteral("<br>") + tr("It doesn't report its health (through this USB adapter, say), so only the write and read "
                                                    "steps can run.").toHtmlEscaped();
        warning += QStringLiteral("<br>") + tr("Type <b>%1</b> to confirm:").arg(name.toHtmlEscaped());
        m_confirm->setPlaceholderText(name);
        ok = ok && m_confirm->text().trimmed() == name;
    }
    m_warning->setText(warning);
    m_confirm->setVisible(d && !m_running);
    m_start->setEnabled(ok);
}

void BurnInDialog::showSteps()
{
    m_steps->clear();
    for (int i = 0; i < burnin::kSteps; ++i) {
        const burnin::StepResult s = i < m_report.steps.size() ? m_report.steps[i] : burnin::StepResult{Step(i)};
        QString how;
        switch (s.outcome) {
        case Outcome::NotRun:
            how = i == m_step && m_running ? tr("Running…") : QString();
            break;
        case Outcome::Passed:
            how = tr("Passed");
            break;
        case Outcome::Failed:
            how = tr("Failed");
            break;
        case Outcome::Skipped:
            how = tr("Skipped");
            break;
        case Outcome::Stopped:
            how = tr("Stopped");
            break;
        }
        if (!s.detail.isEmpty())
            how += (how.isEmpty() ? QString() : QStringLiteral(": ")) + s.detail;
        auto *item = new QTreeWidgetItem({burnin::stepName(Step(i)), how});
        item->setToolTip(1, how);
        if (s.outcome == Outcome::Failed)
            item->setForeground(1, QColor(0xd0, 0x30, 0x40));
        m_steps->addTopLevelItem(item);
    }
    m_steps->resizeColumnToContents(0);
}

void BurnInDialog::reject()
{
    if (m_running) {
        if (QMessageBox::warning(this, windowTitle(), tr("Stop the burn-in? The drive is left partly written: make a new partition table on it before using it."),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes)
            return;
        m_stopping = true;
        if (m_checker)
            m_checker->cancel();
        if (m_step == int(Step::ShortTest) || m_step == int(Step::LongTest)) {
            if (const Disk *d = m_udisks->diskByPath(m_diskPath))
                m_udisks->smartSelftestAbort(*d);
            stepDone(Outcome::Stopped, QString());
        }
        return;
    }
    QDialog::reject();
}

void BurnInDialog::start()
{
    const Disk *d = target();
    if (!d)
        return;
    m_diskPath = d->blockPath;
    m_running = true;
    m_stopping = false;
    m_report = burnin::Report();
    m_report.drive = tr("%1 (%2), serial %3, %4").arg(d->model.isEmpty() ? shortDevice(d->device) : d->model, formatSize(d->size),
                                                      d->serial.isEmpty() ? tr("not known") : d->serial, shortDevice(d->device));
    m_report.started = QDateTime::currentDateTimeUtc();
    m_hasSmart = d->health.state != Health::State::Unknown && !d->health.smartOff;
    m_targets->setEnabled(false);
    m_twice->setEnabled(false);
    m_confirm->clear();
    m_result->clear();
    m_pdf->setEnabled(false);
    updateState();
    qCInfo(lcOps).noquote() << "Burn-in on" << d->device << d->model << d->serial << formatSize(d->size);
    m_step = -1;
    next();
}

void BurnInDialog::next()
{
    if (m_stopping) {
        finish();
        return;
    }
    ++m_step;
    if (m_step >= burnin::kSteps) {
        m_report.completed = true;
        finish();
        return;
    }
    burnin::StepResult s;
    s.step = Step(m_step);
    s.started = QDateTime::currentDateTimeUtc();
    m_report.steps << s;
    showSteps();
    const Disk *d = m_udisks->diskByPath(m_diskPath);
    if (!d) {
        stepDone(Outcome::Failed, tr("The drive is gone (unplugged?)."));
        m_stopping = true;
        return;
    }
    switch (Step(m_step)) {
    case Step::Before:
        if (!m_hasSmart)
            return stepDone(Outcome::Skipped, tr("it doesn't report its health"));
        m_before = m_udisks->smartAttributes(*d);
        return stepDone(Outcome::Passed, tr("%n counter(s) noted", nullptr, int(m_before.size())));
    case Step::ShortTest:
        return runSelftest(QStringLiteral("short"));
    case Step::Write:
        return runWrite();
    case Step::LongTest:
        return runSelftest(QStringLiteral("extended"));
    case Step::After: {
        if (!m_hasSmart)
            return stepDone(Outcome::Skipped, tr("it doesn't report its health"));
        m_udisks->smartUpdate(*d);
        // A moment for the drive to hand over fresh numbers.
        QTimer::singleShot(allowLoopDevicesForTest ? 0 : 3000, this, [this] {
            const Disk *now = m_udisks->diskByPath(m_diskPath);
            const QVector<SmartAttribute> after = now ? m_udisks->smartAttributes(*now) : QVector<SmartAttribute>();
            m_report.changes = burnin::compare(m_before, after);
            int bad = 0;
            for (const burnin::Change &c : std::as_const(m_report.changes))
                bad += c.weight == burnin::Change::Weight::Bad;
            stepDone(bad ? Outcome::Failed : Outcome::Passed,
                     m_report.changes.isEmpty() ? tr("nothing that matters went up") : tr("%n counter(s) went up", nullptr, int(m_report.changes.size())));
        });
        return;
    }
    }
}

void BurnInDialog::runSelftest(const QString &type)
{
    if (!m_hasSmart)
        return stepDone(Outcome::Skipped, tr("it doesn't report its health"));
    const Disk *d = m_udisks->diskByPath(m_diskPath);
#ifdef Q_OS_WIN
    if (d && d->health.nvme && !d->health.nvmeSelftest)
        return stepDone(Outcome::Skipped, tr("this drive doesn't do self-tests"));
#endif
    m_udisks->smartSelftest(*d, type);
    m_seenRunning = false;
    m_askedAt = QDateTime::currentMSecsSinceEpoch();
    m_phase->setText(type == QLatin1String("short") ? tr("The drive is testing itself (a few minutes)…")
                                                    : tr("The drive is testing itself, every part of it (hours on a big hard drive)…"));
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->show();
    m_poll->start();
}

void BurnInDialog::pollSelftest()
{
    const Disk *d = m_udisks->diskByPath(m_diskPath);
    if (!d) {
        m_poll->stop();
        m_stopping = true;
        return stepDone(Outcome::Failed, tr("The drive is gone (unplugged?)."));
    }
    m_udisks->refreshHealthQuietly(*d);
    const Health &h = d->health;
    if (h.selftestStatus == QLatin1String("inprogress")) {
        m_seenRunning = true;
        if (h.selftestPercentRemaining >= 0)
            m_progress->setValue(100 - h.selftestPercentRemaining);
        return;
    }
    if (!m_seenRunning && QDateTime::currentMSecsSinceEpoch() - m_askedAt < kStartWaitMs)
        return;
    m_poll->stop();
    m_progress->hide();
    m_phase->clear();
    const QString s = h.selftestStatus;
    if (s == QLatin1String("success"))
        return stepDone(Outcome::Passed, tr("no errors"));
    if (s == QLatin1String("aborted") || s == QLatin1String("interrupted"))
        return stepDone(Outcome::Stopped, tr("it was stopped before the end (the drive was reset or went to sleep)"));
    if (s.isEmpty())
        return stepDone(Outcome::Skipped, tr("the drive didn't say how it went"));
    stepDone(Outcome::Failed, tr("the drive found a problem (%1)").arg(s));
}

void BurnInDialog::runWrite()
{
    m_phase->setText(tr("Opening the drive…"));
    m_progress->show();
    const stickcheck::Mode mode = m_twice->isChecked() ? stickcheck::Mode::FullTwice : stickcheck::Mode::Full;
    openBlockThen(m_udisks, this, m_diskPath, UDisks::OpenMode::ReadWriteDirect, [this, mode](int fd) {
        if (fd < 0) {
            m_stopping = true;
            return stepDone(Outcome::Failed, tr("the drive couldn't be opened for writing"));
        }
        m_checker = new stickcheck::Checker(stickcheck::fromFd(fd), mode);
        connect(m_checker, &stickcheck::Checker::progress, this, [this](const QString &phase, quint64 done, quint64 total) {
            m_meter.update(phase, done, total);
        });
        connect(m_checker, &stickcheck::Checker::finished, this, [this](const stickcheck::Result &r) {
            m_thread->quit();
            m_thread->wait();
            m_thread = nullptr;
            m_checker = nullptr;
            writeDone(r);
        });
        m_thread = startOnThread(this, m_checker.data());
    });
}

void BurnInDialog::writeDone(const stickcheck::Result &r)
{
    m_progress->hide();
    m_phase->clear();
    m_report.writeErrors = quint64(r.writeErrors);
    m_report.readErrors = quint64(r.readErrors);
    m_report.wrongData = quint64(r.wrongData);
    m_report.fake = r.fake;
    if (!r.completed)
        return stepDone(Outcome::Stopped, r.error);
    if (r.fake) {
        m_stopping = true; // no point testing a fake further
        return stepDone(Outcome::Failed, tr("it holds only %1 of the %2 it says").arg(formatSize(r.real), formatSize(r.claimed)));
    }
    if (!r.good())
        return stepDone(Outcome::Failed, tr("%1 block(s) couldn't be written, %2 couldn't be read, %3 came back wrong")
                                              .arg(r.writeErrors)
                                              .arg(r.readErrors)
                                              .arg(r.wrongData));
    stepDone(Outcome::Passed, tr("all %1 hold what was written").arg(formatSize(r.claimed)));
}

void BurnInDialog::stepDone(Outcome outcome, const QString &detail)
{
    if (m_step < 0 || m_step >= m_report.steps.size())
        return;
    burnin::StepResult &s = m_report.steps[m_step];
    if (s.outcome != Outcome::NotRun)
        return; // already ended (stopped while it ran)
    s.outcome = outcome;
    s.detail = detail;
    s.finished = QDateTime::currentDateTimeUtc();
    qCInfo(lcOps).noquote() << "Burn-in:" << burnin::stepName(s.step) << int(outcome) << detail;
    if (outcome == Outcome::Stopped)
        m_stopping = true;
    showSteps();
    // Off this call's stack: a step can end inside the one that started it.
    QTimer::singleShot(0, this, &BurnInDialog::next);
}

void BurnInDialog::finish()
{
    m_poll->stop();
    m_running = false;
    m_report.finished = QDateTime::currentDateTimeUtc();
    m_progress->hide();
    m_phase->clear();
    m_targets->setEnabled(true);
    m_twice->setEnabled(true);
    showSteps();
    const burnin::Verdict v = burnin::verdict(m_report);
    QString text = burnin::verdictText(m_report);
    for (const burnin::Change &c : std::as_const(m_report.changes))
        text += QStringLiteral("\n• %1: %2 → %3. %4").arg(c.name).arg(c.before).arg(c.after).arg(c.meaning);
    if (v != burnin::Verdict::Failed || !m_report.fake)
        text += QStringLiteral("\n") + tr("The drive is blank now: make a partition table on it before use.");
    m_result->setText(text);
    m_pdf->setEnabled(true);
    m_udisks->refresh();
    updateState();
    emit ended();
}
