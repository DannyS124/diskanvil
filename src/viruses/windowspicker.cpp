// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "windowspicker.h"

#include "activity.h"
#include "dialogs.h"
#include "format.h"
#include "hostprocess.h"
#include "rescuestick.h"
#include "udisks.h"
#include "winstart.h"
#include "winstartup.h"

#include <QApplication>
#include <QFile>
#include <QMessageBox>
#include <QProcess>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <memory>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

namespace {

constexpr int kRunning = 1;

// On Windows, the drive Windows is running from: it's "This PC's own Windows" already (and its
// registry files are locked, so reading them as another Windows' fails).
bool runningRoot(const QString &mount)
{
#ifdef Q_OS_WIN
    const QString system = QDir::cleanPath(qEnvironmentVariable("SystemDrive", QStringLiteral("C:")) + QStringLiteral("/"));
    return QDir::cleanPath(QDir::fromNativeSeparators(mount)).compare(system, Qt::CaseInsensitive) == 0;
#else
    Q_UNUSED(mount)
    return false;
#endif
}

} // namespace

QStringList WindowsPicker::mounted(UDisks *udisks)
{
    QStringList out;
    for (const Disk &d : udisks->disks()) {
        for (const Volume &v : d.volumes) {
            for (const QString &mount : v.mounts()) {
                if (startup::hasWindows(mount) && !runningRoot(mount))
                    out << mount;
            }
        }
    }
    return out;
}

WindowsPicker::WindowsPicker(UDisks *udisks, bool writable, const QString &purpose, QWidget *parent, bool anySystem)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_writable(writable)
    , m_anySystem(anySystem)
    , m_list(new QListWidget)
    , m_note(new QLabel)
{
    setWindowTitle(anySystem ? tr("Which System?") : tr("Which Windows?"));
    m_note->setWordWrap(true);
    m_list->setObjectName(QStringLiteral("windows"));
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(purpose.toHtmlEscaped()));
    layout->addWidget(m_list, 1);
    layout->addWidget(m_note);
    QDialogButtonBox *box = dialogButtons(this, tr("Choose"), &m_ok);
    // Choosing can mount first: it accepts once there's a Windows to use.
    disconnect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_ok, &QPushButton::clicked, this, &WindowsPicker::choose);
    layout->addWidget(box);
    connect(m_list, &QListWidget::itemDoubleClicked, this, &WindowsPicker::choose);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) { m_ok->setEnabled(row >= 0); });
    fill();
    resize(520, 360);
}

void WindowsPicker::fill()
{
    m_list->clear();
#ifdef Q_OS_WIN
    auto *self = new QListWidgetItem(tr("This PC's own Windows (running now)"), m_list);
    self->setData(Qt::UserRole + 1, kRunning);
#else
    // The Linux running now, but not DiskAnvil Live's own: that starts fresh every time.
    if (m_anySystem && !rescue::runningInRescue()) {
        auto *self = new QListWidgetItem(tr("This PC's own system (running now)"), m_list);
        self->setData(Qt::UserRole + 1, kRunning);
    }
#endif
    static const QStringList systems = {QStringLiteral("ntfs"), QStringLiteral("ext2"), QStringLiteral("ext3"), QStringLiteral("ext4"),
                                        QStringLiteral("btrfs"), QStringLiteral("xfs"), QStringLiteral("f2fs"), QStringLiteral("hfsplus")};
    for (const Disk &d : m_udisks->disks()) {
        for (const Volume &v : d.volumes) {
            const QStringList mounts = v.mounts();
            const QString fs = v.effectiveFsType();
            QString text;
            if (!mounts.isEmpty() && (runningRoot(mounts.first()) || (m_anySystem && mounts.contains(QStringLiteral("/")))))
                continue;
            const QString system = mounts.isEmpty() ? QString() : m_anySystem ? activity::systemOn(mounts.first())
                                                                               : startup::hasWindows(mounts.first()) ? QStringLiteral("Windows") : QString();
            if (!system.isEmpty())
                text = tr("%1 on %2 (%3)").arg(system, volumeTitle(v), QDir::toNativeSeparators(mounts.first()));
            else if (mounts.isEmpty() && fs == QLatin1String("ntfs") && v.canMount())
                text = tr("%1: NTFS, not mounted yet").arg(volumeTitle(v));
            else if (m_anySystem && mounts.isEmpty() && systems.contains(fs) && v.canMount())
                text = tr("%1: %2, not mounted yet").arg(volumeTitle(v), fs);
            if (text.isEmpty())
                continue;
            auto *item = new QListWidgetItem(text, m_list);
            item->setData(Qt::UserRole, v.objectPath);
        }
    }
    m_ok->setEnabled(false);
    if (m_list->count() == 0)
        m_note->setText(m_anySystem ? tr("No system was found. A locked drive (BitLocker, LUKS) has to be unlocked first.")
                                    : tr("No Windows was found. A drive locked with BitLocker has to be unlocked first."));
}

