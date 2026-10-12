// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "liveonpcdialog.h"

#include "applog.h"
#include "dialogs.h"
#include "efiboot.h"
#include "format.h"
#include "hostprocess.h"
#include "jobui.h"
#include "liveonpc.h"
#include "rescuestick.h"
#include "rescueusbdialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QThread>
#include <QVBoxLayout>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

LiveOnPcDialog::LiveOnPcDialog(UDisks *udisks, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_image(new QLineEdit)
    , m_imageInfo(new QLabel)
    , m_targets(new QComboBox)
    , m_plan(new QLabel)
    , m_progress(new QProgressBar)
    , m_phase(new QLabel)
{
    setWindowTitle(tr("Put DiskAnvil Live on This PC"));
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("DiskAnvil Live on a partition of this PC's own drive, and in its boot menu: the PC starts as it always "
                                "has, and DiskAnvil Live is there when it's needed, no USB stick. To start it, press the PC's boot menu "
                                "key while it starts (F12, F11, F9 or Esc) and pick DiskAnvil Live.\n\nIf the drive itself fails, it "
                                "goes with it: keep a DiskAnvil Live stick as well."));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    m_image->setObjectName(QStringLiteral("image"));
    m_image->setText(RescueUsbDialog::findImage());
    m_image->setPlaceholderText(QStringLiteral("diskanvil-live-%1.iso").arg(QStringLiteral(APP_VERSION)));
    auto *browse = new QPushButton(tr("Browse…"));
    browse->setAutoDefault(false);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString start = m_image->text().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
                                                        : QFileInfo(m_image->text()).path();
        const QString file = QFileDialog::getOpenFileName(this, tr("Choose the DiskAnvil Live Image"), start,
                                                          tr("DiskAnvil Live (diskanvil-live-*.iso diskforge-live-*.iso);;ISO images (*.iso)"));
        if (!file.isEmpty())
            m_image->setText(file);
    });
    auto *imageRow = new QHBoxLayout;
    imageRow->addWidget(m_image, 1);
    imageRow->addWidget(browse);
    m_imageInfo->setWordWrap(true);
    m_imageInfo->setTextFormat(Qt::RichText);
    m_plan->setObjectName(QStringLiteral("plan"));
    m_plan->setWordWrap(true);
    m_plan->setTextFormat(Qt::RichText);
    auto *form = new QFormLayout;
    form->addRow(tr("DiskAnvil Live:"), imageRow);
    form->addRow(QString(), m_imageInfo);
    form->addRow(tr("Drive:"), m_targets);
    layout->addLayout(form);
    layout->addWidget(m_plan);
    m_progress->setVisible(false);
    m_phase->setWordWrap(true);
    layout->addWidget(m_progress);
    layout->addWidget(m_phase);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    m_put = buttons->addButton(tr("Put It on This PC"), QDialogButtonBox::AcceptRole);
    m_update = buttons->addButton(tr("Update It"), QDialogButtonBox::ActionRole);
    m_takeOff = buttons->addButton(tr("Take It Off"), QDialogButtonBox::ActionRole);
    disconnect(buttons, &QDialogButtonBox::accepted, nullptr, nullptr);
    connect(m_put, &QPushButton::clicked, this, &LiveOnPcDialog::put);
    connect(m_update, &QPushButton::clicked, this, &LiveOnPcDialog::update);
    connect(m_takeOff, &QPushButton::clicked, this, &LiveOnPcDialog::takeOff);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_image, &QLineEdit::textChanged, this, [this] {
        inspectImage();
        updateState();
    });
    connect(m_targets, &QComboBox::currentIndexChanged, this, &LiveOnPcDialog::updateState);
    connect(m_udisks, &UDisks::changed, this, [this] {
        if (m_running)
            return;
        fillTargets();
        updateState();
    });
    fillTargets();
    inspectImage();
    updateState();
    resize(640, sizeHint().height());
}

LiveOnPcDialog::~LiveOnPcDialog()
{
    if (m_writer)
        m_writer->cancel();
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
}

bool LiveOnPcDialog::uefi() const
{
    return allowLoopDevicesForTest || efiboot::read().uefi;
}

