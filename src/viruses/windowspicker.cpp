// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "windowspicker.h"

#include "dialogs.h"
#include "format.h"
#include "udisks.h"
#include "winstartup.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <memory>

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

WindowsPicker::WindowsPicker(UDisks *udisks, bool writable, const QString &purpose, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_writable(writable)
    , m_list(new QListWidget)
    , m_note(new QLabel)
{
    setWindowTitle(tr("Which Windows?"));
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
#endif
    for (const Disk &d : m_udisks->disks()) {
        for (const Volume &v : d.volumes) {
            const QStringList mounts = v.mounts();
            const QString fs = v.effectiveFsType();
            QString text;
            if (!mounts.isEmpty() && runningRoot(mounts.first()))
                continue;
            if (!mounts.isEmpty() && startup::hasWindows(mounts.first()))
                text = tr("Windows on %1 (%2)").arg(volumeTitle(v), QDir::toNativeSeparators(mounts.first()));
            else if (mounts.isEmpty() && fs == QLatin1String("ntfs") && v.canMount())
                text = tr("%1: NTFS, not mounted yet").arg(volumeTitle(v));
            if (text.isEmpty())
                continue;
            auto *item = new QListWidgetItem(text, m_list);
            item->setData(Qt::UserRole, v.objectPath);
        }
    }
    m_ok->setEnabled(false);
    if (m_list->count() == 0)
        m_note->setText(tr("No Windows was found. A drive locked with BitLocker has to be unlocked first."));
}

void WindowsPicker::choose()
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item)
        return;
    if (item->data(Qt::UserRole + 1).toInt() == kRunning) {
        m_running = true;
        m_root.clear();
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
        if (!startup::hasWindows(mount)) {
            m_note->setText(redText(tr("There's no Windows on it.")));
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
    auto mountedConn = std::make_shared<QMetaObject::Connection>();
    auto failedConn = std::make_shared<QMetaObject::Connection>();
    *mountedConn = connect(m_udisks, &UDisks::changed, this, [this, found, use, mountedConn, failedConn] {
        const Volume *now = found();
        if (!now || now->mounts().isEmpty())
            return;
        disconnect(*mountedConn);
        disconnect(*failedConn);
        use(now->mounts().first());
    });
    *failedConn = connect(m_udisks, &UDisks::operationFinished, this, [this, mountedConn, failedConn](bool ok, const QString &message) {
        if (ok)
            return;
        disconnect(*mountedConn);
        disconnect(*failedConn);
        m_ok->setEnabled(true);
        // Windows left it hibernated (Fast Startup): it can only be read until Windows shuts down fully.
        m_note->setText(redText(message.toHtmlEscaped()) + QStringLiteral("<br>")
                        + (m_writable ? tr("If Windows was shut down with Fast Startup, it can only be read: start Windows and hold "
                                           "Shift while clicking Shut down, then try again.")
                                      : QString()));
    });
    m_udisks->mount(*v, !m_writable);
}
