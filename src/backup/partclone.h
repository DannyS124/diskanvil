// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Clonezilla's backups, restored: a folder of partclone images (sda1.ext4-ptcl-img.gz.aa, .ab...),
// each a partition's used blocks after a header and a bitmap of which blocks are used, with
// CRC-32s along the way. Packed with gzip, zstd, xz, bzip2 or lz4 and cut into parts, all read
// through libarchive. Our own reader of partclone's format (version 2, partclone 0.2 and later);
// Clonezilla's raw "dd" images restore too.

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>

namespace partclone {

struct Header {
    QString fs;              // "EXTFS", "FAT32", "NTFS"...
    QString version;         // partclone's own ("0.3.36")
    quint64 deviceSize = 0;  // the partition it came from
    quint64 totalBlocks = 0;
    quint64 usedBlocks = 0;
    quint32 blockSize = 0;
    quint16 checksumMode = 0; // 0x20: CRC-32
    quint16 checksumSize = 0;
    quint32 blocksPerChecksum = 0;
    bool reseed = true;
    QString problem;
    bool valid() const { return problem.isEmpty() && blockSize > 0; }
};
inline constexpr int kHeaderSize = 110;
Header parseHeader(const QByteArray &head);

// One partition's image in a Clonezilla folder.
struct Image {
    QString partition; // "sda1"
    QString fs;        // as Clonezilla named it: "ext4", "vfat", "ntfs"
    bool raw = false;  // a "dd" image: the partition's bytes, not partclone's format
    QStringList files; // its parts, in order
    Header header;     // read from it (not for raw ones)
};
QVector<Image> scanFolder(const QString &folder);

struct Result {
    bool ok = false;
    QString error;
    quint64 written = 0; // bytes
};

// Restores an image onto `fd` (a partition at least as big as it was): only the used blocks are
// written, each group's CRC checked as it comes; a mismatch stops it.
class Restore : public QObject
{
    Q_OBJECT
public:
    Restore(const Image &image, int fd, quint64 targetSize);
    void cancel() { m_cancel = true; }

public slots:
    void run();

signals:
    void progress(quint64 done, quint64 total);
    void finished(const partclone::Result &result);

private:
    Image m_image;
    int m_fd;
    quint64 m_targetSize;
    std::atomic<bool> m_cancel{false};
};

} // namespace partclone

Q_DECLARE_METATYPE(partclone::Result)
