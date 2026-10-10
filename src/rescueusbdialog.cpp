// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "rescueusbdialog.h"

#include "applog.h"
#include "blockio.h"
#include "dialogs.h"
#include "format.h"
#include "usbprep.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <unistd.h>
#include <utility>

namespace {

const QString kLabel = QStringLiteral("DISKFORGE");
// Sticks made while it was still called DiskForge Rescue, then Bluespark.
const QStringList kOldLabels = {QStringLiteral("DFRESCUE"), QStringLiteral("BLUESPARK")};
constexpr quint64 kRoomForLogs = 64 * 1024 * 1024;

} // namespace

bool RescueUsbDialog::allowLoopDevicesForTest = false;
std::function<int(const QString &)> RescueUsbDialog::openStickForTest;

QString RescueUsbDialog::findImage()
{
    // Inside DiskForge Live: the stick it's running from, the same build there's no ISO of.
    // After Copy to Memory it's not at /run/live/medium, but it can be mounted like any stick.
    if (rescue::runningInRescue()) {
        const QString running = QStringLiteral("/run/live/medium");
        if (rescue::stickInfo(running).valid())
            return running;
        const QString id = rescue::runningBuildId();
        for (const QStorageInfo &mount : QStorageInfo::mountedVolumes()) {
            if (!id.isEmpty() && rescue::stickInfo(mount.rootPath()).id == id)
                return mount.rootPath();
        }
    }
    // Downloads first; the build folder's rescue/out too, for running DiskForge from the source tree.
    QStringList folders = {QStandardPaths::writableLocation(QStandardPaths::DownloadLocation), QDir::homePath(),
                           QCoreApplication::applicationDirPath() + QStringLiteral("/../rescue/out")};
    QFileInfo newest;
    for (const QString &folder : folders) {
        const QFileInfoList found = QDir(folder).entryInfoList({QStringLiteral("diskforge-live-*.iso"), QStringLiteral("bluespark-*.iso"), QStringLiteral("diskforge-rescue-*.iso")}, QDir::Files, QDir::Time);
        if (!found.isEmpty() && (!newest.exists() || found.first().lastModified() > newest.lastModified()))
            newest = found.first();
    }
    return newest.exists() ? newest.canonicalFilePath() : QString();
}

