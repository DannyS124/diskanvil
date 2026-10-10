// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "erasedialog.h"

#include "applog.h"
#include "dialogs.h"
#include "format.h"
#include "jobui.h"
#include "rescuestick.h"
#include "theme.h"

#include <QClipboard>
#ifdef Q_OS_WIN
#include <windows.h>
#include <powrprof.h>
#else
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#endif
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// The rescan for a PC where DiskForge doesn't run anything as root itself.
const QString kRescanCommand = QStringLiteral("echo \"- - -\" | sudo tee /sys/class/scsi_host/host*/scan");

// A drive's connector edge: the small data socket, and the wide power socket with an arrow
// pulling its plug out. No words in it, so it needs no translating.
const char kUnplugPicture[] = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="600" height="190" viewBox="0 0 600 190">
<rect x="20" y="40" width="560" height="70" rx="8" fill="#9aa6ba" stroke="#6c7890" stroke-width="3"/>
<rect x="20" y="96" width="560" height="14" fill="#13623f"/>
<g transform="translate(110 58)"><path d="M0 0 H90 V28 H14 V18 H0 Z" fill="#1d2433"/>
<g fill="#c8962c"><rect x="20" y="9" width="6" height="12"/><rect x="31" y="9" width="6" height="12"/><rect x="42" y="9" width="6" height="12"/><rect x="53" y="9" width="6" height="12"/><rect x="64" y="9" width="6" height="12"/><rect x="75" y="9" width="6" height="12"/></g></g>
<g transform="translate(240 58)"><path d="M0 0 H210 V28 H14 V18 H0 Z" fill="#1d2433"/>
<g fill="#c8962c"><rect x="20" y="9" width="7" height="12"/><rect x="32" y="9" width="7" height="12"/><rect x="44" y="9" width="7" height="12"/><rect x="56" y="9" width="7" height="12"/><rect x="68" y="9" width="7" height="12"/><rect x="80" y="9" width="7" height="12"/><rect x="92" y="9" width="7" height="12"/><rect x="104" y="9" width="7" height="12"/><rect x="116" y="9" width="7" height="12"/><rect x="128" y="9" width="7" height="12"/><rect x="140" y="9" width="7" height="12"/><rect x="152" y="9" width="7" height="12"/><rect x="164" y="9" width="7" height="12"/><rect x="176" y="9" width="7" height="12"/><rect x="188" y="9" width="7" height="12"/></g></g>
<rect x="232" y="50" width="226" height="44" rx="6" fill="none" stroke="#00b8d9" stroke-width="4"/>
<rect x="290" y="128" width="110" height="40" rx="5" fill="#1d2433"/>
<rect x="303" y="168" width="7" height="22" fill="#f2c230"/><rect x="318" y="168" width="7" height="22" fill="#20242c"/><rect x="333" y="168" width="7" height="22" fill="#e23b3b"/><rect x="348" y="168" width="7" height="22" fill="#20242c"/><rect x="363" y="168" width="7" height="22" fill="#f08a2c"/>
<path d="M480 74 C 540 90, 540 140, 420 148" fill="none" stroke="#00b8d9" stroke-width="5" stroke-linecap="round"/>
<path d="M432 136 L414 148 L432 160" fill="none" stroke="#00b8d9" stroke-width="5" stroke-linecap="round" stroke-linejoin="round"/>
</svg>)svg";

} // namespace

