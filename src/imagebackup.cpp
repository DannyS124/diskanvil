// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "imagebackup.h"

#include "blockcopy.h"
#include "blockio.h"
#include "format.h"
#include "gpt.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <zstd.h>

namespace {

constexpr int kFormatVersion = 1;
constexpr int kZstdHeaderMax = 18; // ZSTD_FRAMEHEADERSIZE_MAX, which is only in zstd's static API

QString hex(const QByteArray &hash)
{
    return QString::fromLatin1(hash.toHex());
}

} // namespace

QJsonObject BackupInfo::toJson() const
{
    return {
        {QStringLiteral("format"), QStringLiteral("diskforge-backup")},
        {QStringLiteral("version"), kFormatVersion},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("device"), device},
        {QStringLiteral("model"), model},
        {QStringLiteral("serial"), serial},
        {QStringLiteral("label"), label},
        {QStringLiteral("filesystem"), fsType},
        {QStringLiteral("table"), tableType},
        {QStringLiteral("size"), QString::number(size)}, // as text: JSON numbers lose precision past 2^53
        {QStringLiteral("sectorSize"), sectorSize},
        {QStringLiteral("sha256"), sha256},
        {QStringLiteral("fileSha256"), fileSha256},
        {QStringLiteral("app"), app},
        {QStringLiteral("created"), created.toString(Qt::ISODate)},
    };
}

BackupInfo BackupInfo::fromJson(const QJsonObject &j)
{
    BackupInfo i;
    if (j.value(QStringLiteral("format")).toString() != QLatin1String("diskforge-backup")) {
        i.error = QObject::tr("Not a DiskForge backup description");
        return i;
    }
    if (j.value(QStringLiteral("version")).toInt() > kFormatVersion) {
        i.error = QObject::tr("Made by a newer DiskForge; update to restore it");
        return i;
    }
    // The file could come from anywhere, so the names in it are cleaned like a drive's.
    i.kind = j.value(QStringLiteral("kind")).toString();
    i.device = cleanName(j.value(QStringLiteral("device")).toString());
    i.model = cleanName(j.value(QStringLiteral("model")).toString());
    i.serial = cleanName(j.value(QStringLiteral("serial")).toString());
    i.label = cleanName(j.value(QStringLiteral("label")).toString());
    i.fsType = cleanName(j.value(QStringLiteral("filesystem")).toString());
    i.tableType = j.value(QStringLiteral("table")).toString();
    i.size = j.value(QStringLiteral("size")).toString().toULongLong();
    i.sectorSize = j.value(QStringLiteral("sectorSize")).toInt(512);
    i.sha256 = j.value(QStringLiteral("sha256")).toString();
    i.fileSha256 = j.value(QStringLiteral("fileSha256")).toString();
    i.app = cleanName(j.value(QStringLiteral("app")).toString());
    i.created = QDateTime::fromString(j.value(QStringLiteral("created")).toString(), Qt::ISODate);
    if (i.size == 0)
        i.error = QObject::tr("The backup description is incomplete");
    return i;
}

namespace imagebackup {

QString infoPath(const QString &imagePath)
{
    return imagePath + QStringLiteral(".json");
}

BackupInfo describe(const QString &imagePath)
{
    const bool zst = imagePath.endsWith(QLatin1String(".zst"), Qt::CaseInsensitive);
    QFile json(infoPath(imagePath));
    if (json.open(QIODevice::ReadOnly)) {
        BackupInfo i = BackupInfo::fromJson(QJsonDocument::fromJson(json.read(64 * 1024)).object());
        i.compressed = zst;
        return i;
    }
    BackupInfo i;
    i.compressed = zst;
    QFile f(imagePath);
    if (!f.open(QIODevice::ReadOnly)) {
        i.error = f.errorString();
        return i;
    }
    if (!zst) {
        i.size = quint64(f.size());
        return i;
    }
    // zstd records the uncompressed size in the frame header when it was known up front.
    const QByteArray head = f.read(kZstdHeaderMax);
    const unsigned long long size = ZSTD_getFrameContentSize(head.constData(), size_t(head.size()));
    if (size == ZSTD_CONTENTSIZE_ERROR)
        i.error = QObject::tr("Not a zstd file");
    else if (size == ZSTD_CONTENTSIZE_UNKNOWN)
        i.error = QObject::tr("The file doesn't say how big it is unpacked");
    else
        i.size = size;
    return i;
}

BackupInfo infoFor(const Disk &disk, const Volume *volume)
{
    BackupInfo i;
    i.kind = volume ? QStringLiteral("partition") : QStringLiteral("disk");
    i.device = volume ? volume->device : disk.device;
    i.model = disk.model;
    i.serial = disk.serial;
    i.label = volume ? volume->label : QString();
    i.fsType = volume ? volume->fsType : QString();
    i.tableType = volume ? QString() : disk.tableType;
    i.size = volume ? volume->size : disk.size;
    i.sectorSize = disk.sectorSize;
    i.app = QStringLiteral("DiskForge " APP_VERSION);
    i.created = QDateTime::currentDateTimeUtc();
    return i;
}

} // namespace imagebackup

BackupJob::BackupJob(int sourceFd, const QString &imagePath, const BackupInfo &info)
    : m_source(sourceFd)
    , m_path(imagePath)
    , m_info(info)
{
}

BackupJob::~BackupJob()
{
    if (m_source >= 0)
        ::close(m_source);
}

