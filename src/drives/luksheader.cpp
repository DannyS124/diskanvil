// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "luksheader.h"

#include "blockio.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

#include <cstring>

namespace luksheader {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("luksheader", text);
}

bool isLuks(const QByteArray &d)
{
    return d.size() >= 512 && d.startsWith(QByteArray("LUKS\xBA\xBE", 6));
}

quint64 be64(const QByteArray &d, qsizetype at)
{
    return at + 8 <= d.size() ? qFromBigEndian<quint64>(d.constData() + at) : 0;
}

quint32 be32(const QByteArray &d, qsizetype at)
{
    return at + 4 <= d.size() ? qFromBigEndian<quint32>(d.constData() + at) : 0;
}

// `size` bytes from the start, read in whole 4 KiB pieces into aligned memory (what O_DIRECT needs).
QByteArray readStart(int fd, quint64 size)
{
    const quint64 rounded = (size + 4095) / 4096 * 4096;
    blockio::Buffer buf = blockio::alignedBuffer(rounded);
    if (!buf || !blockio::readAt(fd, buf.get(), rounded, 0))
        return {};
    return QByteArray(buf.get(), qsizetype(size));
}

QString text(const QByteArray &d, qsizetype at, qsizetype size)
{
    const QByteArray field = d.mid(at, size);
    const qsizetype end = field.indexOf('\0');
    return QString::fromLatin1(end < 0 ? field : field.left(end));
}

} // namespace

quint64 needed(const QByteArray &first)
{
    if (!isLuks(first))
        return 0;
    const quint16 version = qFromBigEndian<quint16>(first.constData() + 6);
    if (version == 1)
        return 4096;
    const quint64 header = be64(first, 8);
    return version == 2 && header >= 16384 && header <= 4 * 1024 * 1024 ? header : 0;
}

Info inspect(const QByteArray &d)
{
    Info info;
    if (!isLuks(d)) {
        info.problem = tr("it isn't a LUKS header");
        return info;
    }
    info.version = qFromBigEndian<quint16>(d.constData() + 6);
    info.uuid = text(d, 168, 40);
    if (info.version == 1) {
        // The header ends after the last of its eight keyslots' key material (each at a sector,
        // its key split into "stripes"), to the next 4 KiB, as cryptsetup counts it; the data starts later.
        info.dataOffset = quint64(be32(d, 104)) * 512;
        const quint32 keyBytes = be32(d, 108);
        for (int slot = 0; slot < 8; ++slot) {
            const qsizetype at = 208 + slot * 48;
            const quint64 end = quint64(be32(d, at + 40)) * 512 + (quint64(keyBytes) * be32(d, at + 44) + 511) / 512 * 512;
            info.size = std::max(info.size, end);
        }
        info.size = (info.size + 4095) / 4096 * 4096; // cryptsetup's keyslot alignment
        if (info.size < 4096 || info.size > 64ull * 1024 * 1024)
            info.problem = tr("its header doesn't add up");
        return info;
    }
    if (info.version != 2) {
        info.problem = tr("LUKS version %1 isn't known").arg(info.version);
        return info;
    }
    // LUKS2: a binary header and its JSON, twice, then the keyslots area; the JSON says how big
    // that is, and where the data starts.
    const quint64 header = be64(d, 8);
    if (header < 16384 || header > 4 * 1024 * 1024 || quint64(d.size()) < header) {
        info.problem = tr("its header doesn't add up");
        return info;
    }
    const QByteArray json = d.mid(4096, qsizetype(header - 4096));
    const QJsonObject root = QJsonDocument::fromJson(json.left(json.indexOf('\0') < 0 ? json.size() : json.indexOf('\0'))).object();
    const quint64 keyslots = root.value(QLatin1String("config")).toObject().value(QLatin1String("keyslots_size")).toString().toULongLong();
    if (root.isEmpty() || keyslots == 0) {
        info.problem = tr("its header's description can't be read");
        return info;
    }
    info.size = 2 * header + keyslots;
    quint64 data = 0;
    const QJsonObject segments = root.value(QLatin1String("segments")).toObject();
    for (auto it = segments.begin(); it != segments.end(); ++it) {
        const quint64 offset = it.value().toObject().value(QLatin1String("offset")).toString().toULongLong();
        if (data == 0 || offset < data)
            data = offset;
    }
    info.dataOffset = data;
    if (info.size > 64ull * 1024 * 1024)
        info.problem = tr("its header doesn't add up");
    return info;
}

QByteArray read(int fd, Info *info)
{
    *info = Info();
    const QByteArray first = readStart(fd, 4096);
    if (first.isEmpty()) {
        info->problem = tr("its start couldn't be read");
        return {};
    }
    const quint64 head = needed(first);
    if (head == 0) {
        info->problem = tr("there's no LUKS header at its start");
        return {};
    }
    *info = inspect(head <= 4096 ? first : readStart(fd, head));
    if (!info->valid())
        return {};
    const QByteArray all = readStart(fd, info->size);
    if (all.isEmpty())
        info->problem = tr("its header couldn't be read");
    return all;
}

bool write(int fd, const QByteArray &backup, QString *error)
{
    const Info info = inspect(backup.left(qsizetype(std::min<quint64>(quint64(backup.size()), 4 * 1024 * 1024 + 4096))));
    if (!info.valid() || quint64(backup.size()) != info.size) {
        *error = info.problem.isEmpty() ? tr("the copy isn't whole") : info.problem;
        return false;
    }
    const quint64 rounded = (quint64(backup.size()) + 4095) / 4096 * 4096;
    blockio::Buffer buf = blockio::alignedBuffer(rounded);
    if (!buf) {
        *error = tr("out of memory");
        return false;
    }
    // A size that isn't whole 4 KiB keeps what's on the drive after it.
    if (rounded != quint64(backup.size()) && !blockio::readAt(fd, buf.get(), rounded, 0)) {
        *error = tr("its start couldn't be read");
        return false;
    }
    memcpy(buf.get(), backup.constData(), size_t(backup.size()));
    if (!blockio::writeAt(fd, buf.get(), rounded, 0) || !blockio::flush(fd)) {
        *error = tr("writing it failed");
        return false;
    }
    Info again;
    if (read(fd, &again) != backup) {
        *error = tr("it didn't read back the same");
        return false;
    }
    return true;
}

} // namespace luksheader