SecureEraseDialog::SecureEraseDialog(UDisks *udisks, const QString &blockPath, int diskNumber, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_blockPath(blockPath)
    , m_intro(new QLabel)
    , m_first(new QRadioButton)
    , m_second(new QRadioButton)
    , m_frozen(new QLabel)
    , m_howTo(new QLabel)
    , m_sleep(new QPushButton(tr("Sleep Now")))
    , m_checkAgain(new QPushButton(tr("Check Again")))
    , m_warning(new QLabel)
    , m_confirm(new QLineEdit)
    , m_replug(new QPushButton(tr("Unplug and Replug…")))
    , m_replugBox(new QFrame)
    , m_replugPicture(new QLabel)
    , m_replugText(new QLabel)
    , m_replugCommand(new QLabel(kRescanCommand))
    , m_replugCopy(new QPushButton(tr("Copy")))
    , m_replugNext(new QPushButton)
    , m_replugCancel(new QPushButton(tr("Stop")))
    , m_replugTimer(new QTimer(this))
{
    setWindowTitle(tr("Secure Erase Disk %1").arg(diskNumber));
    const Disk *d = udisks->diskByPath(blockPath);
    m_ata = !d || !d->nvmeNamespace;
    for (QLabel *l : {m_intro, m_frozen, m_warning, m_replugText}) {
        l->setWordWrap(true);
        l->setTextFormat(Qt::RichText);
    }
    m_first->setChecked(true);
    if (d) {
        m_intro->setText(tr("<p><b>%1</b></p><p>Tells the drive itself to erase everything on it, including spare areas that "
                            "Wipe Disk can't reach. It's the right way to clear an SSD before selling or giving it away.</p>")
                             .arg(diskTitle(*d).toHtmlEscaped()));
        if (m_ata) {
            m_first->setText(tr("Normal erase (%1)").arg(durationText(d->ataEraseMinutes * 60.0)));
            m_second->setText(tr("Enhanced erase (%1): also overwrites areas the drive set aside").arg(durationText(d->ataEnhancedEraseMinutes * 60.0)));
            m_second->setVisible(d->ataEnhancedEraseMinutes > 0);
            if (d->ataEraseMinutes <= 0 && d->ataEnhancedEraseMinutes > 0)
                m_second->setChecked(true);
            m_first->setVisible(d->ataEraseMinutes > 0);
        } else {
            m_first->setText(tr("Erase all data"));
            m_second->setText(tr("Crypto erase: instant, if the drive encrypts what it stores (many do)"));
        }
    }
    m_frozen->setText(redText(tr("The drive is frozen: the PC locks erasing when it starts, and the drive refuses until it's had its "
                                 "power taken away and given back. Unplug and Replug walks you through doing that by hand. Sleep "
                                 "Now does it too, on PCs that wake up fine from sleep.")));
    m_replug->setObjectName(QStringLiteral("replug"));
    m_howTo->setText(QStringLiteral("<a href=\"https://github.com/DannyS124/diskforge/blob/main/docs/SECURE-ERASE.md\">%1</a>")
                         .arg(tr("How this works, step by step with pictures").toHtmlEscaped()));
    m_howTo->setOpenExternalLinks(true);
    m_howTo->setObjectName(QStringLiteral("howTo"));

    auto *frozenRow = new QHBoxLayout;
    frozenRow->addStretch();
    frozenRow->addWidget(m_replug);
    frozenRow->addWidget(m_sleep);
    frozenRow->addWidget(m_checkAgain);

    // Unplug and Replug: the steps, which plug to pull, and on a PC the rescan to run.
    m_replugBox->setObjectName(QStringLiteral("replugBox"));
    m_replugBox->setFrameShape(QFrame::StyledPanel);
    m_replugText->setObjectName(QStringLiteral("replugText"));
    QImage picture;
    if (picture.loadFromData(QByteArray(kUnplugPicture), "SVG"))
        m_replugPicture->setPixmap(QPixmap::fromImage(picture.scaledToWidth(360, Qt::SmoothTransformation)));
    m_replugPicture->setAlignment(Qt::AlignCenter);
    m_replugCommand->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_replugCommand->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_replugCommand->setWordWrap(true);
    auto *commandRow = new QHBoxLayout;
    commandRow->addWidget(m_replugCommand, 1);
    commandRow->addWidget(m_replugCopy);
    auto *replugButtons = new QHBoxLayout;
    replugButtons->addStretch();
    replugButtons->addWidget(m_replugCancel);
    replugButtons->addWidget(m_replugNext);
    auto *replugLayout = new QVBoxLayout(m_replugBox);
    replugLayout->addWidget(m_replugPicture);
    replugLayout->addWidget(m_replugText);
    replugLayout->addLayout(commandRow);
    replugLayout->addLayout(replugButtons);
    m_replugBox->hide();
    m_replugTimer->setInterval(3000);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_intro);
    layout->addWidget(m_first);
    layout->addWidget(m_second);
    layout->addWidget(m_frozen);
    layout->addLayout(frozenRow);
    layout->addWidget(m_replugBox);
    layout->addWidget(m_howTo);
    layout->addWidget(m_warning);
    layout->addWidget(m_confirm);
    layout->addWidget(dialogButtons(this, tr("Erase"), &m_erase));

    connect(m_confirm, &QLineEdit::textChanged, this, &SecureEraseDialog::updateState);
    connect(m_sleep, &QPushButton::clicked, this, [this] {
        // Some PCs don't wake up under DiskForge Live (graphics, or the stick losing power while
        // asleep) and hang until they're switched off.
        if (rescue::runningInRescue()) {
            QString text = tr("Some PCs don't wake up properly from sleep under DiskForge Live, and freeze until they're switched "
                              "off. Unplug and Replug does the same job without sleeping.");
            if (rescue::runningFromStick())
                text += QStringLiteral("\n\n") + tr("It's also running from the USB stick, and many PCs cut the power to USB while "
                                                    "they sleep. \"Copy to memory\" in the boot menu avoids that part.");
            QMessageBox box(QMessageBox::Warning, windowTitle(), text, QMessageBox::Cancel, this);
            box.addButton(tr("Sleep Anyway"), QMessageBox::AcceptRole);
            box.setDefaultButton(QMessageBox::Cancel);
            if (box.exec() == QMessageBox::Cancel)
                return;
        }
#ifdef Q_OS_WIN
        SetSuspendState(FALSE, FALSE, FALSE);
#else
        QDBusMessage sleep = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"),
                                                            QStringLiteral("org.freedesktop.login1.Manager"), QStringLiteral("Suspend"));
        sleep << true; // let polkit ask if it needs to
        QDBusConnection::systemBus().asyncCall(sleep);
#endif
    });
    // The frozen flag is read when the drive is probed; re-reading the drive refreshes it.
    connect(m_checkAgain, &QPushButton::clicked, this, [this] {
        m_udisks->rescan(m_blockPath);
        m_udisks->refresh();
        updateState();
    });
    connect(m_replug, &QPushButton::clicked, this, [this] {
        const Disk *d = m_udisks->diskByPath(m_blockPath);
        if (!d)
            return;
        m_serial = d->serial;
        m_model = d->model;
        m_size = d->size;
        m_foundAs.clear();
        setReplugStep(Replug::Preparing);
        const QString name = shortDevice(d->device);
        QPointer<SecureEraseDialog> self(this);
        m_udisks->prepareUnplug(*d, [self, name](bool ready) {
            if (!self)
                return;
            if (!ready)
                return self->setReplugStep(Replug::Off);
            // On DiskForge Live the system lets go of the drive too, so it's gone cleanly.
            if (rescue::canRescanDrives())
                rescue::rescanDrives({QStringLiteral("detach"), name}, self, [self](bool) {
                    if (self)
                        self->setReplugStep(Replug::Unplug);
                });
            else
                self->setReplugStep(Replug::Unplug);
        });
    });
    connect(m_replugNext, &QPushButton::clicked, this, [this] { setReplugStep(Replug::Replug); });
    connect(m_replugCancel, &QPushButton::clicked, this, [this] { setReplugStep(Replug::Off); });
    connect(m_replugCopy, &QPushButton::clicked, this, [] { QGuiApplication::clipboard()->setText(kRescanCommand); });
    connect(m_replugTimer, &QTimer::timeout, this, &SecureEraseDialog::lookForDrive);
    connect(m_udisks, &UDisks::changed, this, &SecureEraseDialog::lookForDrive);
    connect(m_udisks, &UDisks::changed, this, &SecureEraseDialog::updateState);
    updateState();
    resize(560, sizeHint().height());
}

