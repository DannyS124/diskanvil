// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "imagesource.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>

#include <archive.h>
#include <archive_entry.h>
#include <zstd.h>

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("ImageSource", text);
}

// What the file starts with decides; the name only for .lzma, which has no marker of its own.
QString compressionOf(const QString &path)
{
    QFile f(path);
    QByteArray head;
    if (f.open(QIODevice::ReadOnly))
        head = f.read(6);
    if (head.startsWith("\xFD" "7zXZ"))
        return QStringLiteral("xz");
    if (head.startsWith("\x1F\x8B"))
        return QStringLiteral("gzip");
    if (head.startsWith("BZh"))
        return QStringLiteral("bzip2");
    if (head.startsWith("\x28\xB5\x2F\xFD"))
        return QStringLiteral("zstd");
    if (head.startsWith("PK\x03\x04"))
        return QStringLiteral("zip");
    if (path.endsWith(QLatin1String(".lzma"), Qt::CaseInsensitive))
        return QStringLiteral("lzma");
    return {};
}

class PlainImage : public ImageSource
{
public:
    bool open(const QString &path)
    {
        m_file.setFileName(path);
        if (!m_file.open(QIODevice::ReadOnly)) {
            m_error = m_file.errorString();
            return false;
        }
        m_fileSize = quint64(m_file.size());
        return true;
    }
    qint64 read(char *buffer, qint64 max) override
    {
        const qint64 n = m_file.read(buffer, max);
        if (n < 0)
            m_error = m_file.errorString();
        return n;
    }
    quint64 size() const override { return m_fileSize; }
    quint64 fileDone() const override { return quint64(m_file.pos()); }

private:
    QFile m_file;
};

class ArchiveImage : public ImageSource
{
public:
    ~ArchiveImage() override
    {
        if (m_archive)
            archive_read_free(m_archive);
    }

    bool open(const QString &path, const QString &compression)
    {
        m_compression = compression;
        m_fileSize = quint64(QFileInfo(path).size());
        m_archive = archive_read_new();
        if (compression == QLatin1String("zip")) {
            archive_read_support_format_zip(m_archive);
        } else {
            archive_read_support_filter_all(m_archive);
            archive_read_support_format_raw(m_archive);
        }
        if (archive_read_open_filename(m_archive, QFile::encodeName(path).constData(), 1024 * 1024) != ARCHIVE_OK)
            return fail();
        // A zip can hold more than the image (a README, a checksum file, the macOS "__MACOSX"
        // copies): the first .img or .iso is it, else the first file.
        archive_entry *entry = nullptr;
        const bool zip = compression == QLatin1String("zip");
        bool found = false;
        int status = ARCHIVE_OK;
        while (!found && (status = archive_read_next_header(m_archive, &entry)) == ARCHIVE_OK) {
            if (zip && archive_entry_filetype(entry) != AE_IFREG)
                continue;
            const QString name = QString::fromUtf8(archive_entry_pathname_utf8(entry) ? archive_entry_pathname_utf8(entry) : "");
            if (name.startsWith(QLatin1String("__MACOSX/")))
                continue;
            const bool image = name.endsWith(QLatin1String(".img"), Qt::CaseInsensitive) || name.endsWith(QLatin1String(".iso"), Qt::CaseInsensitive)
                || name.endsWith(QLatin1String(".raw"), Qt::CaseInsensitive) || !zip;
            if (!image && m_firstOther.isEmpty())
                m_firstOther = name;
            if (image) {
                found = true;
                if (zip) {
                    m_innerName = name;
                    if (archive_entry_size_is_set(entry))
                        m_size = quint64(archive_entry_size(entry));
                }
            }
        }
        if (!found) {
            if (status != ARCHIVE_EOF && status != ARCHIVE_OK)
                return fail();
            m_error = m_firstOther.isEmpty() ? tr("The zip file has no disk image in it.")
                                             : tr("The zip file has no .img or .iso in it (the first file is %1).").arg(m_firstOther);
            return false;
        }
        if (compression == QLatin1String("zstd"))
            m_size = zstdSize(path);
        return true;
    }

    qint64 read(char *buffer, qint64 max) override
    {
        const la_ssize_t n = archive_read_data(m_archive, buffer, size_t(max));
        if (n < 0) {
            fail();
            return -1;
        }
        return qint64(n);
    }
    quint64 size() const override { return m_size; }
    quint64 fileDone() const override { return quint64(archive_filter_bytes(m_archive, -1)); }

private:
    bool fail()
    {
        const char *why = archive_error_string(m_archive);
        m_error = why ? tr("Couldn't unpack the image: %1").arg(QString::fromUtf8(why)) : tr("Couldn't unpack the image.");
        return false;
    }
    // zstd puts the unpacked size in the frame header (when it was known while packing).
    static quint64 zstdSize(const QString &path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return 0;
        const QByteArray head = f.read(18); // the longest a frame header gets
        const unsigned long long size = ZSTD_getFrameContentSize(head.constData(), size_t(head.size()));
        return size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR ? 0 : quint64(size);
    }

    archive *m_archive = nullptr;
    quint64 m_size = 0;
    QString m_firstOther;
};

} // namespace

std::unique_ptr<ImageSource> ImageSource::open(const QString &path, QString *error)
{
    const QString compression = compressionOf(path);
    if (compression.isEmpty()) {
        auto plain = std::make_unique<PlainImage>();
        if (plain->open(path))
            return plain;
        if (error)
            *error = plain->error();
        return nullptr;
    }
    auto packed = std::make_unique<ArchiveImage>();
    if (packed->open(path, compression))
        return packed;
    if (error)
        *error = packed->error();
    return nullptr;
}

bool hasBootSignature(const QString &path)
{
    const std::unique_ptr<ImageSource> image = ImageSource::open(path, nullptr);
    if (!image)
        return false;
    QByteArray sector(512, '\0');
    qint64 got = 0;
    while (got < 512) {
        const qint64 n = image->read(sector.data() + got, 512 - got);
        if (n <= 0)
            return false;
        got += n;
    }
    return uchar(sector[510]) == 0x55 && uchar(sector[511]) == 0xAA;
}