RescueUsbDialog::RescueUsbDialog(UDisks *udisks, const QString &preferredDisk, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_image(new QLineEdit)
    , m_imageInfo(new QLabel)
    , m_targets(new QComboBox)
    , m_stickInfo(new QLabel)
    , m_openLogs(new QPushButton(tr("Open Logs")))
    , m_warning(new QLabel)
    , m_confirm(new QLineEdit)
    , m_bios(new QCheckBox(tr("Also start old PCs without UEFI (BIOS)")))
    , m_progress(new QProgressBar)
    , m_phase(new QLabel)
    , m_meter(m_progress, m_phase)
{
    setWindowTitle(tr("Make a DiskForge Live USB"));
    m_image->setObjectName(QStringLiteral("image"));
    m_imageInfo->setObjectName(QStringLiteral("imageInfo"));
    m_stickInfo->setObjectName(QStringLiteral("stickInfo"));
    m_confirm->setObjectName(QStringLiteral("confirm"));
    m_targets->setObjectName(QStringLiteral("targets"));
    m_warning->setObjectName(QStringLiteral("warning"));
    auto *browse = new QPushButton(tr("Browse…"));
    browse->setAutoDefault(false);
    m_openLogs->setAutoDefault(false);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString start = m_image->text().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
                                                        : QFileInfo(m_image->text()).path();
        const QString file = QFileDialog::getOpenFileName(this, tr("Choose the DiskForge Live Image"), start,
                                                          tr("DiskForge Live (diskforge-live-*.iso bluespark-*.iso diskforge-rescue-*.iso);;ISO images (*.iso)"));
        if (!file.isEmpty())
            m_image->setText(file);
    });
    auto *imageRow = new QHBoxLayout;
    imageRow->addWidget(m_image, 1);
    imageRow->addWidget(browse);
    m_image->setPlaceholderText(QStringLiteral("diskforge-live-%1.iso").arg(QStringLiteral(APP_VERSION)));
    m_imageInfo->setWordWrap(true);
    m_imageInfo->setTextFormat(Qt::RichText);
    auto *stickRow = new QHBoxLayout;
    stickRow->addWidget(m_stickInfo, 1);
    stickRow->addWidget(m_openLogs);
    m_stickInfo->setWordWrap(true);
    m_stickInfo->setTextFormat(Qt::PlainText);
    m_warning->setWordWrap(true);
    m_warning->setTextFormat(Qt::RichText);
    m_phase->setWordWrap(true);
    m_progress->setRange(0, 1000);
    m_progress->setVisible(false);

    auto *form = new QFormLayout;
    form->addRow(tr("DiskForge Live image:"), imageRow);
    form->addRow(QString(), m_imageInfo);
    form->addRow(tr("USB stick:"), m_targets);
    form->addRow(QString(), stickRow);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("DiskForge Live starts any PC from a USB stick, with DiskForge and other repair tools, "
                                       "even when the PC's own system won't start. The stick stays readable on any PC, and "
                                       "the rescue system keeps its logs on it.")));
    layout->addLayout(form);
    m_bios->setChecked(true);
    m_bios->setObjectName(QStringLiteral("bios"));
    m_bios->setToolTip(tr("Puts GRUB's boot code in front of the stick's partition, so PCs from before UEFI start from it too. "
                          "That's a direct write to the stick, so on a normal PC it asks for the admin password once."));
    layout->addWidget(m_bios);
    layout->addWidget(m_warning);
    layout->addWidget(m_confirm);
    layout->addWidget(m_phase);
    layout->addWidget(m_progress);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    m_make = box->addButton(tr("Make the DiskForge Live USB"), QDialogButtonBox::ActionRole);
    m_make->setAutoDefault(false);
    box->button(QDialogButtonBox::Close)->setDefault(true);
    connect(m_make, &QPushButton::clicked, this, &RescueUsbDialog::start);
    connect(box, &QDialogButtonBox::rejected, this, &RescueUsbDialog::reject);
    connect(m_openLogs, &QPushButton::clicked, this, &RescueUsbDialog::openLogs);
    layout->addWidget(box);

    fillTargets(preferredDisk);
    m_image->setText(findImage());
    inspectImage();
    connect(m_image, &QLineEdit::textChanged, this, [this] {
        inspectImage();
        updateState();
    });
    connect(m_targets, &QComboBox::currentIndexChanged, this, &RescueUsbDialog::updateState);
    connect(m_confirm, &QLineEdit::textChanged, this, &RescueUsbDialog::updateState);
    // Sticks plugged in (or mounted) while the dialog is open.
    connect(m_udisks, &UDisks::changed, this, [this] {
        if (m_running)
            return;
        if (m_image->text().isEmpty())
            m_image->setText(findImage()); // the rescue stick, mounted after Copy to Memory
        fillTargets(m_targets->currentData().toString());
        updateState();
    });
    updateState();
    resize(640, sizeHint().height());
}

RescueUsbDialog::~RescueUsbDialog()
{
    if (m_thread) {
        if (m_writer)
            m_writer->cancel();
        m_thread->quit();
        m_thread->wait();
    }
}

void RescueUsbDialog::fillTargets(const QString &preferred)
{
    const QSignalBlocker block(m_targets);
    m_targets->clear();
    const QVector<Disk> &disks = m_udisks->disks();
    for (int i = 0; i < disks.size(); ++i) {
        const Disk &d = disks[i];
        // USB and removable drives only, like Write Image to USB.
        if (d.isSystem || d.isRaid)
            continue;
        if (d.isLoop ? !allowLoopDevicesForTest : !(d.removable || d.bus == QLatin1String("usb")))
            continue;
        m_targets->addItem(tr("Disk %1: %2").arg(i).arg(diskTitle(d)), d.blockPath);
        if (d.blockPath == preferred)
            m_targets->setCurrentIndex(m_targets->count() - 1);
    }
    if (m_targets->count() == 0)
        m_targets->addItem(tr("No USB stick (plug one in)"));
}

