// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "windowsusbdialog.h"

#include "blockio.h"
#include "dialogs.h"
#include "format.h"
#include "hostprocess.h"
#include "isomode.h"
#include "isomount.h"
#include "windowsusbjob.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QVBoxLayout>

bool WindowsUsbDialog::allowLoopDevicesForTest = false;

QString WindowsUsbDialog::findIso()
{
    QFileInfo newest;
    for (const QString &folder : {QStandardPaths::writableLocation(QStandardPaths::DownloadLocation), QDir::homePath()}) {
        for (const QFileInfo &f : QDir(folder).entryInfoList({QStringLiteral("*.iso")}, QDir::Files, QDir::Time)) {
            const QString name = f.fileName().toLower();
            if ((name.startsWith(QLatin1String("win")) || name.contains(QLatin1String("windows")))
                && (!newest.exists() || f.lastModified() > newest.lastModified()))
                newest = f;
        }
    }
    return newest.exists() ? newest.absoluteFilePath() : QString();
}

WindowsUsbDialog::WindowsUsbDialog(UDisks *udisks, const QString &preferredDisk, QWidget *parent, const QString &iso)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_iso(new QLineEdit)
    , m_isoInfo(new QLabel)
    , m_targets(new QComboBox)
    , m_skipChecks(new QCheckBox(tr("Don't require TPM 2.0, Secure Boot or 4 GB of memory (Windows 11)")))
    , m_noAccount(new QCheckBox(tr("Don't require a Microsoft account")))
    , m_localUser(new QCheckBox(tr("Make a local account named:")))
    , m_userName(new QLineEdit)
    , m_privacy(new QCheckBox(tr("Skip the privacy questions (answer no to sending data)")))
    , m_region(new QCheckBox)
    , m_noBitLocker(new QCheckBox(tr("Don't turn on BitLocker device encryption by itself")))
    , m_warning(new QLabel)
    , m_confirm(new QLineEdit)
    , m_phase(new QLabel)
    , m_progress(new QProgressBar)
    , m_meter(m_progress, m_phase)
{
    setWindowTitle(tr("Make a Windows USB"));
    m_iso->setObjectName(QStringLiteral("iso"));
    m_isoInfo->setObjectName(QStringLiteral("isoInfo"));
    m_confirm->setObjectName(QStringLiteral("confirm"));
    m_userName->setObjectName(QStringLiteral("userName"));
    m_localUser->setObjectName(QStringLiteral("localUser"));
    m_isoInfo->setWordWrap(true);
    m_isoInfo->setTextFormat(Qt::RichText);
    m_warning->setWordWrap(true);
    m_warning->setTextFormat(Qt::RichText);
    m_phase->setWordWrap(true);
    m_progress->setRange(0, 1000);
    m_progress->setVisible(false);

    auto *browse = new QPushButton(tr("Browse…"));
    browse->setAutoDefault(false);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, tr("Choose the Windows ISO"),
                                                          QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
                                                          tr("ISO images (*.iso)"));
        if (!file.isEmpty())
            m_iso->setText(file);
    });
    auto *isoRow = new QHBoxLayout;
    isoRow->addWidget(m_iso, 1);
    isoRow->addWidget(browse);

    // The options, remembered from last time. The account name starts as this PC's.
    QSettings s(QStringLiteral("diskanvil"), QStringLiteral("diskanvil"));
    s.beginGroup(QStringLiteral("windowsusb"));
    m_skipChecks->setChecked(s.value(QStringLiteral("skipChecks"), true).toBool());
    m_noAccount->setChecked(s.value(QStringLiteral("noAccount"), true).toBool());
    m_localUser->setChecked(s.value(QStringLiteral("localUser"), false).toBool());
#ifdef Q_OS_WIN
    QString user = qEnvironmentVariable("USERNAME");
#else
    QString user = qEnvironmentVariable("USER");