void SecureEraseDialog::showReplugStepForPreview(int step)
{
    if (const Disk *d = m_udisks->diskByPath(m_blockPath)) {
        m_serial = d->serial;
        m_model = d->model;
        m_size = d->size;
    }
    setReplugStep(step == 1 ? Replug::Unplug : step == 2 ? Replug::Replug : Replug::NotBack);
    m_replugTimer->stop();
}

void SecureEraseDialog::setReplugStep(Replug step)
{
    m_step = step;
    m_ticks = 0;
    if (step == Replug::Replug)
        m_replugWaited.start();
    if (step == Replug::Unplug || step == Replug::Replug)
        m_replugTimer->start();
    else
        m_replugTimer->stop();
    m_replugNext->setVisible(step == Replug::Unplug || step == Replug::NotBack);
    m_replugNext->setText(step == Replug::NotBack ? tr("Keep Looking") : tr("Done, It's Unplugged"));
    m_replugNext->setDefault(m_replugNext->isVisible());
    m_replugPicture->setVisible(!m_replugPicture->pixmap().isNull() && step == Replug::Unplug);
    m_replugText->setText(replugText());
    updateState();
}

QString SecureEraseDialog::replugText() const
{
    const QStringList steps = {tr("Get the drive ready: unmount it and spin it down"), tr("Unplug its power cable"),
                               tr("Plug it back in"), tr("Find it again and check it isn't frozen")};
    int current = 0;
    QString now;
    switch (m_step) {
    case Replug::Off:
        return {};
    case Replug::Preparing:
        now = tr("Unmounting and spinning it down…");
        break;
    case Replug::Unplug:
        current = 1;
        now = tr("Unplug the drive's <b>power</b> cable now: the wide plug in the picture. Leave the thin data cable in. "
                 "Then click Done.");
        break;
    case Replug::Replug: {
        current = 2;
        const int waited = int(m_replugWaited.elapsed() / 1000);
        now = tr("Wait a few seconds, then plug the power cable back in, straight and firmly. DiskForge is looking for the "
                 "drive (%1 s).").arg(waited);
        if (!rescue::canRescanDrives() && waited >= 20)
            now += QStringLiteral("<br><br>") + tr("Not showing up? Some PCs don't notice a drive plugged in while they're on. "
                                                   "Run this in a terminal, then wait a moment:");
        break;
    }
    case Replug::NotBack:
        current = 2;
        now = tr("The drive didn't come back. Check that the power plug is all the way in. Some PCs only notice a drive "
                 "plugged in later with <b>Hot Plug</b> turned on for that port in the BIOS setup (and SATA Mode set to "
                 "AHCI). Or use Wipe Disk, which needs none of this.");
        break;
    }
    const Theme &theme = Theme::instance();
    QString html;
    for (int i = 0; i < steps.size(); ++i) {
        if (i < current)
            html += QStringLiteral("<span style=\"color:%1\">✓ %2</span><br>").arg(theme.html(Theme::Role::Good), steps[i].toHtmlEscaped());
        else if (i == current)
            html += QStringLiteral("<b>▶ %1</b><br>").arg(steps[i].toHtmlEscaped());
        else
            html += QStringLiteral("<span style=\"color:%1\">%2. %3</span><br>").arg(theme.html(Theme::Role::Muted)).arg(i + 1).arg(steps[i].toHtmlEscaped());
    }
    return html + QStringLiteral("<br>") + now;
}