const Disk *RescueUsbDialog::target() const
{
    return m_udisks->diskByPath(m_targets->currentData().toString());
}

const Volume *RescueUsbDialog::rescueVolume(const Disk &disk) const
{
    for (const Volume &v : disk.volumes) {
        if (v.fsType == QLatin1String("vfat") && (v.label == kLabel || kOldLabels.contains(v.label)))
            return &v;
    }
    return nullptr;
}

void RescueUsbDialog::inspectImage()
{
    const QString path = m_image->text().trimmed();
    if (path == m_inspectedPath)
        return;
    m_inspectedPath = path;
    m_inspected = rescue::Image();
    if (path.isEmpty()) {
        m_inspected.error = rescue::runningInRescue()
            ? tr("After Copy to Memory the rescue stick isn't mounted: mount its %1 partition and it shows up here. Or choose the DiskForge Live ISO.").arg(kLabel)
            : tr("Choose the DiskForge Live ISO. It's on the DiskForge releases page on GitHub.");
        m_imageInfo->setText(m_inspected.error.toHtmlEscaped());
        return;
    }
    // A folder is a running rescue stick (see findImage).
    if (!QFileInfo(path).isFile() && !QFileInfo(path).isDir()) {
        m_inspected.error = tr("File not found");
        m_imageInfo->setText(redText(m_inspected.error));
        return;
    }
    m_inspected = rescue::inspect(path);
    if (!m_inspected.error.isEmpty()) {
        m_imageInfo->setText(redText(m_inspected.error));
        return;
    }
    const QString what = QFileInfo(path).isDir() ? tr("A copy of the rescue stick DiskForge is running from: DiskForge Live %1, built %2 (%3)")
                                                 : tr("DiskForge Live %1, built %2 (%3)");
    m_imageInfo->setText(what.arg(m_inspected.info.version, m_inspected.info.built, formatSize(m_inspected.bytes)).toHtmlEscaped());
}

void RescueUsbDialog::updateState()
{
    if (m_running)
        return;
    const Disk *d = target();
    const Volume *existing = d ? rescueVolume(*d) : nullptr;
    m_openLogs->setVisible(existing != nullptr);
    if (existing && !existing->mounts().isEmpty()) {
        const QString root = existing->mounts().first();
        const rescue::Info info = rescue::stickInfo(root);
        const int boots = rescue::logFolders(root);
        QString text = tr("This stick looks like a DiskForge Live stick.");
        if (info.valid() && boots == 0)
            text = tr("This stick has DiskForge Live %1 on it, with no logs yet.").arg(info.version);
        else if (info.valid() && boots == 1)
            text = tr("This stick has DiskForge Live %1 on it, with logs from one start.").arg(info.version);
        else if (info.valid())
            text = tr("This stick has DiskForge Live %1 on it, with logs from %2 starts.").arg(info.version).arg(boots);
        m_stickInfo->setText(text);
    } else {
        m_stickInfo->setText(existing ? tr("This stick has DiskForge Live on it. Open Logs shows what it saved.") : QString());
    }
    m_stickInfo->setVisible(existing != nullptr);

    bool ok = d && m_inspected.error.isEmpty();
    QString warning;
    // Copying a rescue stick: not onto itself.
    bool isSource = false;
    bool asked = false;
    if (d && QFileInfo(m_inspectedPath).isDir()) {
        const QString source = QFileInfo(m_inspectedPath).canonicalFilePath();
        for (const Volume &v : d->volumes) {
            for (const QString &mp : v.mounts())
                isSource = isSource || QFileInfo(mp).canonicalFilePath() == source;
        }
    }
    if (isSource) {
        warning = redText(tr("That's the stick being copied. Plug in another one."));
        ok = false;
    } else if (d && QFileInfo(m_inspectedPath).isFile() && blockio::pathIsOnDisk(QFileInfo(m_inspectedPath).absoluteFilePath(), d->device)) {
        warning = redText(tr("The ISO is on this stick. Pick another stick, or copy the ISO somewhere else first."));
        ok = false;
    } else if (d && m_inspected.error.isEmpty()) {
        const quint64 needed = m_inspected.bytes + kRoomForLogs;
        if (d->size < needed) {
            warning = redText(tr("This stick is too small: DiskForge Live needs at least %1.").arg(formatSize(needed)));
            ok = false;
        } else {
            warning = redText(tr("Everything on %1 will be erased.").arg(diskTitle(*d)));
            if (existing)
                warning += QStringLiteral(" ") + redText(tr("That includes its logs: open them first if you need them."));
            if (!diskWarning(*d).isEmpty())
                warning += QStringLiteral("<br>") + redText(diskWarning(*d));
            const QString name = shortDevice(d->device);
            warning += QStringLiteral("<br>") + tr("Type <b>%1</b> to confirm:").arg(name.toHtmlEscaped());
            m_confirm->setPlaceholderText(name);
            ok = ok && m_confirm->text().trimmed() == name;
            asked = true;
        }
    }
    m_warning->setText(warning);
    m_confirm->setVisible(asked);
    m_make->setEnabled(ok);
}