void BackupJob::run()
{
    auto fail = [this](const QString &message) {
        QFile::remove(m_path);
        QFile::remove(imagebackup::infoPath(m_path));
        emit finished(false, message);
    };
    const int out = blockio::openFile(m_path, blockio::FileMode::Replace);
    if (out < 0)
        return fail(tr("Couldn't create %1: %2").arg(m_path, QString::fromLocal8Bit(std::strerror(errno))));

    blockcopy::Options o;
    o.sourceFd = m_source;
    o.targetFd = out;
    o.targetFormat = blockcopy::Format::Zstd;
    o.extents = {{0, m_info.size, 0}};
    const QString phase = tr("Backing up");
    const blockcopy::Result r = blockcopy::copy(o, m_cancel, [&](quint64 done, quint64 total) { emit progress(phase, done, total); });
    ::close(out);
    if (!r.ok)
        return fail(r.cancelled ? tr("Stopped. The unfinished backup was removed.") : r.error);

    m_info.sha256 = hex(r.dataSha256);
    m_info.fileSha256 = hex(r.fileSha256);
    QSaveFile json(imagebackup::infoPath(m_path));
    if (!json.open(QIODevice::WriteOnly) || json.write(QJsonDocument(m_info.toJson()).toJson()) < 0 || !json.commit())
        return fail(tr("Couldn't save the backup description: %1").arg(json.errorString()));
    emit finished(true, tr("Backed up %1 into %2 (%3).")
                            .arg(formatSize(r.bytes), QFileInfo(m_path).fileName(), formatSize(quint64(QFileInfo(m_path).size()))));
}

RestoreJob::RestoreJob(const QString &imagePath, const BackupInfo &info, int targetFd, bool checkFirst, bool verify)
    : m_path(imagePath)
    , m_info(info)
    , m_target(targetFd)
    , m_checkFirst(checkFirst)
    , m_verify(verify)
{
}

RestoreJob::~RestoreJob()
{
    if (m_target >= 0)
        ::close(m_target);
}

void RestoreJob::run()
{
    const blockcopy::Format format = m_info.compressed ? blockcopy::Format::Zstd : blockcopy::Format::Raw;
    const QVector<blockcopy::Extent> whole = {{0, m_info.size, 0}};
    const QByteArray expected = QByteArray::fromHex(m_info.sha256.toLatin1());

    if (blockio::deviceSize(m_target) < m_info.size) {
        emit finished(false, tr("The backup (%1) is bigger than where it's going (%2).")
                                 .arg(formatSize(m_info.size), formatSize(blockio::deviceSize(m_target))), false);
        return;
    }

    auto openImage = [this]() { return blockio::openFile(m_path, blockio::FileMode::Read); };
    if (m_checkFirst) {
        const int in = openImage();
        blockcopy::Options o;
        o.sourceFd = in;
        o.sourceFormat = format;
        o.extents = whole;
        const QString phase = tr("Checking the backup");
        const blockcopy::Result check = blockcopy::copy(o, m_cancel, [&](quint64 done, quint64 total) { emit progress(phase, done, total); });
        ::close(in);
        if (!check.ok) {
            emit finished(false, check.cancelled ? tr("Stopped. Nothing was written.") : tr("Nothing was written. %1").arg(check.error), false);
            return;
        }
        if (!expected.isEmpty() && check.dataSha256 != expected) {
            emit finished(false, tr("The backup doesn't match its checksum, so nothing was written. It was changed or damaged after it was made."), false);
            return;
        }
    }

    const int in = openImage();
    if (in < 0) {
        emit finished(false, tr("Couldn't open %1").arg(m_path), false);
        return;
    }
    blockcopy::Options o;
    o.sourceFd = in;
    o.sourceFormat = format;
    o.targetFd = m_target;
    o.extents = whole;
    const QString writing = tr("Restoring");
    const blockcopy::Result r = blockcopy::copy(o, m_cancel, [&](quint64 done, quint64 total) { emit progress(writing, done, total); });
    ::close(in);
    if (!r.ok) {
        emit finished(false, r.cancelled ? tr("Stopped partway. What's there now is incomplete; restore again or format it.") : r.error, true);
        return;
    }
    if (!expected.isEmpty() && r.dataSha256 != expected) {
        emit finished(false, tr("The backup changed while it was being restored. Restore it again."), true);
        return;
    }

    if (m_verify) {
        const QString checking = tr("Checking");
        const blockcopy::Result back = blockcopy::readBack(m_target, whole, m_cancel, [&](quint64 done, quint64 total) { emit progress(checking, done, total); });
        if (!back.ok || back.dataSha256 != r.dataSha256) {
            emit finished(false, back.cancelled ? tr("Restored, but the check was stopped.") : tr("What was written doesn't read back the same. The drive may be failing."), true);
            return;
        }
    }

    // A whole disk restored onto a bigger one: put the GPT backup at the real end.
    if (m_info.kind == QLatin1String("disk") && gpt::isGpt(m_target, m_info.sectorSize) && blockio::deviceSize(m_target) > m_info.size) {
        const gpt::Result g = gpt::relocateBackup(m_target, m_info.sectorSize, false);
        if (!g.ok) {
            emit finished(false, tr("Restored, but the partition table couldn't be finished: %1").arg(g.error), true);
            return;
        }
    }
    emit finished(true, tr("Restored %1.").arg(formatSize(r.bytes)), true);
}