void SecureEraseDialog::lookForDrive()
{
    if (m_step != Replug::Unplug && m_step != Replug::Replug)
        return;
    const bool tick = sender() == m_replugTimer;
    if (tick)
        ++m_ticks;
    const QVector<Disk> &disks = m_udisks->disks();
    const int found = UDisks::findDrive(disks, m_serial, m_model, m_size);
    if (m_step == Replug::Unplug) {
        // On PCs that notice, the drive leaving means it's been unplugged.
        if (found < 0)
            setReplugStep(Replug::Replug);
        return;
    }
    if (found >= 0 && !disks[found].ataFrozen) {
        m_blockPath = disks[found].blockPath;
        m_foundAs = shortDevice(disks[found].device);
        qCInfo(lcOps).noquote() << "Secure Erase: the drive came back as" << m_foundAs << "and isn't frozen";
        return setReplugStep(Replug::Off);
    }
    if (tick && m_ticks % 2 == 0) {
        // Every 6 s: on DiskForge Live, ask the controllers to look again (old ones don't notice by
        // themselves); a drive that's there but still looks frozen gets re-read, since what's
        // known about it can be from before it lost power.
        if (rescue::canRescanDrives())
            rescue::rescanDrives({QStringLiteral("scan")}, this);
        if (found >= 0)
            m_udisks->rescan(disks[found].blockPath);
    }
    if (m_replugWaited.elapsed() > 120000)
        return setReplugStep(Replug::NotBack);
    if (tick)
        m_replugText->setText(replugText());
    updateState();
}