void RescueUsbDialog::openLogs()
{
    const Disk *d = target();
    const Volume *v = d ? rescueVolume(*d) : nullptr;
    if (!v)
        return;
    auto open = [](const QString &root) {
        const QString logs = root + QStringLiteral("/logs");
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo::exists(logs) ? logs : root));
    };
    if (!v->mounts().isEmpty())
        return open(v->mounts().first());

    // Mount it first; the mount point shows up with the next refresh.
    const QString disk = d->blockPath;
    const QString volume = v->objectPath;
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(m_udisks, &UDisks::operationFinished, this, [this, conn, disk, volume, open](bool ok, const QString &) {
        disconnect(*conn);
        if (!ok)
            return; // the main window says why
        auto *poll = new QTimer(this);
        auto tries = std::make_shared<int>(0);
        connect(poll, &QTimer::timeout, this, [this, poll, tries, disk, volume, open] {
            const Disk *d = m_udisks->diskByPath(disk);
            for (const Volume &v : d ? d->volumes : QVector<Volume>()) {
                if (v.objectPath == volume && !v.mounts().isEmpty()) {
                    poll->deleteLater();
                    updateState();
                    return open(v.mounts().first());
                }
            }
            if (++*tries > 20)
                poll->deleteLater();
        });
        poll->start(250);
    });
    m_udisks->mount(*v);
}

void RescueUsbDialog::reject()
{
    if (m_running) {
        if (!m_writer) {
            QMessageBox::information(this, windowTitle(), tr("One moment: DiskForge is in the middle of setting up the stick."));
            return;
        }
        const auto answer = QMessageBox::warning(this, windowTitle(),
                                                 tr("Stop making the DiskForge Live USB? The stick won't start anything until it's made again."),
                                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes && m_writer)
            m_writer->cancel(); // finish() runs when the copy stops
        return;
    }
    QDialog::reject();
}

void RescueUsbDialog::start()
{
    const Disk *d = target();
    if (!d || !m_inspected.error.isEmpty())
        return;
    m_diskPath = d->blockPath;
    m_copyDone = false;
    m_failure.clear();
    m_running = true;
    for (QWidget *w : std::initializer_list<QWidget *>{m_image, m_targets, m_confirm, m_make, m_openLogs, m_bios})
        w->setEnabled(false);
    m_biosBoot.clear();
    m_biosCore.clear();
    m_progress->setRange(0, 0);
    m_progress->setVisible(true);
    qCInfo(lcOps).noquote() << "Make a DiskForge Live USB on" << d->device << d->model << "from" << m_inspectedPath
                            << "version" << m_inspected.info.version << "build" << m_inspected.info.id;

    // One FAT32 partition, marked bootable: some PCs only offer a USB stick in their boot menu then.
    m_prep = new UsbPrep(m_udisks, m_diskPath, QStringLiteral("dos"), {{QStringLiteral("vfat"), kLabel, 0, 0x80, true}}, this);
    connect(m_prep, &UsbPrep::phase, m_phase, &QLabel::setText);
    connect(m_prep, &UsbPrep::failed, this, [this](const QString &message, bool shownAlready) {
        finish(false, message, shownAlready);
    });
    connect(m_prep, &UsbPrep::ready, this, [this](const QStringList &mountPoints) { copyFiles(mountPoints.value(0)); });
    connect(m_prep, &UsbPrep::done, this, [this] {
        if (!m_copyDone)
            return finish(false, m_failure.isEmpty() ? tr("The files couldn't be copied.") : m_failure);
        // Unmounted now: the boot code for old PCs goes in front of the partition.
        if (m_bios->isChecked() && !m_biosCore.isEmpty())
            return writeBiosBoot();
        finish(true, readyText(false));
    });
    m_prep->start();
}