void LiveOnPcDialog::fillTargets()
{
    const QString current = m_targets->currentData().toString();
    const QSignalBlocker block(m_targets);
    m_targets->clear();
    const QVector<Disk> &disks = m_udisks->disks();
    const Disk *already = nullptr;
    liveonpc::installed(disks, &already);
    for (int i = 0; i < disks.size(); ++i) {
        const Disk &d = disks[i];
        if (d.isLoop ? !allowLoopDevicesForTest || d.tableType != QLatin1String("gpt") : !liveonpc::suits(d))
            continue;
        const liveonpc::Place place = liveonpc::placeOn(d, rescue::partitionSize(m_imageBytes));
        QString text = tr("Disk %1: %2").arg(i).arg(diskTitle(d));
        if (&d == already)
            text += tr(", DiskAnvil Live is on it");
        else if (place.freeBytes > 0)
            text += tr(", %1 free").arg(formatSize(place.freeBytes));
        m_targets->addItem(text, d.blockPath);
        if (d.blockPath == current || (current.isEmpty() && &d == already))
            m_targets->setCurrentIndex(m_targets->count() - 1);
    }
    if (m_targets->count() == 0)
        m_targets->addItem(tr("No drive inside this PC to put it on"));
}

const Disk *LiveOnPcDialog::target() const
{
    return m_udisks->diskByPath(m_targets->currentData().toString());
}

void LiveOnPcDialog::inspectImage()
{
    const QString path = m_image->text().trimmed();
    // The first time too, when there's no image yet: that says where to get one.
    if (m_inspectedOnce && path == m_inspectedPath)
        return;
    m_inspectedOnce = true;
    m_inspectedPath = path;
    m_imageBytes = 0;
    m_imageError.clear();
    if (path.isEmpty()) {
        m_imageError = tr("Choose the DiskAnvil Live ISO. It's on the DiskAnvil releases page on GitHub.");
        m_imageInfo->setText(m_imageError.toHtmlEscaped());
        return;
    }
    const rescue::Image image = rescue::inspect(path);
    if (!image.error.isEmpty()) {
        m_imageError = image.error;
        m_imageInfo->setText(redText(image.error));
        return;
    }
    m_imageBytes = image.bytes;
    m_imageInfo->setText(tr("DiskAnvil Live %1, built %2 (%3)").arg(image.info.version, image.info.built, formatSize(image.bytes)).toHtmlEscaped());
    fillTargets();
}

void LiveOnPcDialog::updateState()
{
    if (m_running)
        return;
    const Disk *d = target();
    const Disk *on = nullptr;
    const Volume *already = liveonpc::installed(m_udisks->disks(), &on);
    const bool here = already && d && on == d;
    QString plan;
    bool canPut = false;
    if (!uefi()) {
        plan = redText(tr("This PC starts the old way (BIOS), which only ever starts one program from a drive: DiskAnvil Live can't "
                          "join its boot menu. Use a DiskAnvil Live stick."));
    } else if (!m_imageError.isEmpty()) {
        plan = redText(m_imageError);
    } else if (here) {
        plan = tr("DiskAnvil Live is on this drive (%1, %2). Update It puts this version there and keeps its logs; Take It Off removes it "
                  "from the boot menu and gives the space back.")
                   .arg(shortDevice(already->device), formatSize(already->size))
                   .toHtmlEscaped();
    } else if (already) {
        plan = tr("DiskAnvil Live is on another drive of this PC already (%1). Take it off there first.").arg(shortDevice(already->device)).toHtmlEscaped();
    } else if (d) {
        const liveonpc::Place place = liveonpc::placeOn(*d, rescue::partitionSize(m_imageBytes));
        if (!place.ok()) {
            plan = redText(place.why);
        } else {
            plan = tr("A %1 partition is made at the end of the free space on %2, DiskAnvil Live is copied onto it, and \"DiskAnvil "
                      "Live\" goes last in the PC's boot menu. Nothing else on the drive changes, and the PC still starts as it does now.")
                       .arg(formatSize(place.size), diskTitle(*d))
                       .toHtmlEscaped();
            canPut = true;
        }
    }
    m_plan->setText(plan);
    m_put->setVisible(!here);
    m_put->setEnabled(canPut);
    m_update->setVisible(here);
    m_update->setEnabled(here && m_imageError.isEmpty() && uefi());
    m_takeOff->setVisible(here);
}