UDisks::EraseMethod SecureEraseDialog::method() const
{
    if (m_ata)
        return m_second->isChecked() ? UDisks::EraseMethod::AtaEnhanced : UDisks::EraseMethod::AtaNormal;
    return m_second->isChecked() ? UDisks::EraseMethod::NvmeCrypto : UDisks::EraseMethod::NvmeUserData;
}

void SecureEraseDialog::updateState()
{
    const Disk *d = m_udisks->diskByPath(m_blockPath);
    const bool replugging = m_step != Replug::Off;
    const bool frozen = d && m_ata && d->ataFrozen;
    m_frozen->setVisible(frozen && !replugging);
    m_replug->setVisible(frozen && !replugging && d->bus != QLatin1String("usb"));
    m_sleep->setVisible(frozen && !replugging);
    m_checkAgain->setVisible(frozen && !replugging);
    m_replugBox->setVisible(replugging);
    m_howTo->setVisible(frozen || replugging);
    const bool showCommand = m_step == Replug::Replug && !rescue::canRescanDrives() && m_replugWaited.elapsed() >= 20000;
    m_replugCommand->setVisible(showCommand);
    m_replugCopy->setVisible(showCommand);
    bool ok = d && !frozen && !d->isSystem && !replugging;
    if (d && d->isSystem) {
        m_warning->setText(redText(tr("%1 runs this system, so it can't be erased while it's in use. Erase it from a live USB.")
                                       .arg(shortDevice(d->device))));
    } else if (d) {
        QString text;
        if (!m_foundAs.isEmpty())
            text = QStringLiteral("<span style=\"color:%1\"><b>%2</b></span><br>")
                       .arg(Theme::instance().html(Theme::Role::Good),
                            tr("Found it again as %1, and it isn't frozen anymore.").arg(m_foundAs).toHtmlEscaped());
        text += redText(tr("Everything on %1 will be erased, for good.").arg(diskTitle(*d)));
        if (!diskWarning(*d).isEmpty())
            text += QStringLiteral("<br>") + redText(diskWarning(*d));
        if (m_ata && d->bus == QLatin1String("usb"))
            text += QStringLiteral("<br>") + warningText(tr("It's connected through USB: most USB adapters don't pass Secure Erase through "
                                                          "to the drive, so it usually fails. A SATA port inside a PC works, and Wipe "
                                                          "Disk works through any adapter."));
        // UDisks sets the ATA password "xxxx" for the erase; an interrupted erase leaves it set.
        text += QStringLiteral("<br>")
              + (m_ata ? tr("Once it starts it can't be stopped: the drive does the erasing itself. Keep the PC on and the drive "
                            "plugged in until it's done. If the erase is cut off, the drive stays locked with the password "
                            "<b>xxxx</b> until it's unlocked (see Help).")
                       : tr("Once it starts it can't be stopped: the drive does the erasing itself. Keep the PC on until it's done."));
        const QString name = shortDevice(d->device);
        text += QStringLiteral("<br>") + tr("Type <b>%1</b> to confirm:").arg(name);
        m_confirm->setPlaceholderText(name);
        ok = ok && m_confirm->text().trimmed() == name;
        m_warning->setText(text);
    }
    m_warning->setVisible(d && !frozen && !replugging);
    m_confirm->setVisible(d && !frozen && !d->isSystem && !replugging);
    m_erase->setEnabled(ok);
    fitHeight(this);
}