void RescueUsbDialog::copyFiles(const QString &mountPoint)
{
    m_progress->setRange(0, 1000);
    m_writer = new rescue::StickWriter(m_inspectedPath, mountPoint);
    connect(m_writer, &rescue::StickWriter::progress, this, [this](const QString &phase, quint64 done, quint64 total) {
        m_meter.update(phase, done, total);
    });
    connect(m_writer, &rescue::StickWriter::finished, this, [this](bool ok, const QString &message) {
        // Before the thread stops: that deletes the writer.
        m_biosBoot = m_writer->biosBoot();
        m_biosCore = m_writer->biosCore();
        m_thread->quit();
        m_thread->wait();
        m_thread = nullptr;
        m_writer = nullptr;
        m_copyDone = ok;
        m_failure = ok ? QString() : message;
        // Unmounted either way, so it isn't left mounted after a failed copy.
        m_progress->setRange(0, 0);
        m_prep->finish();
    });
    m_thread = startOnThread(this, m_writer);
}

QString RescueUsbDialog::readyText(bool bios) const
{
    return tr("The DiskForge Live USB is ready.\n\n"
              "To use it, plug it into the PC that needs fixing and turn the PC on while pressing its boot menu key (usually "
              "F12, F11, F9 or Esc), then pick the USB stick. %1\n\n"
              "Every start leaves its logs in the logs folder on the stick.")
        .arg(bios ? tr("It starts UEFI PCs (Secure Boot can stay on) and old BIOS PCs.") : tr("It starts UEFI PCs, and Secure Boot can stay on."));
}

void RescueUsbDialog::writeBiosBoot()
{
    m_phase->setText(tr("Writing the boot code for old BIOS PCs…"));
    const QByteArray boot = m_biosBoot, core = m_biosCore;
    auto write = [this, boot, core](int fd) {
        QString error = tr("the stick couldn't be opened for writing");
        const bool ok = fd >= 0 && rescue::writeBiosBoot(fd, boot, core, &error);
        if (fd >= 0)
            blockio::close(fd);
        qCInfo(lcOps).noquote() << "Make a DiskForge Live USB: boot code for BIOS PCs" << (ok ? "written" : "not written: " + error);
        finish(true, ok ? readyText(true)
                        : readyText(false) + QStringLiteral("\n\n") + tr("Old BIOS-only PCs won't start from it: %1. Making it again "
                                                                         "adds that.").arg(error));
    };
    if (openStickForTest)
        return write(openStickForTest(m_diskPath));
    openBlockThen(m_udisks, this, m_diskPath, UDisks::OpenMode::ReadWrite, write);
}

void RescueUsbDialog::finish(bool ok, const QString &message, bool alreadyShown)
{
    if (m_prep) {
        m_prep->deleteLater();
        m_prep = nullptr;
    }
    m_running = false;
    if (!ok)
        qCInfo(lcOps).noquote() << "Make a DiskForge Live USB failed:" << message;
    m_progress->setVisible(false);
    m_udisks->refresh();
    if (ok) {
        m_phase->clear();
        QMessageBox::information(this, windowTitle(), message);
        QDialog::accept();
        return;
    }
    // A failed UDisks step was already shown by the main window.
    if (!alreadyShown)
        QMessageBox::warning(this, windowTitle(), message);
    m_phase->setText(redText(message));
    m_phase->setTextFormat(Qt::RichText);
    m_confirm->clear();
    for (QWidget *w : std::initializer_list<QWidget *>{m_image, m_targets, m_confirm, m_openLogs, m_bios})
        w->setEnabled(true);
    fillTargets(m_diskPath);
    updateState();
}