void LiveOnPcDialog::reject()
{
    if (m_running) {
        QMessageBox::information(this, windowTitle(), tr("One moment: DiskAnvil is in the middle of it. It's safe to wait."));
        return;
    }
    QDialog::reject();
}

void LiveOnPcDialog::put()
{
    const Disk *d = target();
    if (!d)
        return;
    const liveonpc::Place place = liveonpc::placeOn(*d, rescue::partitionSize(m_imageBytes));
    if (!place.ok())
        return;
    if (QMessageBox::question(this, windowTitle(),
                              tr("Make a %1 partition in the free space on %2 and put DiskAnvil Live in this PC's boot menu?")
                                  .arg(formatSize(place.size), diskTitle(*d)))
        != QMessageBox::Yes)
        return;
    m_running = true;
    m_diskPath = d->blockPath;
    m_put->setEnabled(false);
    m_progress->setVisible(true);
    m_progress->setRange(0, 0);
    m_phase->setText(tr("Making the partition…"));
    qCInfo(lcOps).noquote() << "Put DiskAnvil Live on This PC:" << d->device << "at" << place.offset << "size" << place.size;
    const quint64 offset = place.offset;
    m_waitingFailed = connect(m_udisks, &UDisks::operationFinished, this, [this, offset](bool ok, const QString &message) {
        disconnect(m_waitingFailed);
        if (!ok)
            return finish(false, message, true);
        m_udisks->refresh();
        const Disk *disk = m_udisks->diskByPath(m_diskPath);
        for (const Volume &v : disk ? disk->volumes : QVector<Volume>()) {
            if (v.offset >= offset && v.offset < offset + 1024 * 1024 && v.label == liveonpc::kLabel)
                m_volumePath = v.objectPath;
        }
        if (m_volumePath.isEmpty())
            return finish(false, tr("The new partition didn't show up."));
        whenMounted(m_volumePath, [this](const QString &root) { copyInto(root, true); });
    });
    m_udisks->createPartition(*d, place.offset, place.size, QStringLiteral("vfat"), liveonpc::kLabel, {}, true);
}

void LiveOnPcDialog::update()
{
    const Disk *on = nullptr;
    const Volume *v = liveonpc::installed(m_udisks->disks(), &on);
    if (!v || !on)
        return;
    if (v->size < m_imageBytes + 64ull * 1024 * 1024) {
        QMessageBox::warning(this, windowTitle(), tr("DiskAnvil Live's partition is too small for this version: take it off, then put it on again."));
        return;
    }
    m_running = true;
    m_diskPath = on->blockPath;
    m_volumePath = v->objectPath;
    m_update->setEnabled(false);
    m_progress->setVisible(true);
    m_progress->setRange(0, 0);
    qCInfo(lcOps).noquote() << "Update DiskAnvil Live on This PC:" << v->device;
    whenMounted(m_volumePath, [this](const QString &root) { copyInto(root, false); });
}

void LiveOnPcDialog::whenMounted(const QString &volumePath, const std::function<void(const QString &root)> &then)
{
    const auto mountOf = [this, volumePath]() -> QString {
        for (const Disk &d : m_udisks->disks()) {
            for (const Volume &v : d.volumes) {
                if (v.objectPath == volumePath)
                    return v.mounts().value(0);
            }
        }
        return {};
    };
    if (const QString root = mountOf(); !root.isEmpty())
        return then(root);
    m_phase->setText(tr("Mounting it…"));
    m_waiting = connect(m_udisks, &UDisks::changed, this, [this, mountOf, then] {
        const QString root = mountOf();
        if (root.isEmpty())
            return;
        disconnect(m_waiting);
        disconnect(m_waitingFailed);
        then(root);
    });
    m_waitingFailed = connect(m_udisks, &UDisks::operationFinished, this, [this](bool ok, const QString &message) {
        if (ok)
            return;
        disconnect(m_waiting);
        disconnect(m_waitingFailed);
        finish(false, message, true);
    });
    for (const Disk &d : m_udisks->disks()) {
        for (const Volume &v : d.volumes) {
            if (v.objectPath == volumePath)
                return m_udisks->mount(v);
        }
    }
}

