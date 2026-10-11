// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "usbprep.h"

#include "applog.h"
#include "udisks.h"

#include <QTimer>

#include <algorithm>

namespace {

QString fsName(const QString &fsType)
{
    if (fsType == QLatin1String("vfat"))
        return QStringLiteral("FAT32");
    if (fsType == QLatin1String("exfat"))
        return QStringLiteral("exFAT");
    if (fsType == QLatin1String("ntfs"))
        return QStringLiteral("NTFS");
    return fsType;
}

} // namespace

UsbPrep::UsbPrep(UDisks *udisks, const QString &diskPath, const QString &tableType, const QVector<Partition> &partitions,
                 QObject *parent)
    : UDisksSteps(udisks, parent)
    , m_disk(diskPath)
    , m_table(tableType)
    , m_partitions(partitions)
{
}

void UsbPrep::start()
{
    m_volumes = QStringList(m_partitions.size(), QString());
    m_mounts = QStringList(m_partitions.size(), QString());
    followResults();
    makeTable();
}

QString UsbPrep::mountPointOf(const QString &volumePath) const
{
    const Disk *d = m_udisks->diskByPath(m_disk);
    for (const Volume &v : d ? d->volumes : QVector<Volume>()) {
        if (v.objectPath == volumePath)
            return v.mounts().value(0);
    }
    return {};
}

void UsbPrep::makeTable()
{
    const Disk *d = m_udisks->diskByPath(m_disk);
    if (!d)
        return fail(tr("The stick isn't there anymore."));
    emit phase(tr("Making a new partition table…"));
    expect([this] {
        waitFor([this] {
            const Disk *d = m_udisks->diskByPath(m_disk);
            return d && d->tableType == m_table && d->volumes.isEmpty();
        }, 30, [this] { makePartition(0); }, tr("The stick's new partition table didn't show up. Unplug it, plug it back in and try again."));
    });
    m_udisks->createPartitionTable(*d, m_table);
}

void UsbPrep::makePartition(int index)
{
    if (index >= m_partitions.size())
        return setFlags(0);
    const Disk *d = m_udisks->diskByPath(m_disk);
    if (!d)
        return fail(tr("The stick isn't there anymore."));
    // Each one goes right after the ones before it.
    quint64 offset = 0;
    QStringList before;
    for (const Volume &v : d->volumes) {
        offset = std::max(offset, v.offset + v.size);
        before << v.objectPath;
    }
    const Partition &p = m_partitions[index];
    emit phase(tr("Making the %1 partition…").arg(fsName(p.fsType)));
    expect([this, index, before] {
        waitFor([this, index, before] {
            const Disk *d = m_udisks->diskByPath(m_disk);
            for (const Volume &v : d ? d->volumes : QVector<Volume>()) {
                if (!before.contains(v.objectPath) && v.fsType == m_partitions[index].fsType) {
                    m_volumes[index] = v.objectPath;
                    return true;
                }
            }
            return false;
        }, 30, [this, index] { makePartition(index + 1); },
                tr("The stick's new partition didn't show up. Unplug it, plug it back in and try again."));
    });
    m_udisks->createPartition(*d, offset, p.size ? p.size : d->size, p.fsType, p.label);
}

void UsbPrep::setFlags(int index)
{
    while (index < m_partitions.size() && m_partitions[index].flags == 0)
        ++index;
    if (index >= m_partitions.size())
        return mountNext(0);
    const Disk *d = m_udisks->diskByPath(m_disk);
    const Volume *v = nullptr;
    for (const Volume &candidate : d ? d->volumes : QVector<Volume>()) {
        if (candidate.objectPath == m_volumes[index])
            v = &candidate;
    }
    if (!v)
        return fail(tr("The stick isn't there anymore."));
    // Some PCs only offer a USB stick in their boot menu when a partition is marked bootable.
    emit phase(tr("Marking it bootable…"));
    expect([this, index] { setFlags(index + 1); });
    m_udisks->setPartitionTypeAndFlags(*v, QString(), true, m_partitions[index].flags);
}

void UsbPrep::mountNext(int index)
{
    while (index < m_partitions.size() && !m_partitions[index].mount)
        ++index;
    if (index >= m_partitions.size()) {
        qCInfo(lcOps).noquote() << "Set up" << m_disk << "mounted at" << m_mounts.join(QLatin1Char(' '));
        emit ready(m_mounts);
        return;
    }
    const QString path = m_volumes[index];
    // Some desktops mount new USB partitions by themselves.
    if (!mountPointOf(path).isEmpty()) {
        m_mounts[index] = mountPointOf(path);
        return mountNext(index + 1);
    }
    const Disk *d = m_udisks->diskByPath(m_disk);
    const Volume *v = nullptr;
    for (const Volume &candidate : d ? d->volumes : QVector<Volume>()) {
        if (candidate.objectPath == path)
            v = &candidate;
    }
    if (!v)
        return fail(tr("The stick isn't there anymore."));
    emit phase(tr("Mounting it…"));
    // If the desktop mounted it first, Mount fails with "already mounted": what counts is
    // whether it ends up mounted.
    expect([this, index, path] {
        waitFor([this, path] { return !mountPointOf(path).isEmpty(); }, 15,
                [this, index, path] {
                    m_mounts[index] = mountPointOf(path);
                    mountNext(index + 1);
                },
                tr("The stick didn't mount."));
    }, true);
    m_udisks->mount(*v);
}

void UsbPrep::finish()
{
    emit phase(tr("Finishing up…"));
#ifdef Q_OS_WIN
    // The stick keeps its drive letter there, so it shows in File Explorer: what was written is
    // on it already, and Windows lets USB sticks go without being told.
    QTimer::singleShot(0, this, [this] { emit done(true, QString()); });
    return;
#endif
    followResults();
    unmountNext(0);
}

void UsbPrep::unmountNext(int index)
{
    while (index < m_partitions.size() && (m_volumes[index].isEmpty() || mountPointOf(m_volumes[index]).isEmpty()))
        ++index;
    if (index >= m_partitions.size()) {
        stopFollowing();
        emit done(true, QString());
        return;
    }
    const Disk *d = m_udisks->diskByPath(m_disk);
    for (const Volume &v : d ? d->volumes : QVector<Volume>()) {
        if (v.objectPath == m_volumes[index]) {
            expect([this, index] { unmountNext(index + 1); });
            m_udisks->unmount(v);
            return;
        }
    }
    unmountNext(index + 1);
}
