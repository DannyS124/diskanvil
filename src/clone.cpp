// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "clone.h"

#include "newids.h"

#include "blockio.h"
#include "format.h"
#include "gpt.h"

#include <QRandomGenerator>
#include <QUuid>

#include <cstring>
#include <unistd.h>

namespace diskclone {

namespace {

constexpr quint64 MiB = 1024 * 1024;

// Room the GPT backup needs at the end: 128 entries of 128 bytes plus its header.
quint64 gptTail(int sectorSize)
{
    return 16384 + quint64(sectorSize);
}

} // namespace

Plan plan(const Disk &source, const Disk &target)
{
    Plan p;
    p.gpt = source.tableType == QLatin1String("gpt");
    p.sectorSize = source.sectorSize;
    if (source.blockPath == target.blockPath) {
        p.error = QObject::tr("Pick a different drive to copy to.");
        return p;
    }
    if (source.isSystem) {
        p.error = QObject::tr("%1 runs this system, so it keeps changing while it's copied. Start the PC from a live USB to clone it.")
                      .arg(shortDevice(source.device));
        return p;
    }
    if (target.isSystem) {
        p.error = QObject::tr("%1 runs this system, so DiskForge won't write over it.").arg(shortDevice(target.device));
        return p;
    }
    if (target.readOnly) {
        p.error = QObject::tr("%1 is read-only.").arg(shortDevice(target.device));
        return p;
    }
    if (source.sectorSize != target.sectorSize) {
        p.error = QObject::tr("The drives use different sector sizes (%1 and %2 bytes), so the copy wouldn't work. "
                              "Copy the partitions' files instead.")
                      .arg(source.sectorSize)
                      .arg(target.sectorSize);
        return p;
    }

    // A partition table can say anything; one that runs past the end of the drive is broken.
    for (const Volume &v : source.volumes) {
        if (v.offset > source.size || v.size > source.size - v.offset) {
            p.error = QObject::tr("The partition table on %1 doesn't fit the drive, so it can't be copied this way.").arg(shortDevice(source.device));
            return p;
        }
    }
    if (source.tableType.isEmpty()) {
        // A file system on the whole drive: copy all of it.
        p.extents = {{0, source.size, 0}};
    } else {
        quint64 first = source.size, end = 0;
        for (const Volume &v : source.volumes) {
            first = std::min(first, v.offset);
            if (!v.isContainer)
                end = std::max(end, v.offset + v.size);
        }
        if (source.volumes.isEmpty())
            first = std::min<quint64>(MiB, source.size);
        // Partition table plus whatever boot loaders keep before the first partition.
        p.extents.append({0, first, 0});
        for (const Volume &v : source.volumes) {
            if (!v.isContainer && !v.isContained)
                p.extents.append({v.offset, v.size, v.offset});
        }
        // MBR logical partitions: each one has its little table (EBR) just before it.
        for (const Volume &c : source.volumes) {
            if (!c.isContainer)
                continue;
            quint64 from = c.offset;
            for (const Volume &v : source.volumes) {
                if (!v.isContained || v.offset < c.offset || v.offset >= c.offset + c.size)
                    continue;
                if (v.offset > from)
                    p.extents.append({from, v.offset - from, from});
                p.extents.append({v.offset, v.size, v.offset});
                from = v.offset + v.size;
            }
            if (from == c.offset) // empty extended partition: still copy its first table
                p.extents.append({c.offset, std::min<quint64>(MiB, c.size), c.offset});
        }
        const quint64 needed = std::max(end, first) + (p.gpt ? gptTail(source.sectorSize) : 0);
        if (target.size < needed) {
            p.error = QObject::tr("%1 is too small: the partitions need %2 and it has %3.")
                          .arg(shortDevice(target.device), formatSize(needed), formatSize(target.size));
            return p;
        }
        std::sort(p.extents.begin(), p.extents.end(), [](const blockcopy::Extent &a, const blockcopy::Extent &b) { return a.source < b.source; });
    }
    if (target.size < p.extents.last().target + p.extents.last().length) {
        p.error = QObject::tr("%1 is too small: it needs %2 and has %3.")
                      .arg(shortDevice(target.device), formatSize(source.size), formatSize(target.size));
        return p;
    }
    p.bytes = blockcopy::totalLength(p.extents);
    return p;
}

QString newUuid(const QString &fsType)
{
    auto hex = [](int digits) {
        QString s;
        for (int i = 0; i < digits; ++i)
            s += QString::number(QRandomGenerator::system()->bounded(16), 16).toUpper();
        return s;
    };
    static const QStringList uuidStyle = {QStringLiteral("ext2"), QStringLiteral("ext3"), QStringLiteral("ext4"), QStringLiteral("xfs"),
                                          QStringLiteral("btrfs"), QStringLiteral("f2fs"), QStringLiteral("swap")};
    if (uuidStyle.contains(fsType))
        return QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (fsType == QLatin1String("vfat") || fsType == QLatin1String("exfat")) {
        const QString s = hex(8);
        return s.left(4) + QLatin1Char('-') + s.mid(4);
    }
    if (fsType == QLatin1String("ntfs"))
        return hex(16);
    return {};
}

} // namespace diskclone

CloneJob::CloneJob(int sourceFd, int targetFd, const diskclone::Plan &plan, bool newIds, bool verify)
    : m_source(sourceFd)
    , m_target(targetFd)
    , m_plan(plan)
    , m_newIds(newIds)
    , m_verify(verify)
{
}

CloneJob::~CloneJob()
{
    if (m_source >= 0)
        ::close(m_source);
    if (m_target >= 0)
        ::close(m_target);
}

void CloneJob::run()
{
    blockcopy::Options o;
    o.sourceFd = m_source;
    o.targetFd = m_target;
    o.extents = m_plan.extents;
    const QString copying = tr("Copying");
    const blockcopy::Result copied = blockcopy::copy(o, m_cancel, [&](quint64 done, quint64 total) { emit progress(copying, done, total); });
    if (!copied.ok) {
        emit finished(false, copied.cancelled ? tr("Stopped. The target drive now holds a partial copy; format it before using it.")
                                              : copied.error);
        return;
    }

    if (m_verify) {
        const QString checking = tr("Checking");
        const blockcopy::Result back = blockcopy::readBack(m_target, m_plan.extents, m_cancel,
                                                           [&](quint64 done, quint64 total) { emit progress(checking, done, total); });
        if (!back.ok) {
            emit finished(false, back.cancelled ? tr("Copied, but the check was stopped.") : back.error);
            return;
        }
        if (back.dataSha256 != copied.dataSha256) {
            emit finished(false, tr("The copy doesn't match the original. The target drive may be failing."));
            return;
        }
    }

    if (m_plan.gpt) {
        const gpt::Result r = gpt::relocateBackup(m_target, m_plan.sectorSize, m_newIds);
        if (!r.ok) {
            emit finished(false, tr("Copied, but the partition table couldn't be finished: %1").arg(r.error));
            return;
        }
    } else if (m_newIds && !m_plan.extents.isEmpty() && m_plan.extents.first().source == 0) {
        QString error;
        if (!newids::giveDiskNewIds(m_target, false, m_plan.sectorSize, &error)) {
            emit finished(false, tr("Copied, but the disk ID couldn't be changed."));
            return;
        }
    }
    emit finished(true, m_verify ? tr("Copied and checked %1.").arg(formatSize(copied.bytes)) : tr("Copied %1.").arg(formatSize(copied.bytes)));
}
