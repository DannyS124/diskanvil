// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bootentriesdialog.h"

#include "applog.h"
#include "dialogs.h"
#include "format.h"
#include "hostprocess.h"
#include "rescuestick.h"
#include "udisks.h"
#include "winstartup.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QTreeWidget>
#include <QVBoxLayout>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

namespace {

// The volume a GPT partition GUID is, and the file on it when it's mounted.
const Volume *volumeOf(UDisks *udisks, const QString &partition, const Disk **disk = nullptr)
{
    for (const Disk &d : udisks->disks()) {
        for (const Volume &v : d.volumes) {
            if (!partition.isEmpty() && v.partUuid.compare(partition, Qt::CaseInsensitive) == 0) {
                if (disk)
                    *disk = &d;
                return &v;
            }
        }
    }
    return nullptr;
}

// A path on a FAT partition, whatever case its folders have.
QString findOnVolume(const QString &mount, const QString &efiPath)
{
    QString at = mount;
    for (const QString &part : QString(efiPath).replace(QLatin1Char('\\'), QLatin1Char('/')).split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        const QStringList names = QDir(at).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        const auto match = std::find_if(names.cbegin(), names.cend(), [&part](const QString &n) { return n.compare(part, Qt::CaseInsensitive) == 0; });
        if (match == names.cend())
            return {};
        at += QLatin1Char('/') + *match;
    }
    return at;
}

QString whereText(UDisks *udisks, const efi::BootEntry &e, bool *missing)
{
    *missing = false;
    if (e.partition.isEmpty())
        return QObject::tr("Not a file on a disk (the firmware's own, or a network start)");
    const Volume *v = volumeOf(udisks, e.partition);
    if (!v) {
        *missing = true;
        return QObject::tr("Its partition isn't here");
    }
    if (v->mounts().isEmpty())
        return QObject::tr("%1 (not mounted to check)").arg(volumeTitle(*v));
    if (findOnVolume(v->mounts().first(), e.path).isEmpty()) {
        *missing = true;
        return QObject::tr("%1: the file is missing").arg(volumeTitle(*v));
    }
    return volumeTitle(*v);
}

} // namespace

