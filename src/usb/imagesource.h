// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

#include <memory>

// A disk image for Write Image to USB: as it is (.iso, .img), or compressed (.xz, .gz, .bz2,
// .lzma, .zst, or a .zip with the image inside, the way Raspberry Pi and many others hand them
// out). Compressed ones are unpacked on the way with libarchive, so nothing extra lands on disk.
class ImageSource
{
public:
    // Opens `path`. On failure returns nullptr and says why in `error`.
    static std::unique_ptr<ImageSource> open(const QString &path, QString *error);
    virtual ~ImageSource() = default;

    // The next part of the image, up to `max` bytes: how many came, 0 at the end, -1 on error.
    virtual qint64 read(char *buffer, qint64 max) = 0;
    // The image's size once unpacked; 0 when the format doesn't say (xz, gzip, bzip2).
    virtual quint64 size() const = 0;
    // How much of the file itself has been read, for progress when size() is 0.
    virtual quint64 fileDone() const = 0;
    quint64 fileSize() const { return m_fileSize; }
    // "xz", "zip"...; empty for an image that isn't compressed.
    QString compression() const { return m_compression; }
    // The name inside a zip, else empty.
    QString innerName() const { return m_innerName; }
    QString error() const { return m_error; }

protected:
    quint64 m_fileSize = 0;
    QString m_compression;
    QString m_innerName;
    QString m_error;
};

// Whether the image's first sector ends in 55 AA, like everything a PC's firmware can start
// from a USB stick as it is (hybrid ISOs, disk images). Reads just that much.
bool hasBootSignature(const QString &path);