void WindowsPicker::choose()
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item)
        return;
    if (item->data(Qt::UserRole + 1).toInt() == kRunning) {
        m_running = true;
#ifdef Q_OS_WIN
        m_root.clear();
#else
        m_root = QStringLiteral("/");
#endif
        accept();
        return;
    }
    const QString path = item->data(Qt::UserRole).toString();
    const auto found = [this, path]() -> const Volume * {
        for (const Disk &d : m_udisks->disks()) {
            for (const Volume &v : d.volumes) {
                if (v.objectPath == path || v.cleartextPath == path)
                    return &v;
            }
        }
        return nullptr;
    };
    const Volume *v = found();
    if (!v)
        return fill();
    const auto use = [this](const QString &mount) {
        if (m_anySystem ? activity::systemOn(mount).isEmpty() : !startup::hasWindows(mount)) {
            m_note->setText(redText(m_anySystem ? tr("There's no Windows, Linux or Mac system on it.") : tr("There's no Windows on it.")));
            return;
        }
        m_root = mount;
        accept();
    };
    if (!v->mounts().isEmpty())
        return use(v->mounts().first());
    // Mounted, then used: read-only unless the tool changes something.
    m_ok->setEnabled(false);
    m_note->setText(m_writable ? tr("Mounting it…") : tr("Mounting it, read-only…"));
    const QString device = v->device;
    const bool ntfs = v->effectiveFsType() == QLatin1String("ntfs");
    mountThen(path, !m_writable, [this, use, device, ntfs, path](const QString &mount, const QString &error) {
        if (!mount.isEmpty())
            return use(mount);
#ifndef Q_OS_WIN
        // Linux opens an NTFS drive for changes only when Windows closed it properly. Read-only
        // tells which it is: hibernated (Fast Startup: it must stay as it is) or just not closed
        // (a crash, the power, a PC switched off while starting: a mark that can be cleared).
        if (m_writable && ntfs) {
            m_note->setText(tr("Looking at why it can't be changed…"));
            mountThen(path, true, [this, use, device, path, error](const QString &readOnly, const QString &) {
                if (readOnly.isEmpty())
                    return failed(error);
                const bool hibernated = windowsHibernated(readOnly);
                unmountThen(path, [this, use, device, path, hibernated, error] {
                    if (hibernated)
                        return failed(error, tr("Windows left it hibernated (Fast Startup), so it can only be read: start Windows and hold "
                                                "Shift while clicking Shut down, then try again. Clearing that would lose what Windows "
                                                "kept open."));
                    if (QMessageBox::question(this, windowTitle(),
                                              tr("Windows didn't close this drive properly: it crashed, lost power or was switched off while "
                                                 "starting, which is usual for a Windows that won't start. Linux only changes such a drive "
                                                 "once that mark is cleared.\n\nClear the mark and open it? Nothing else on the drive is "
                                                 "changed. Afterwards, let Windows check the drive (chkdsk) when it starts again."))
                        != QMessageBox::Yes)
                        return failed(error);
                    QString why;
                    if (!clearDirtyMark(device, &why))
                        return failed(error, tr("The mark couldn't be cleared: %1").arg(why));
                    m_note->setText(tr("Mounting it…"));
                    mountThen(path, false, [this, use, error](const QString &mount, const QString &again) {
                        if (mount.isEmpty())
                            return failed(again.isEmpty() ? error : again);
                        use(mount);
                    });
                });
            });
            return;
        }
#endif
        failed(error);
    });
}

