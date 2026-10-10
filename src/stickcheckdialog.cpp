// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "stickcheckdialog.h"

#include "applog.h"
#include "blockmapwidget.h"
#include "dialogs.h"
#include "format.h"
#include "theme.h"
#include "usbprep.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QThread>
#include <QVBoxLayout>

namespace {

constexpr quint64 kMiB = 1024 * 1024;

QString coloured(Theme::Role role, const QString &text)
{
    return QStringLiteral("<span style=\"color:%1\"><b>%2</b></span>").arg(Theme::instance().html(role), text.toHtmlEscaped());
}

} // namespace

bool StickCheckDialog::allowLoopDevicesForTest = false;

StickCheckDialog::StickCheckDialog(UDisks *udisks, const QString &preferredDisk, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_targets(new QComboBox)
    , m_quick(new QRadioButton)
    , m_full(new QRadioButton)
    , m_twice(new QRadioButton)
    , m_warning(new QLabel)
    , m_confirm(new QLineEdit)
    , m_map(new BlockMapWidget)
    , m_phase(new QLabel)
    , m_progress(new QProgressBar)
    , m_result(new QLabel)
    , m_meter(m_progress, m_phase)
{
    setWindowTitle(tr("Check a USB Stick"));
    m_confirm->setObjectName(QStringLiteral("confirm"));
    m_result->setObjectName(QStringLiteral("result"));
    m_quick->setObjectName(QStringLiteral("quick"));
    m_full->setObjectName(QStringLiteral("full"));
    m_twice->setObjectName(QStringLiteral("twice"));
    m_quick->setChecked(true);
    auto *modes = new QButtonGroup(this);
    for (QRadioButton *b : {m_quick, m_full, m_twice})
        modes->addButton(b);
    m_warning->setWordWrap(true);
    m_warning->setTextFormat(Qt::RichText);
    m_result->setWordWrap(true);
    m_result->setTextFormat(Qt::RichText);
    m_phase->setWordWrap(true);
    m_progress->setRange(0, 1000);
    m_progress->setVisible(false);
    m_map->setVisible(false);
    m_map->setLabels(tr("Checked, fine"), tr("Written, not checked yet"), tr("Wrong data or errors"), tr("Not written yet"));

    auto *form = new QFormLayout;
    form->addRow(tr("USB stick:"), m_targets);
    auto *modeBox = new QVBoxLayout;
    modeBox->addWidget(m_quick);
    modeBox->addWidget(m_full);
    modeBox->addWidget(m_twice);
    form->addRow(tr("Check:"), modeBox);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("Writes to the whole stick and reads it back, to find bad spots and fake sticks: ones "
                                       "that say they're bigger than they are, and quietly lose what goes past their real "
                                       "size. Works for memory cards too.")));
    layout->addLayout(form);
    layout->addWidget(m_warning);
    layout->addWidget(m_confirm);
    layout->addWidget(m_map, 1);
    layout->addWidget(m_phase);
    layout->addWidget(m_progress);
    layout->addWidget(m_result);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    m_start = box->addButton(tr("Start the Check"), QDialogButtonBox::ActionRole);
    m_safe = box->addButton(tr("Make It Safe to Use"), QDialogButtonBox::ActionRole);
    m_start->setAutoDefault(false);
    m_safe->setAutoDefault(false);
    m_safe->setVisible(false);
    box->button(QDialogButtonBox::Close)->setDefault(true);
    connect(m_start, &QPushButton::clicked, this, &StickCheckDialog::start);
    connect(m_safe, &QPushButton::clicked, this, &StickCheckDialog::makePartition);
    connect(box, &QDialogButtonBox::rejected, this, &StickCheckDialog::reject);
    layout->addWidget(box);

    fillTargets(preferredDisk);
    connect(m_targets, &QComboBox::currentIndexChanged, this, &StickCheckDialog::updateState);
    connect(m_confirm, &QLineEdit::textChanged, this, &StickCheckDialog::updateState);
    connect(modes, &QButtonGroup::buttonToggled, this, &StickCheckDialog::updateState);
    connect(m_udisks, &UDisks::changed, this, [this] {
        if (m_running || m_prep)
            return;
        fillTargets(m_targets->currentData().toString());
        updateState();
    });
    updateState();
    resize(640, sizeHint().height());
}

StickCheckDialog::~StickCheckDialog()
{
    if (m_thread) {
        if (m_checker)
            m_checker->cancel();
        m_thread->quit();
        m_thread->wait();
    }
}

