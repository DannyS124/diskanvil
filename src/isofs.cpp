// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "isofs.h"

#include "blockio.h"

#include <QCoreApplication>
#include <QSet>

namespace {

constexpr quint64 kSector = 2048;
constexpr int kMaxDepth = 32;
constexpr int kMaxEntries = 200000;
constexpr quint64 kMaxFolderSize = 16 * 1024 * 1024;

QString tr(const char *text)
{
    return QCoreApplication::translate("isofs", text);
}

quint32 le32(const uchar *p)
{
    return quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16 | quint32(p[3]) << 24;
}

bool readAt(int fd, quint64 offset, QByteArray &buffer)
{
    return blockio::readAt(fd, buffer.data(), quint64(buffer.size()), offset);
}

// Joliet names are UCS-2, big-endian, with ";1" (the file's version) on the end.
QString jolietName(const uchar *bytes, int length)
{
    QString name;
    for (int i = 0; i + 1 < length; i += 2)
        name += QChar(char16_t(bytes[i] << 8 | bytes[i + 1]));
    const int semicolon = name.lastIndexOf(QLatin1Char(';'));
    if (semicolon >= 0)
        name.truncate(semicolon);
    return name;
}

bool saneName(const QString &name)
{
    if (name.isEmpty() || name.size() > 255 || name == QLatin1String(".") || name == QLatin1String(".."))
        return false;
    for (const QChar c : name) {
        if (c.unicode() < 0x20 || c == QLatin1Char('/') || c == QLatin1Char('\\'))
            return false;
    }
    return true;
}

struct Walker {
    int fd;
    quint64 imageSize;
    isofs::Listing &out;
    QSet<quint32> seen;

    bool fail(const QString &error)
    {
        out.error = error;
        return false;
    }

    bool inside(quint64 start, quint64 length) const
    {
        return start <= imageSize && length <= imageSize - start;
    }

    bool walk(quint32 extent, quint32 size, const QString &prefix, int depth)
    {
        if (depth > kMaxDepth)
            return fail(tr("The folders inside the image go too deep."));
        if (seen.contains(extent))
            return fail(tr("The image is damaged: a folder contains itself."));
        seen.insert(extent);
        if (size > kMaxFolderSize || !inside(quint64(extent) * kSector, size))
            return fail(tr("The image is damaged: a folder points outside it."));

        QByteArray buffer(qsizetype((size + kSector - 1) / kSector * kSector), '\0');
        if (!readAt(fd, quint64(extent) * kSector, buffer))
            return fail(tr("Couldn't read the image."));
        const auto *data = reinterpret_cast<const uchar *>(buffer.constData());

        QSet<QString> names;
        quint32 pos = 0;
        while (pos < size) {
            const quint32 length = data[pos];
            if (length == 0) {
                pos = (pos / kSector + 1) * kSector; // records don't cross sectors; the rest is padding
                continue;
            }
            if (length < 34 || pos + length > size)
                return fail(tr("The image is damaged: a folder entry is cut short."));
            const uchar *record = data + pos;
            pos += length;

            const int nameLength = record[32];
            if (33 + nameLength > int(length))
                return fail(tr("The image is damaged: a name runs past its entry."));
            if (nameLength == 1 && (record[33] == 0 || record[33] == 1))
                continue; // "." and ".."
            const uchar flags = record[25];
            if (flags & 0x04)
                continue; // an "associated file" (old Mac resource forks)
            if (flags & 0x80)
                return fail(tr("The image has files over 4 GB, which a FAT32 stick can't hold."));

            const QString name = jolietName(record + 33, nameLength);
            if (!saneName(name))
                return fail(tr("The image has a file with a name that isn't allowed."));
            if (names.contains(name))
                return fail(tr("The image has two files called \"%1\" in the same folder.").arg(name));
            names.insert(name);

            isofs::Entry entry;
            entry.path = prefix.isEmpty() ? name : prefix + QLatin1Char('/') + name;
            entry.isDir = flags & 0x02;
            entry.offset = quint64(le32(record + 2)) * kSector;
            entry.size = le32(record + 10);
            if (!entry.isDir && entry.size == 0)
                entry.offset = 0; // where an empty file "starts" is anyone's guess, and never read
            if (entry.size && !inside(entry.offset, entry.size))
                return fail(tr("The image is damaged: %1 points outside it.").arg(entry.path));
            out.entries.push_back(entry);
            if (out.entries.size() > kMaxEntries)
                return fail(tr("The image has too many files."));
            if (entry.isDir && !walk(le32(record + 2), le32(record + 10), entry.path, depth + 1))
                return false;
        }
        return true;
    }
};

} // namespace

isofs::Listing isofs::list(int fd)
{
    Listing out;
    const quint64 imageSize = blockio::deviceSize(fd);
    if (imageSize == 0) {
        out.error = tr("Couldn't read the image.");
        return out;
    }

    // The volume descriptors start at sector 16. The Joliet one has the names as they were; the
    // primary one has the full label (Joliet's only has room for 16 characters), which is the
    // one blkid and so the boot menus go by.
    QByteArray joliet;
    QString primaryLabel;
    QByteArray sector(qsizetype(kSector), '\0');
    for (quint64 i = 16; i < 16 + 64; ++i) {
        if ((i + 1) * kSector > imageSize || !readAt(fd, i * kSector, sector) || sector.mid(1, 5) != "CD001") {
            out.error = i == 16 ? tr("This isn't an ISO image.") : tr("The image is damaged.");
            return out;
        }
        const uchar type = uchar(sector[0]);
        if (type == 255)
            break;
        if (type == 1)
            primaryLabel = QString::fromLatin1(sector.mid(40, 32)).trimmed();
        const QByteArray escape = sector.mid(88, 3);
        if (type == 2 && (escape == "%/@" || escape == "%/C" || escape == "%/E")) {
            joliet = sector;
            break;
        }
    }
    if (joliet.isEmpty()) {
        out.error = tr("This ISO has no Joliet names (the long file names), so it can't be copied file by file.");
        return out;
    }

    const auto *descriptor = reinterpret_cast<const uchar *>(joliet.constData());
    out.volumeId = primaryLabel.isEmpty() ? jolietName(descriptor + 40, 32).trimmed() : primaryLabel;
    const uchar *root = descriptor + 156;
    Walker walker{fd, imageSize, out, {}};
    if (!walker.walk(le32(root + 2), le32(root + 10), QString(), 0))
        out.entries.clear();
    return out;
}

const isofs::Entry *isofs::find(const Listing &listing, const QString &path)
{
    for (const Entry &e : listing.entries) {
        if (e.path == path)
            return &e;
    }
    return nullptr;
}
