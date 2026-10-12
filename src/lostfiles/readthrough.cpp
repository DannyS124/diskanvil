// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "readthrough.h"

#include "blockio.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>

#include <cerrno>
#include <cstring>

namespace lost {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("readthrough", text);
}

constexpr quint64 kBlock = 64 * 1024; // what's copied at a time: whole blocks, aligned

} // namespace

std::unique_ptr<ReadThrough> ReadThrough::open(std::shared_ptr<Source> drive, const QString &image, QString *why)
{
    std::unique_ptr<ReadThrough> t(new ReadThrough);
    t->m_drive = std::move(drive);
    t->m_size = t->m_drive->size();
    t->m_mapPath = image + QStringLiteral(".map");
    t->m_map = RescueMap(t->m_size);
    // An image made before carries on, when its map is there and is for a drive this size.
    if (QFileInfo::exists(image) && QFileInfo::exists(t->m_mapPath)) {
        QString problem;
        if (!t->m_map.load(t->m_mapPath, t->m_size, &problem)) {
            if (why)
                *why = tr("The copy made before doesn't fit this drive: %1").arg(problem);
            return nullptr;
        }
    }
    t->m_image = blockio::openFile(image, blockio::FileMode::Update);
    if (t->m_image < 0 || !blockio::setFileSize(t->m_image, t->m_size)) {
        if (why)
            *why = tr("The image couldn't be made there: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        return nullptr;
    }
    QString problem;
    if (!t->m_map.save(t->m_mapPath, &problem)) {
        if (why)
            *why = tr("Its map couldn't be written: %1").arg(problem);
        return nullptr;
    }
    return t;
}

ReadThrough::~ReadThrough()
{
    saveMap();
    if (m_image >= 0) {
        blockio::flush(m_image);
        blockio::close(m_image);
    }
}

bool ReadThrough::read(quint64 offset, char *buf, quint64 len)
{
    if (offset > m_size || len > m_size - offset)
        return false;
    QMutexLocker lock(&m_lock);
    QByteArray block(qsizetype(kBlock), Qt::Uninitialized);
    for (quint64 done = 0; done < len;) {
        const quint64 at = offset + done;
        const quint64 blockStart = at / kBlock * kBlock;
        const quint64 blockLength = std::min(kBlock, m_size - blockStart);
        const quint64 inBlock = at - blockStart, take = std::min(blockLength - inBlock, len - done);
        const char status = m_map.statusAt(blockStart);
        if (status == RescueMap::Finished && m_map.statusAt(blockStart + blockLength - 1) == RescueMap::Finished) {
            // In the image already: from there.
            if (!blockio::readAt(m_image, buf + done, take, at))
                return false;
        } else if (status == RescueMap::NonTrimmed || status == RescueMap::Bad) {
            // The drive wouldn't read all of it before: not asked again. The image has what it
            // did read, and zeros where it wouldn't.
            if (!blockio::readAt(m_image, buf + done, take, at))
                return false;
        } else {
            // From the drive, into the image, then marked: copied, or failed for Rescue Copy to try again.
            m_drive->read(blockStart, block.data(), blockLength);
            const bool bad = m_drive->unreadable(blockStart, blockLength);
            if (!blockio::writeAt(m_image, block.constData(), blockLength, blockStart))
                return false;
            m_map.set(blockStart, blockLength, bad ? RescueMap::NonTrimmed : RescueMap::Finished);
            std::copy(block.constData() + inBlock, block.constData() + inBlock + take, buf + done);
        }
        done += take;
    }
    // The map, every few seconds.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_savedAt > 5000) {
        m_savedAt = now;
        QString ignored;
        m_map.save(m_mapPath, &ignored);
    }
    return true;
}

bool ReadThrough::saveMap(QString *why)
{
    QMutexLocker lock(&m_lock);
    QString problem;
    if (m_map.save(m_mapPath, &problem))
        return true;
    if (why)
        *why = problem;
    return false;
}

quint64 ReadThrough::copied() const
{
    QMutexLocker lock(&m_lock);
    return m_map.total(RescueMap::Finished);
}

} // namespace lost