void StickCheckDialog::fillTargets(const QString &preferred)
{
    const QSignalBlocker block(m_targets);
    m_targets->clear();
    const QVector<Disk> &disks = m_udisks->disks();
    for (int i = 0; i < disks.size(); ++i) {
        const Disk &d = disks[i];
        if (d.isSystem || d.isRaid || d.readOnly)
            continue;
        if (d.isLoop ? !allowLoopDevicesForTest : !(d.removable || d.bus == QLatin1String("usb")))
            continue;
        m_targets->addItem(tr("Disk %1: %2").arg(i).arg(diskTitle(d)), d.blockPath);
        if (d.blockPath == preferred)
            m_targets->setCurrentIndex(m_targets->count() - 1);
    }
    if (m_targets->count() == 0)
        m_targets->addItem(tr("No USB stick or memory card (plug one in)"));
}

const Disk *StickCheckDialog::target() const
{
    return m_udisks->diskByPath(m_targets->currentData().toString());
}

stickcheck::Mode StickCheckDialog::mode() const
{
    if (m_twice->isChecked())
        return stickcheck::Mode::FullTwice;
    return m_full->isChecked() ? stickcheck::Mode::Full : stickcheck::Mode::Quick;
}

void StickCheckDialog::updateState()
{
    if (m_running)
        return;
    const Disk *d = target();
    // How long each takes, once there's a stick to work it out for.
    const auto time = [d](stickcheck::Mode mode) {
        return d ? QStringLiteral(", ") + durationText(stickcheck::estimate(mode, d->size)) : QString();
    };
    m_quick->setText(tr("Quick: a stamp every few MB%1. Finds most fake sticks").arg(time(stickcheck::Mode::Quick)));
    m_full->setText(tr("Full: every byte%1. Finds everything").arg(time(stickcheck::Mode::Full)));
    m_twice->setText(tr("Full, twice: every bit both ways%1").arg(time(stickcheck::Mode::FullTwice)));

    bool ok = d != nullptr;
    QString warning;
    if (d) {
        warning = redText(tr("Everything on %1 will be erased.").arg(diskTitle(*d)));
        if (!diskWarning(*d).isEmpty())
            warning += QStringLiteral("<br>") + redText(diskWarning(*d));
        const QString name = shortDevice(d->device);
        warning += QStringLiteral("<br>") + tr("Type <b>%1</b> to confirm:").arg(name.toHtmlEscaped());
        m_confirm->setPlaceholderText(name);
        ok = m_confirm->text().trimmed() == name;
    }
    m_warning->setText(warning);
    m_confirm->setVisible(d != nullptr);
    m_start->setEnabled(ok);
}

void StickCheckDialog::reject()
{
    if (m_prep) {
        QMessageBox::information(this, windowTitle(), tr("One moment: DiskForge is in the middle of making the partition."));
        return;
    }
    if (m_running) {
        const auto answer = QMessageBox::warning(this, windowTitle(),
                                                 tr("Stop the check? What it wrote stays on the stick, so make a new partition "
                                                    "before using it."),
                                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes && m_checker)
            m_checker->cancel(); // showResult() runs when it stops
        return;
    }
    QDialog::reject();
}

void StickCheckDialog::start()
{
    const Disk *d = target();
    if (!d)
        return;
    m_diskPath = d->blockPath;
    m_running = true;
    m_safe->setVisible(false);
    m_result->clear();
    for (QWidget *w : std::initializer_list<QWidget *>{m_targets, m_quick, m_full, m_twice, m_confirm, m_start})
        w->setEnabled(false);
    m_map->reset(d->size, false);
    m_map->setVisible(true);
    m_progress->setValue(0);
    m_progress->setVisible(true);
    m_phase->setText(tr("Opening the stick…"));
    adjustSize();
    keepOnScreen(this);
    qCInfo(lcOps).noquote() << "Check a USB Stick on" << d->device << d->model << formatSize(d->size);

    const stickcheck::Mode chosen = mode();
    openBlockThen(m_udisks, this, m_diskPath, UDisks::OpenMode::ReadWriteDirect, [this, chosen](int fd) {
        if (fd < 0) {
            stickcheck::Result r;
            r.error = tr("Couldn't open the stick.");
            return showResult(r);
        }
        m_checker = new stickcheck::Checker(stickcheck::fromFd(fd), chosen);
        connect(m_checker, &stickcheck::Checker::progress, this, [this](const QString &phase, quint64 done, quint64 total) {
            m_meter.update(phase, done, total);
        });
        connect(m_checker, &stickcheck::Checker::marks, this, [this](const QVector<stickcheck::Mark> &marks) {
            for (const stickcheck::Mark &m : marks) {
                if (m.state == stickcheck::Mark::Good)
                    m_map->data().markChecked(m.offset, m.length);
                else
                    m_map->data().mark(m.offset, m.length, m.state == stickcheck::Mark::Bad ? BlockMapData::State::Bad : BlockMapData::State::Pending);
            }
            m_map->refresh();
        });
        connect(m_checker, &stickcheck::Checker::finished, this, [this](const stickcheck::Result &r) {
            m_thread->quit();
            m_thread->wait();
            m_thread = nullptr;
            m_checker = nullptr;
            showResult(r);
        });
        m_thread = startOnThread(this, m_checker);
    });
}