#endif
    if (!user.isEmpty())
        user[0] = user[0].toUpper();
    m_userName->setText(s.value(QStringLiteral("userName"), user).toString());
    m_privacy->setChecked(s.value(QStringLiteral("privacy"), true).toBool());
    m_region->setChecked(s.value(QStringLiteral("region"), false).toBool());
    m_noBitLocker->setChecked(s.value(QStringLiteral("noBitLocker"), false).toBool());
    s.endGroup();
    windowsusb::fillRegionFromThisPc(m_region0);
    m_region->setText(tr("Region, keyboard and time zone like this PC (%1, %2)")
                          .arg(m_region0.userLocale, m_region0.timeZone.isEmpty() ? tr("time zone unknown") : m_region0.timeZone));

    auto *userRow = new QHBoxLayout;
    userRow->addWidget(m_localUser);
    userRow->addWidget(m_userName, 1);
    auto *optionsBox = new QGroupBox(tr("Windows setup"));
    auto *optionsLayout = new QVBoxLayout(optionsBox);
    optionsLayout->addWidget(m_skipChecks);
    optionsLayout->addWidget(m_noAccount);
    optionsLayout->addLayout(userRow);
    optionsLayout->addWidget(m_privacy);
    optionsLayout->addWidget(m_region);
    optionsLayout->addWidget(m_noBitLocker);

    auto *form = new QFormLayout;
    form->addRow(tr("Windows ISO:"), isoRow);
    form->addRow(QString(), m_isoInfo);
    form->addRow(tr("USB stick:"), m_targets);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("Makes a stick that installs Windows 10 or 11, from Microsoft's ISO. It starts UEFI PCs, "
                                       "with Secure Boot on or off. The options are answered for Windows Setup, so it doesn't "
                                       "ask them.")));
    layout->addLayout(form);
    layout->addWidget(optionsBox);
    layout->addWidget(m_warning);
    layout->addWidget(m_confirm);
    layout->addWidget(m_phase);
    layout->addWidget(m_progress);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    m_make = box->addButton(tr("Make the Windows USB"), QDialogButtonBox::ActionRole);
    m_make->setAutoDefault(false);
    box->button(QDialogButtonBox::Close)->setDefault(true);
    connect(m_make, &QPushButton::clicked, this, &WindowsUsbDialog::start);
    connect(box, &QDialogButtonBox::rejected, this, &WindowsUsbDialog::reject);
    layout->addWidget(box);

    fillTargets(preferredDisk);
    for (QCheckBox *c : {m_skipChecks, m_noAccount, m_localUser, m_privacy, m_region, m_noBitLocker})
        connect(c, &QCheckBox::toggled, this, &WindowsUsbDialog::updateState);
    connect(m_userName, &QLineEdit::textChanged, this, &WindowsUsbDialog::updateState);
    connect(m_targets, &QComboBox::currentIndexChanged, this, &WindowsUsbDialog::updateState);
    connect(m_confirm, &QLineEdit::textChanged, this, &WindowsUsbDialog::updateState);
    // The ISO is opened once the path has stopped changing, not on every key.
    m_openTimer.setSingleShot(true);
    m_openTimer.setInterval(400);
    connect(&m_openTimer, &QTimer::timeout, this, &WindowsUsbDialog::openIso);
    connect(m_iso, &QLineEdit::textChanged, &m_openTimer, qOverload<>(&QTimer::start));
    connect(m_udisks, &UDisks::changed, this, [this] {
        if (m_job)
            return;
        fillTargets(m_targets->currentData().toString());
        updateState();
    });
    m_iso->setText(iso.isEmpty() ? findIso() : iso);
    updateState();
    resize(660, sizeHint().height());
}

WindowsUsbDialog::~WindowsUsbDialog()
{
    delete m_job;
    // Gone without being closed (DiskAnvil quitting with it open, say): the ISO still gets
    // closed, by its IsoMount on its own, so the loop device doesn't stay behind.
    if (m_mount) {
        IsoMount *mount = m_mount;
        m_mount = nullptr;
        disconnect(mount, nullptr, this, nullptr);
        mount->setParent(m_udisks);
        connect(mount, &IsoMount::closed, mount, &QObject::deleteLater);
        mount->close();
    }
}

void WindowsUsbDialog::fillTargets(const QString &preferred)
{
    const QSignalBlocker block(m_targets);
    m_targets->clear();
    const QVector<Disk> &disks = m_udisks->disks();
    for (int i = 0; i < disks.size(); ++i) {
        const Disk &d = disks[i];
        if (d.isSystem || d.isRaid || d.readOnly)
            continue;
        // The ISO's own loop device is never a target.
        if (d.isLoop ? (!allowLoopDevicesForTest || d.backingFile == m_mounted || d.backingFile == m_iso->text().trimmed())
                     : !(d.removable || d.bus == QLatin1String("usb")))
            continue;
        m_targets->addItem(tr("Disk %1: %2").arg(i).arg(diskTitle(d)), d.blockPath);
        if (d.blockPath == preferred)
            m_targets->setCurrentIndex(m_targets->count() - 1);
    }
    if (m_targets->count() == 0)
        m_targets->addItem(tr("No USB stick (plug one in)"));
}