void LiveOnPcDialog::copyInto(const QString &root, bool fresh)
{
    // An update: what's there goes, but for its logs.
    if (!fresh) {
        for (const QFileInfo &f : QDir(root).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
            if (f.fileName().compare(QLatin1String("logs"), Qt::CaseInsensitive) == 0 || f.fileName() == QLatin1String("diskanvil-live"))
                continue;
            if (f.isDir() && !f.isSymLink())
                QDir(f.absoluteFilePath()).removeRecursively();
            else
                QFile::remove(f.absoluteFilePath());
        }
    }
    m_progress->setRange(0, 1000);
    auto *meter = new PhaseProgress(m_progress, m_phase);
    m_writer = new rescue::StickWriter(m_inspectedPath, root);
    connect(m_writer, &rescue::StickWriter::progress, this, [meter](const QString &phase, quint64 done, quint64 total) { meter->update(phase, done, total); });
    connect(m_writer, &rescue::StickWriter::finished, this, [this, meter, fresh](bool ok, const QString &message) {
        delete meter;
        m_thread->quit();
        m_thread->wait();
        m_thread = nullptr;
        m_writer = nullptr;
        m_progress->setRange(0, 0);
        for (const Disk &d : m_udisks->disks()) {
            for (const Volume &v : d.volumes) {
                if (v.objectPath == m_volumePath)
                    m_udisks->unmount(v, true);
            }
        }
        if (!ok)
            return finish(false, message);
        // An update keeps its boot menu entry.
        if (!fresh)
            return finish(true, tr("DiskAnvil Live on this PC is updated."));
        addEntry();
    });
    m_thread = startOnThread(this, m_writer);
}

bool LiveOnPcDialog::runAsRoot(const QString &script, const QStringList &args, QString *said)
{
    if (rootForTest)
        return rootForTest(script, args, said);
#ifdef Q_OS_WIN
    Q_UNUSED(script)
    Q_UNUSED(args)
    *said = tr("not on Windows");
    return false;
#else
    const QStringList bash = QStringList{QStringLiteral("bash"), QStringLiteral("-c"), script, QStringLiteral("diskanvil")} + args;
    const QStringList command = rescue::runningInRescue() ? QStringList{QStringLiteral("sudo"), QStringLiteral("-n")} + bash
                              : ::geteuid() == 0          ? bash
                                                          : host::command(QStringList{QStringLiteral("pkexec")} + bash);
    QProcess run;
    run.setProcessChannelMode(QProcess::MergedChannels);
    run.start(command.first(), command.mid(1));
    const bool ok = run.waitForFinished(120000) && run.exitStatus() == QProcess::NormalExit && run.exitCode() == 0;
    *said = QString::fromLocal8Bit(run.readAll()).trimmed();
    return ok;
#endif
}