void StickCheckDialog::showResult(const stickcheck::Result &r)
{
    m_running = false;
    m_last = r;
    m_progress->setVisible(false);
    m_phase->clear();
    for (QWidget *w : std::initializer_list<QWidget *>{m_targets, m_quick, m_full, m_twice, m_confirm})
        w->setEnabled(true);
    m_confirm->clear();
    updateState();
    m_udisks->refresh();

    QString text;
    if (!r.completed) {
        text = coloured(Theme::Role::Warning, r.error.isEmpty() ? tr("Stopped.") : r.error);
        if (r.real) // it got as far as writing
            text += QStringLiteral("<br>") + tr("What was written so far is still on the stick, so make a new partition before using it.");
    } else if (r.fake) {
        text = coloured(Theme::Role::Danger, tr("This stick is fake: it says it holds %1, but it really holds %2.")
                                                 .arg(formatSize(r.claimed), formatSize(r.real)));
        text += QStringLiteral("<br>")
              + (r.wraps ? tr("Anything written past %1 lands on top of what's at the start, so files go bad without a "
                              "warning.").arg(formatSize(r.real))
                         : tr("Anything written past %1 is lost without a warning.").arg(formatSize(r.real)));
        text += QStringLiteral("<br>") + tr("Make It Safe to Use makes one partition that ends at %1, so nothing goes past it. "
                                           "If you paid for %2, it's worth asking for your money back.")
                                            .arg(formatSize(r.real), formatSize(r.claimed));
    } else if (r.good()) {
        text = coloured(Theme::Role::Good, tr("No problems found: all %1 hold what was written.").arg(formatSize(r.claimed)));
        if (mode() == stickcheck::Mode::Quick)
            text += QStringLiteral("<br>") + tr("A full check also finds single bad spots between the stamps.");
        text += QStringLiteral("<br>") + tr("The check overwrote the stick: Format It makes a new partition.");
    } else {
        text = coloured(Theme::Role::Danger, tr("This stick has problems: %1 block(s) couldn't be written, %2 couldn't be read "
                                                "and %3 came back wrong.")
                                                 .arg(r.writeErrors)
                                                 .arg(r.readErrors)
                                                 .arg(r.wrongData));
        text += QStringLiteral("<br>") + tr("It's failing. Don't keep anything you care about on it.");
    }
    m_result->setText(text);
    m_safe->setText(r.fake ? tr("Make It Safe to Use") : tr("Format It"));
    m_safe->setVisible(r.completed || r.real);
    m_safe->setEnabled(m_udisks->diskByPath(m_diskPath) != nullptr);
    fitHeight(this);
}

void StickCheckDialog::makePartition()
{
    const Disk *d = m_udisks->diskByPath(m_diskPath);
    if (!d)
        return;
    const quint64 size = m_last.fake ? m_last.real : d->size;
    if (!askPlain(this, windowTitle(),
                  tr("Make one partition on %1, %2 big? It takes a minute and erases the stick again.").arg(diskTitle(*d), formatSize(size))))
        return;
    // FAT32 is what everything reads; past 32 GB, exFAT is the usual choice.
    const bool exfat = size > 32ULL * 1000 * 1000 * 1000 && m_udisks->filesystem(QStringLiteral("exfat"))
                       && m_udisks->filesystem(QStringLiteral("exfat"))->available;
    UsbPrep::Partition p{exfat ? QStringLiteral("exfat") : QStringLiteral("vfat"), QStringLiteral("USB"), 0, 0, false};
    // The partition starts 1 MiB in; ending it at the real size keeps everything inside.
    if (m_last.fake)
        p.size = size > 2 * kMiB ? size - kMiB : size;
    m_safe->setEnabled(false);
    m_progress->setRange(0, 0);
    m_progress->setVisible(true);
    m_prep = new UsbPrep(m_udisks, m_diskPath, QStringLiteral("dos"), {p}, this);
    connect(m_prep, &UsbPrep::phase, m_phase, &QLabel::setText);
    auto finish = [this](bool ok, const QString &message, bool shownAlready) {
        m_prep->deleteLater();
        m_prep = nullptr;
        m_progress->setVisible(false);
        m_phase->clear();
        m_safe->setEnabled(true);
        m_udisks->refresh();
        if (ok)
            QMessageBox::information(this, windowTitle(), message);
        else if (!shownAlready)
            QMessageBox::warning(this, windowTitle(), message);
    };
    connect(m_prep, &UsbPrep::failed, this, [finish](const QString &message, bool shownAlready) { finish(false, message, shownAlready); });
    connect(m_prep, &UsbPrep::ready, this, [this] { m_prep->finish(); });
    connect(m_prep, &UsbPrep::done, this, [this, finish, size] {
        finish(true, tr("Done: the stick has one %1 partition and is ready to use.").arg(formatSize(size)), false);
    });
    m_prep->start();
}