const Disk *WindowsUsbDialog::target() const
{
    return m_udisks->diskByPath(m_targets->currentData().toString());
}

void WindowsUsbDialog::openIso()
{
    const QString path = m_iso->text().trimmed();
    if (path == m_mounted && m_mount)
        return;
    m_info = {};
    m_isoError.clear();
    m_isoLabel.clear();
    auto open = [this, path] {
        m_mounted.clear();
        if (path.isEmpty() || !QFileInfo(path).isFile()) {
            m_isoError = path.isEmpty() ? QString() : tr("File not found");
            return updateState();
        }
        m_mount = new IsoMount(m_udisks, path, this);
        connect(m_mount, &IsoMount::ready, this, [this, path](const QString &root) {
            m_mounted = path;
            m_info = windowsusb::inspect(root);
            m_isoLabel = QStorageInfo(root).name();
            fillTargets(m_targets->currentData().toString());
            updateState();
        });
        connect(m_mount, &IsoMount::failed, this, [this](const QString &message, bool) {
            m_isoError = message;
            updateState();
        });
        updateState();
        m_mount->open();
    };
    closeIso(open);
}

void WindowsUsbDialog::closeIso(const std::function<void()> &then)
{
    if (!m_mount)
        return then();
    IsoMount *mount = m_mount;
    m_mount = nullptr;
    m_mounted.clear();
    connect(mount, &IsoMount::closed, this, [mount, then] {
        mount->deleteLater();
        then();
    });
    mount->close();
}

windowsusb::Options WindowsUsbDialog::options() const
{
    windowsusb::Options o;
    o.skipChecks = m_skipChecks->isChecked();
    o.noOnlineAccount = m_noAccount->isChecked();
    if (m_localUser->isChecked())
        o.localUser = windowsusb::userName(m_userName->text(), nullptr);
    o.skipPrivacy = m_privacy->isChecked();
    o.noBitLocker = m_noBitLocker->isChecked();
    if (m_region->isChecked()) {
        o.sameRegion = true;
        o.userLocale = m_region0.userLocale;
        o.inputLocale = m_region0.inputLocale;
        o.timeZone = m_region0.timeZone;
    }
    return o;
}

void WindowsUsbDialog::saveOptions() const
{
    QSettings s(QStringLiteral("diskanvil"), QStringLiteral("diskanvil"));
    s.beginGroup(QStringLiteral("windowsusb"));
    s.setValue(QStringLiteral("skipChecks"), m_skipChecks->isChecked());
    s.setValue(QStringLiteral("noAccount"), m_noAccount->isChecked());
    s.setValue(QStringLiteral("localUser"), m_localUser->isChecked());
    s.setValue(QStringLiteral("userName"), m_userName->text().trimmed());
    s.setValue(QStringLiteral("privacy"), m_privacy->isChecked());
    s.setValue(QStringLiteral("region"), m_region->isChecked());
    s.setValue(QStringLiteral("noBitLocker"), m_noBitLocker->isChecked());
}