void WindowsPicker::failed(const QString &error, const QString &why)
{
    m_ok->setEnabled(true);
    m_note->setText(redText(error.toHtmlEscaped()) + (why.isEmpty() ? QString() : QStringLiteral("<br>") + why.toHtmlEscaped()));
}

void WindowsPicker::mountThen(const QString &path, bool readOnly, const std::function<void(const QString &mount, const QString &error)> &then)
{
    const auto found = [this, path]() -> const Volume * {
        for (const Disk &d : m_udisks->disks())
            for (const Volume &v : d.volumes)
                if (v.objectPath == path || v.cleartextPath == path)
                    return &v;
        return nullptr;
    };
    const Volume *v = found();
    if (!v)
        return then({}, tr("The drive is gone."));
    auto mountedConn = std::make_shared<QMetaObject::Connection>();
    auto failedConn = std::make_shared<QMetaObject::Connection>();
    *mountedConn = connect(m_udisks, &UDisks::changed, this, [found, then, mountedConn, failedConn] {
        const Volume *now = found();
        if (!now || now->mounts().isEmpty())
            return;
        disconnect(*mountedConn);
        disconnect(*failedConn);
        then(now->mounts().first(), {});
    });
    *failedConn = connect(m_udisks, &UDisks::operationFinished, this, [then, mountedConn, failedConn](bool ok, const QString &message) {
        if (ok)
            return;
        disconnect(*mountedConn);
        disconnect(*failedConn);
        then({}, message);
    });
    m_udisks->mount(*v, readOnly);
}

void WindowsPicker::unmountThen(const QString &path, const std::function<void()> &then)
{
    for (const Disk &d : m_udisks->disks()) {
        for (const Volume &v : d.volumes) {
            if ((v.objectPath == path || v.cleartextPath == path) && !v.mounts().isEmpty()) {
                auto conn = std::make_shared<QMetaObject::Connection>();
                *conn = connect(m_udisks, &UDisks::operationFinished, this, [then, conn](bool, const QString &) {
                    disconnect(*conn);
                    then();
                });
                m_udisks->unmount(v);
                return;
            }
        }
    }
    then();
}

bool WindowsPicker::windowsHibernated(const QString &mount)
{
    // hiberfil.sys starts with "hibr" (or "HIBR", "wake") while it holds a hibernated Windows.
    QFile f(winstart::find(mount, QStringLiteral("hiberfil.sys")));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = f.read(4).toLower();
    return head == "hibr" || head == "wake";
}

bool WindowsPicker::clearDirtyMark(const QString &device, QString *why)
{
#ifdef Q_OS_WIN
    Q_UNUSED(device)
    *why = tr("not needed on Windows");
    return false;
#else
    // ntfs-3g's ntfsfix, as root: DiskAnvil Live's sudo, or the admin password on a PC.
    QStringList command{QStringLiteral("ntfsfix"), QStringLiteral("-d"), device};
    if (::geteuid() != 0)
        command = rescue::runningInRescue() ? QStringList{QStringLiteral("sudo"), QStringLiteral("-n")} + command
                                            : host::command(QStringList{QStringLiteral("pkexec")} + command);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QProcess p;
    p.start(command.first(), command.mid(1));
    const bool ok = p.waitForFinished(60000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    QApplication::restoreOverrideCursor();
    if (!ok)
        *why = QString::fromLocal8Bit(p.readAllStandardError() + p.readAllStandardOutput()).trimmed().section(QLatin1Char('\n'), -2);
    if (!ok && why->isEmpty())
        *why = p.errorString();
    return ok;
#endif
}