void LiveOnPcDialog::addEntry()
{
    m_phase->setText(tr("Adding it to the PC's boot menu…"));
    const Disk *d = m_udisks->diskByPath(m_diskPath);
    const Volume *v = nullptr;
    for (const Volume &x : d ? d->volumes : QVector<Volume>()) {
        if (x.objectPath == m_volumePath)
            v = &x;
    }
    if (!d || !v)
        return finish(false, tr("DiskAnvil Live's partition went away."));
    QString said;
    bool ok = false;
#ifdef Q_OS_WIN
    if (!rootForTest) {
        // Written straight into the firmware's variables: the old entry of that name out, the new
        // one last in the order.
        const efiboot::State state = efiboot::read();
        QVector<int> order = state.order;
        ok = true;
        for (const efi::BootEntry &e : state.entries) {
            if (e.description == liveonpc::kEntry) {
                ok = ok && efi::setVariable(QStringLiteral("Boot") + efiboot::hex4(e.number), QByteArray(), &said);
                order.removeAll(e.number);
            }
        }
        int free = 0;
        while (free < 0xFFFF && efi::variable(QStringLiteral("Boot") + efiboot::hex4(free)))
            ++free;
        const quint64 sector = quint64(std::max(d->sectorSize, 512));
        const QByteArray option = efiboot::loadOption(liveonpc::kEntry, quint32(v->number), v->offset / sector, v->size / sector, v->partUuid, liveonpc::kLoader);
        ok = ok && efi::setVariable(QStringLiteral("Boot") + efiboot::hex4(free), option, &said)
             && efi::setVariable(QStringLiteral("BootOrder"), efiboot::orderValue(liveonpc::orderWithLast(order, free)), &said);
        if (ok)
            said = QStringLiteral("Boot") + efiboot::hex4(free);
    } else
#endif
        ok = runAsRoot(liveonpc::addScript(), {d->device, QString::number(v->number)}, &said);
    qCInfo(lcOps).noquote() << "Put DiskAnvil Live on This PC: boot menu entry" << (ok ? "added" : "not added") << said;
    if (!ok)
        return finish(false, tr("DiskAnvil Live is on the drive, but the PC's boot menu couldn't be changed: %1\n\nBoot Menu Entries (Tools) can "
                                "add it by hand: \\EFI\\BOOT\\BOOTX64.EFI on the DISKANVIL partition.")
                                 .arg(said.isEmpty() ? tr("no reason given") : said.right(500)));
    finish(true, tr("DiskAnvil Live is on this PC.\n\nTo start it, press the PC's boot menu key while it starts (usually F12, F11, F9 or Esc) "
                    "and pick \"DiskAnvil Live\". The PC starts as before otherwise."));
}

void LiveOnPcDialog::takeOff()
{
    const Disk *on = nullptr;
    const Volume *v = liveonpc::installed(m_udisks->disks(), &on);
    if (!v || !on)
        return;
    if (QMessageBox::question(this, windowTitle(),
                              tr("Take DiskAnvil Live off this PC? Its boot menu entry goes, and its partition (%1, with the logs on it) is "
                                 "deleted, leaving free space.")
                                  .arg(formatSize(v->size)))
        != QMessageBox::Yes)
        return;
    m_running = true;
    QString said;
    bool ok = true;
#ifdef Q_OS_WIN
    if (!rootForTest) {
        const efiboot::State state = efiboot::read();
        QVector<int> order = state.order;
        for (const efi::BootEntry &e : state.entries) {
            if (e.description == liveonpc::kEntry) {
                ok = ok && efi::setVariable(QStringLiteral("Boot") + efiboot::hex4(e.number), QByteArray(), &said);
                order.removeAll(e.number);
            }
        }
        ok = ok && efi::setVariable(QStringLiteral("BootOrder"), efiboot::orderValue(order), &said);
    } else
#endif
        ok = !uefi() || runAsRoot(liveonpc::removeScript(), {}, &said);
    qCInfo(lcOps).noquote() << "Take DiskAnvil Live off This PC: boot menu entry" << (ok ? "removed" : "not removed") << said;
    if (!ok)
        return finish(false, tr("The boot menu entry couldn't be taken out, so the partition stays: %1").arg(said.right(500)));
    m_waitingFailed = connect(m_udisks, &UDisks::operationFinished, this, [this](bool done, const QString &message) {
        disconnect(m_waitingFailed);
        if (!done)
            return finish(false, message, true);
        finish(true, tr("DiskAnvil Live is off this PC; its space is free again."));
    });
    m_udisks->deletePartition(*v, true);
}

void LiveOnPcDialog::finish(bool ok, const QString &message, bool alreadyShown)
{
    m_running = false;
    m_progress->setVisible(false);
    m_udisks->refresh();
    if (ok) {
        m_phase->clear();
        QMessageBox::information(this, windowTitle(), message);
    } else {
        qCInfo(lcOps).noquote() << "Put DiskAnvil Live on This PC failed:" << message;
        if (!alreadyShown)
            QMessageBox::warning(this, windowTitle(), message);
        m_phase->setText(redText(message));
        m_phase->setTextFormat(Qt::RichText);
    }
    fillTargets();
    updateState();
}