void WindowsUsbDialog::updateState()
{
    if (m_job)
        return;
#ifdef Q_OS_WIN
    const bool wimlib = true; // Windows' own DISM splits it there
#else
    const bool wimlib = host::programExists(QStringLiteral("wimlib-imagex"));
#endif
    bool ok = !m_mounted.isEmpty() && m_info.windows;
    QString info;
    if (!m_isoError.isEmpty()) {
        info = redText(m_isoError);
    } else if (m_mount && m_mounted.isEmpty()) {
        info = tr("Looking inside…");
    } else if (!m_mounted.isEmpty() && !m_info.windows) {
        info = redText(tr("This isn't a Windows install ISO. For other ISOs, use Write Image to USB."));
    } else if (m_info.windows) {
        QString what = m_info.wim.name.isEmpty() ? tr("Windows") : m_info.wim.name;
        if (m_info.wim.images == 2)
            what = tr("%1 and one more edition").arg(what);
        else if (m_info.wim.images > 2)
            what = tr("%1 and %2 more editions").arg(what).arg(m_info.wim.images - 1);
        if (m_info.wim.build)
            what += tr(", build %1").arg(m_info.wim.build);
        if (!m_info.wim.arch.isEmpty())
            what += QStringLiteral(" (%1)").arg(m_info.wim.arch);
        info = what.toHtmlEscaped();
        if (m_info.needsSplit()) {
            info += QStringLiteral("<br>")
                  + tr("%1 is %2, too big for FAT32 in one piece, so it's split into parts Windows Setup reads as they are.")
                        .arg(m_info.install, formatSize(m_info.installSize))
                        .toHtmlEscaped();
            if (!wimlib) {
                info += QStringLiteral("<br>") + redText(tr("That needs wimlib, which isn't installed: it's called wimlib (Arch) or "
                                                            "wimtools (Debian, Ubuntu)."));
                ok = false;
            }
        }
    }
    m_isoInfo->setText(info);

    QString warning;
    m_userName->setEnabled(m_localUser->isChecked());
    if (m_localUser->isChecked()) {
        QString error;
        if (windowsusb::userName(m_userName->text(), &error).isEmpty()) {
            warning += redText(error) + QStringLiteral("<br>");
            ok = false;
        }
    }
    const Disk *d = target();
    bool asked = false;
    if (d && m_info.windows) {
        const quint64 needed = m_info.bytes + 64 * 1024 * 1024;
        if (blockio::pathIsOnDisk(QFileInfo(m_iso->text().trimmed()).absoluteFilePath(), d->device)) {
            warning += redText(tr("The ISO is on this stick. Pick another stick, or copy the ISO somewhere else first."));
            ok = false;
        } else if (d->size < needed) {
            warning += redText(tr("This stick is too small: it needs at least %1.").arg(formatSize(needed)));
            ok = false;
        } else {
            warning += redText(tr("Everything on %1 will be erased.").arg(diskTitle(*d)));
            if (!diskWarning(*d).isEmpty())
                warning += QStringLiteral("<br>") + redText(diskWarning(*d));
            const QString name = shortDevice(d->device);
            warning += QStringLiteral("<br>") + tr("Type <b>%1</b> to confirm:").arg(name.toHtmlEscaped());
            m_confirm->setPlaceholderText(name);
            ok = ok && m_confirm->text().trimmed() == name;
            asked = true;
        }
    }
    m_confirm->setVisible(asked);
    m_warning->setText(warning);
    m_make->setEnabled(ok && d);
}

void WindowsUsbDialog::reject()
{
    if (m_job) {
        if (!m_job->canCancel()) {
            QMessageBox::information(this, windowTitle(), tr("One moment: DiskAnvil is in the middle of setting up the stick."));
            return;
        }
        const auto answer = QMessageBox::warning(this, windowTitle(), tr("Stop? The stick won't install anything until it's made again."),
                                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes && m_job)
            m_job->cancel(); // done() runs when it has stopped
        return;
    }
    if (m_closing)
        return;
    m_closing = true;
    closeIso([this] { QDialog::reject(); });
}

void WindowsUsbDialog::start()
{
    const Disk *d = target();
    if (!d || !m_mount || m_mounted.isEmpty())
        return;
    saveOptions();
    for (QWidget *w : std::initializer_list<QWidget *>{m_iso, m_targets, m_skipChecks, m_noAccount, m_localUser, m_userName, m_privacy,
                                                        m_region, m_noBitLocker, m_confirm, m_make})
        w->setEnabled(false);
    m_progress->setRange(0, 0);
    m_progress->setVisible(true);
    m_job = new WindowsUsbJob(m_udisks, d->blockPath, m_mount->mountPoint(), isomode::fatLabel(m_isoLabel), m_info, options(), this);
    connect(m_job, &WindowsUsbJob::phase, this, [this](const QString &text) {
        m_progress->setRange(0, 0);
        m_phase->setText(text);
    });
    connect(m_job, &WindowsUsbJob::progress, this, [this](const QString &phase, quint64 done, quint64 total) {
        if (m_progress->maximum() == 0)
            m_progress->setRange(0, 1000);
        m_meter.update(phase, done, total);
    });
    connect(m_job, &WindowsUsbJob::finished, this, &WindowsUsbDialog::done);
    m_job->start();
}

void WindowsUsbDialog::done(bool ok, const QString &message, bool shownAlready)
{
    m_job->deleteLater();
    m_job = nullptr;
    m_progress->setVisible(false);
    m_udisks->refresh();
    if (ok) {
        m_phase->clear();
        QMessageBox::information(this, windowTitle(), message);
        m_closing = true;
        closeIso([this] { QDialog::accept(); });
        return;
    }
    if (!shownAlready)
        QMessageBox::warning(this, windowTitle(), message);
    m_phase->setText(redText(message));
    m_phase->setTextFormat(Qt::RichText);
    m_confirm->clear();
    for (QWidget *w : std::initializer_list<QWidget *>{m_iso, m_targets, m_skipChecks, m_noAccount, m_localUser, m_userName, m_privacy,
                                                        m_region, m_noBitLocker, m_confirm})
        w->setEnabled(true);
    updateState();
}