BootEntriesDialog::BootEntriesDialog(UDisks *udisks, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_list(new QTreeWidget)
    , m_status(new QLabel)
    , m_saveOrder(new QPushButton(tr("Save the Order")))
{
    setWindowTitle(tr("Boot Menu Entries"));
    m_list->setHeaderLabels({tr("Name"), tr("Starts"), tr("On"), tr("State")});
    m_list->setRootIsDecorated(false);
    m_list->setObjectName(QStringLiteral("entries"));
    m_status->setWordWrap(true);
    m_saveOrder->setEnabled(false);
    m_saveOrder->setObjectName(QStringLiteral("saveOrder"));

    auto *up = new QPushButton(tr("Up"));
    auto *down = new QPushButton(tr("Down"));
    auto *next = new QPushButton(tr("Start It Next Time"));
    auto *onOff = new QPushButton(tr("Switch Off/On"));
    auto *remove = new QPushButton(tr("Remove…"));
    auto *add = new QPushButton(tr("Add…"));
    auto *reportButton = new QPushButton(tr("Report…"));
    next->setObjectName(QStringLiteral("next"));
    remove->setObjectName(QStringLiteral("remove"));
    connect(up, &QPushButton::clicked, this, [this] { move(-1); });
    connect(down, &QPushButton::clicked, this, [this] { move(1); });
    connect(m_saveOrder, &QPushButton::clicked, this, &BootEntriesDialog::saveOrder);
    connect(next, &QPushButton::clicked, this, [this] {
        const int n = selectedNumber();
        if (n < 0)
            return;
        change(efiboot::nextCommand(n), [n](QString *error) {
#ifdef Q_OS_WIN
            return efi::setVariable(QStringLiteral("BootNext"), efiboot::orderValue({n}), error);
#else
            Q_UNUSED(error)
            return false;
#endif
        });
    });
    connect(onOff, &QPushButton::clicked, this, [this] {
        const int n = selectedNumber();
        const auto it = std::find_if(m_state.entries.cbegin(), m_state.entries.cend(), [n](const efi::BootEntry &e) { return e.number == n; });
        if (it == m_state.entries.cend())
            return;
        const bool active = !it->active;
        change(efiboot::activeCommand(n, active), [n, active](QString *error) {
#ifdef Q_OS_WIN
            QByteArray raw = efi::variable(QStringLiteral("Boot") + efiboot::hex4(n)).value_or(QByteArray());
            if (raw.size() < 4)
                return false;
            raw[0] = char(active ? (raw[0] | 1) : (raw[0] & ~1));
            return efi::setVariable(QStringLiteral("Boot") + efiboot::hex4(n), raw, error);
#else
            Q_UNUSED(error)
            return false;
#endif
        });
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int n = selectedNumber();
        if (n < 0)
            return;
        const auto it = std::find_if(m_state.entries.cbegin(), m_state.entries.cend(), [n](const efi::BootEntry &e) { return e.number == n; });
        QString text = tr("Remove \"%1\" from the boot menu? Only the entry goes: the files it starts stay where they are, and an "
                          "entry can be added for them again.")
                           .arg(it == m_state.entries.cend() ? efiboot::hex4(n) : it->description);
        if (n == m_state.current)
            text += QStringLiteral("\n\n") + tr("It's the one this PC started from this time.");
        if (QMessageBox::question(this, windowTitle(), text, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        QVector<int> order = m_state.order;
        order.removeAll(n);
        change(efiboot::removeCommand(n), [n, order](QString *error) {
#ifdef Q_OS_WIN
            return efi::setVariable(QStringLiteral("Boot") + efiboot::hex4(n), QByteArray(), error)
                && efi::setVariable(QStringLiteral("BootOrder"), efiboot::orderValue(order), error);
#else
            Q_UNUSED(error)
            Q_UNUSED(order)
            return false;
#endif
        });
    });
    connect(add, &QPushButton::clicked, this, &BootEntriesDialog::addEntry);
    connect(reportButton, &QPushButton::clicked, this, [this] {
        QDialog shown(this);
        shown.setWindowTitle(tr("How This PC Starts"));
        auto *text = new QPlainTextEdit(report(m_udisks, m_state));
        text->setReadOnly(true);
        text->setObjectName(QStringLiteral("report"));
        auto *copy = new QPushButton(tr("Copy"));
        connect(copy, &QPushButton::clicked, &shown, [text] { QApplication::clipboard()->setText(text->toPlainText()); });
        auto *close = new QPushButton(tr("Close"));
        connect(close, &QPushButton::clicked, &shown, &QDialog::accept);
        auto *row = new QHBoxLayout;
        row->addStretch();
        row->addWidget(copy);
        row->addWidget(close);
        auto *layout = new QVBoxLayout(&shown);
        layout->addWidget(text);
        layout->addLayout(row);
        shown.resize(720, 520);
        shown.exec();
    });

    auto *side = new QVBoxLayout;
    for (QPushButton *b : {up, down, m_saveOrder, next, onOff, remove, add})
        side->addWidget(b);
    side->addStretch();
    side->addWidget(reportButton);
    auto *body = new QHBoxLayout;
    body->addWidget(m_list, 1);
    body->addLayout(side);
    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("The PC's own boot menu: what it tries to start, in this order. When a system stopped "
                                       "starting after an update or a firmware reset, its entry is often just gone or moved down: "
                                       "put it back first, or add it again.")));
    layout->addLayout(body, 1);
    layout->addWidget(m_status);
    layout->addWidget(close, 0, Qt::AlignRight);
    resize(860, 460);
    reload();
}

void BootEntriesDialog::reload()
{
    m_state = efiboot::read();
    m_order = m_state.order;
    m_saveOrder->setEnabled(false);
    fill();
}

void BootEntriesDialog::fill()
{
    const int selected = selectedNumber();
    m_list->clear();
    if (!m_state.uefi) {
        m_status->setText(tr("This PC started the old BIOS way, so it has no boot menu of this kind: it starts the first drive "
                             "(or the one picked in its setup) by its boot code."));
        return;
    }
    // In the order being arranged, then the ones not in it.
    QVector<efi::BootEntry> shown;
    for (int n : std::as_const(m_order)) {
        for (const efi::BootEntry &e : std::as_const(m_state.entries)) {
            if (e.number == n)
                shown << e;
        }
    }
    for (const efi::BootEntry &e : std::as_const(m_state.entries)) {
        if (!m_order.contains(e.number))
            shown << e;
    }
    for (const efi::BootEntry &e : std::as_const(shown)) {
        bool missing = false;
        const QString where = whereText(m_udisks, e, &missing);
        QStringList state;
        state << (!m_order.contains(e.number) ? tr("Not in the order") : e.active ? tr("On") : tr("Off"));
        if (e.number == m_state.current)
            state << tr("started this time");
        if (e.number == m_state.next)
            state << tr("next time");
        auto *item = new QTreeWidgetItem(m_list, {e.description, e.path, where, state.join(QStringLiteral(", "))});
        item->setData(0, Qt::UserRole, e.number);
        item->setToolTip(0, QStringLiteral("Boot%1").arg(efiboot::hex4(e.number)));
        if (missing)
            item->setForeground(2, QBrush(QColor(Qt::red)));
        if (e.number == selected)
            m_list->setCurrentItem(item);
    }
    for (int c = 0; c < m_list->columnCount(); ++c)
        m_list->resizeColumnToContents(c);
}

int BootEntriesDialog::selectedNumber() const
{
    return m_list->currentItem() ? m_list->currentItem()->data(0, Qt::UserRole).toInt() : -1;
}

bool BootEntriesDialog::change(const QStringList &command, const std::function<bool(QString *)> &onWindows)
{
    QString said;
    bool ok = false;
    if (runForTest) {
        ok = runForTest(command, &said);
    } else {
#ifdef Q_OS_WIN
        Q_UNUSED(command)
        ok = onWindows(&said);
#else
        Q_UNUSED(onWindows)
        // As root: DiskAnvil Live's sudo, or the admin password.
        const QStringList full = rescue::runningInRescue() ? QStringList{QStringLiteral("sudo"), QStringLiteral("-n")} + command
                               : ::geteuid() == 0          ? command
                                                           : host::command(QStringList{QStringLiteral("pkexec")} + command);
        QProcess run;
        run.setProcessChannelMode(QProcess::MergedChannels);
        run.start(full.first(), full.mid(1));
        if (!run.waitForStarted(10000)) {
            said = tr("efibootmgr isn't on this computer: install it, or use DiskAnvil Live");
        } else {
            ok = run.waitForFinished(60000) && run.exitStatus() == QProcess::NormalExit && run.exitCode() == 0;
            said = QString::fromLocal8Bit(run.readAll()).trimmed();
        }
#endif
    }
    qCInfo(lcOps).noquote() << "Boot menu:" << command.join(QLatin1Char(' ')) << (ok ? "done" : "failed: " + said);
    m_status->setText(ok ? tr("Done.") : redText(tr("It wasn't changed: %1").arg(said.right(500)).toHtmlEscaped()));
    reload();
    return ok;
}

void BootEntriesDialog::move(int by)
{
    const int n = selectedNumber();
    const int at = int(m_order.indexOf(n));
    if (at < 0 || at + by < 0 || at + by >= m_order.size())
        return;
    m_order.swapItemsAt(at, at + by);
    m_saveOrder->setEnabled(m_order != m_state.order);
    fill();
}

void BootEntriesDialog::saveOrder()
{
    const QVector<int> order = m_order;
    change(efiboot::orderCommand(order), [order](QString *error) {
#ifdef Q_OS_WIN
        return efi::setVariable(QStringLiteral("BootOrder"), efiboot::orderValue(order), error);
#else
        Q_UNUSED(error)
        Q_UNUSED(order)
        return false;
#endif
    });
}

void BootEntriesDialog::addEntry()
{
    // A loader on a mounted EFI partition (or any FAT partition on a GPT disk).
    const QString file = QFileDialog::getOpenFileName(this, tr("The Program to Start"), QDir::rootPath(), tr("UEFI programs (*.efi *.EFI)"));
    if (file.isEmpty())
        return;
    const Disk *disk = nullptr;
    const Volume *volume = nullptr;
    QString mount;
    for (const Disk &d : m_udisks->disks()) {
        for (const Volume &v : d.volumes) {
            for (const QString &m : v.mounts()) {
                if ((file.startsWith(m + QLatin1Char('/')) || m == QLatin1String("/")) && m.size() > mount.size() && d.tableType == QLatin1String("gpt")) {
                    disk = &d;
                    volume = &v;
                    mount = m;
                }
            }
        }
    }
    if (!disk || !volume || !QStringList{QStringLiteral("vfat"), QStringLiteral("fat")}.contains(volume->fsType)) {
        QMessageBox::warning(this, windowTitle(), tr("It has to be on an EFI partition (FAT, on a GPT disk): the firmware only starts programs from there."));
        return;
    }
    const QString path = QLatin1Char('\\') + QDir(mount).relativeFilePath(file).replace(QLatin1Char('/'), QLatin1Char('\\'));
    const QString suggested = path.section(QLatin1Char('\\'), -2, -2);
    bool ok = false;
    const QString label = QInputDialog::getText(this, windowTitle(), tr("Its name in the boot menu:"), QLineEdit::Normal, suggested, &ok).trimmed();
    if (!ok || label.isEmpty())
        return;
    const quint64 sector = quint64(std::max(disk->sectorSize, 512));
    const QByteArray option = efiboot::loadOption(label, quint32(volume->number), volume->offset / sector, volume->size / sector, volume->partUuid, path);
    QVector<int> order = m_state.order;
    change(efiboot::createCommand(disk->device, volume->number, path, label), [option, order](QString *error) {
#ifdef Q_OS_WIN
        // The first free number, put first in the order (what efibootmgr does).
        int free = 0;
        while (free < 0xFFFF && efi::variable(QStringLiteral("Boot") + efiboot::hex4(free)))
            ++free;
        QVector<int> first = order;
        first.prepend(free);
        return efi::setVariable(QStringLiteral("Boot") + efiboot::hex4(free), option, error)
            && efi::setVariable(QStringLiteral("BootOrder"), efiboot::orderValue(first), error);
#else
        Q_UNUSED(error)
        Q_UNUSED(option)
        Q_UNUSED(order)
        return false;
#endif
    });
}

QString BootEntriesDialog::report(UDisks *udisks, const efiboot::State &state)
{
    QStringList out;
    out << tr("How this PC starts (DiskAnvil, %1)").arg(QDateTime::currentDateTime().toString(Qt::ISODate)) << QString();
    if (!state.uefi) {
        out << tr("Firmware: the old BIOS way (no UEFI boot menu).");
    } else {
        const efi::Trust trust = efi::trust();
        out << tr("Firmware: UEFI.")
            << (trust.known ? (trust.secureBoot ? tr("Secure Boot: on.") : tr("Secure Boot: off.")) + (trust.setupMode ? QLatin1Char(' ') + tr("(Setup mode: no platform key.)") : QString())
                            : tr("Secure Boot: couldn't be read."));
        out << QString() << tr("Boot menu, in order:");
        for (const efi::BootEntry &e : state.entries) {
            bool missing = false;
            const QString where = whereText(udisks, e, &missing);
            out << QStringLiteral("  Boot%1 %2%3: %4 %5 [%6]%7")
                       .arg(efiboot::hex4(e.number), e.description, e.active ? QString() : QStringLiteral(" (off)"), e.path, e.partition, where,
                            e.number == state.current ? QStringLiteral(" <- started this time") : QString());
        }
        if (state.next >= 0)
            out << tr("  Next time, once: Boot%1").arg(efiboot::hex4(state.next));
    }
    out << QString() << tr("EFI partitions and what's on them:");
    for (const Disk &d : udisks->disks()) {
        for (const Volume &v : d.volumes) {
            if (!v.isEfi)
                continue;
            out << QStringLiteral("  %1 on %2 (%3, %4)").arg(volumeTitle(v), shortDevice(d.device), formatSize(v.size), v.partUuid);
            if (v.mounts().isEmpty()) {
                out << tr("    not mounted, so not looked into");
                continue;
            }
            const QString efi = findOnVolume(v.mounts().first(), QStringLiteral("EFI"));
            QDirIterator it(efi, {QStringLiteral("*.efi"), QStringLiteral("*.EFI")}, QDir::Files, QDirIterator::Subdirectories);
            int listed = 0;
            while (it.hasNext() && listed < 60) {
                out << QStringLiteral("    \\EFI\\") + QDir(efi).relativeFilePath(it.next()).replace(QLatin1Char('/'), QLatin1Char('\\'));
                ++listed;
            }
        }
    }
    out << QString() << tr("Systems on mounted drives:");
    for (const Disk &d : udisks->disks()) {
        for (const Volume &v : d.volumes) {
            for (const QString &m : v.mounts()) {
                QFile release(m + QStringLiteral("/etc/os-release"));
                if (release.open(QIODevice::ReadOnly)) {
                    static const QRegularExpression pretty(QStringLiteral("^PRETTY_NAME=\"?([^\"\\n]*)"), QRegularExpression::MultilineOption);
                    out << QStringLiteral("  %1 on %2 (%3)").arg(pretty.match(QString::fromUtf8(release.readAll())).captured(1), volumeTitle(v), m);
                }
                if (startup::hasWindows(m))
                    out << QStringLiteral("  Windows on %1 (%2)").arg(volumeTitle(v), m);
            }
        }
    }
    return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
}
