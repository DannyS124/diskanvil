// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "imagewriter.h"

#include "blockio.h"
#include "imagesource.h"

#include <QCryptographicHash>
#include <QFile>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr qint64 kChunk = 4 * 1024 * 1024;
using blockio::readFull;
using blockio::writeAll;

} // namespace

ImageWriter::ImageWriter(const QString &imagePath, int fd, bool verify, const checksums::Expected &expected)
    : m_imagePath(imagePath)
    , m_fd(fd)
    , m_verify(verify)
    , m_expected(expected)
{
}

ImageWriter::~ImageWriter()
{
    if (m_fd >= 0)
        ::close(m_fd);
}

bool ImageWriter::fail(const QString &message)
{
    emit finished(false, message);
    return false;
}

void ImageWriter::run()
{
    QByteArray buffer(kChunk, Qt::Uninitialized);

    // A wrong or broken download is caught before anything is written. The checksum is for
    // the file as it was downloaded, compressed or not.
    if (m_expected.isSet()) {
        QFile file(m_imagePath);
        if (!file.open(QIODevice::ReadOnly)) {
            fail(tr("Couldn't open the image: %1").arg(file.errorString()));
            return;
        }
        QCryptographicHash hash(m_expected.algorithm);
        const quint64 fileSize = quint64(file.size());
        quint64 done = 0;
        while (!file.atEnd()) {
            if (m_cancel) {
                fail(tr("Cancelled. Nothing was written."));
                return;
            }
            const qint64 n = file.read(buffer.data(), kChunk);
            if (n < 0) {
                fail(tr("Couldn't read the image"));
                return;
            }
            hash.addData(QByteArrayView(buffer.constData(), n));
            done += quint64(n);
            emit progress(tr("Checking the download"), done, fileSize);
        }
        if (hash.result().toHex() != m_expected.hex) {
            fail(tr("The image's %1 doesn't match the one you gave. The download may be broken. Nothing was written.")
                     .arg(checksums::name(m_expected.algorithm)));
            return;
        }
    }

    QString error;
    const std::unique_ptr<ImageSource> image = ImageSource::open(m_imagePath, &error);
    if (!image) {
        fail(tr("Couldn't open the image: %1").arg(error));
        return;
    }
    const quint64 deviceSize = blockio::deviceSize(m_fd);
    const quint64 size = image->size(); // 0: a compressed format that doesn't say
    if (deviceSize == 0 || size > deviceSize) {
        fail(tr("The image is bigger than the drive"));
        return;
    }
    // Unpacking gives small pieces; fill the buffer before each write.
    auto readChunk = [&image, &buffer]() -> qint64 {
        qint64 got = 0;
        while (got < kChunk) {
            const qint64 n = image->read(buffer.data() + got, kChunk - got);
            if (n < 0)
                return -1;
            if (n == 0)
                break;
            got += n;
        }
        return got;
    };

    blockio::seekTo(m_fd, 0);
    QCryptographicHash written(QCryptographicHash::Sha256);
    quint64 done = 0;
    const QString phase = image->compression().isEmpty() ? tr("Writing") : tr("Unpacking and writing");
    for (;;) {
        if (m_cancel) {
            fail(tr("Cancelled. The drive won't work until you format it."));
            return;
        }
        const qint64 n = readChunk();
        if (n < 0) {
            fail(tr("Writing failed: %1").arg(image->error()));
            return;
        }
        if (n == 0)
            break;
        if (done + quint64(n) > deviceSize) {
            fail(tr("The image is bigger than the drive. The drive won't work until you format it."));
            return;
        }
        if (!writeAll(m_fd, buffer.constData(), n)) {
            fail(tr("Writing failed: %1").arg(QString::fromLocal8Bit(strerror(errno))));
            return;
        }
        written.addData(QByteArrayView(buffer.constData(), n));
        done += quint64(n);
        if (size)
            emit progress(phase, done, size);
        else
            emit progress(phase, image->fileDone(), image->fileSize());
    }

    emit progress(tr("Finishing (flushing to the drive)"), 1, 1);
    if (!blockio::flush(m_fd)) {
        fail(tr("Writing failed: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        return;
    }

    if (m_verify) {
        // Drop the cached copy so the check reads what actually landed on the drive.
        blockio::dropCache(m_fd);
        blockio::seekTo(m_fd, 0);
        QCryptographicHash readBack(QCryptographicHash::Sha256);
        quint64 checked = 0;
        while (checked < done) {
            if (m_cancel) {
                fail(tr("Verification cancelled. The image was written but not checked."));
                return;
            }
            const qint64 want = qint64(std::min<quint64>(kChunk, done - checked));
            const qint64 n = readFull(m_fd, buffer.data(), want);
            if (n != want) {
                fail(tr("Couldn't read the drive back to verify it"));
                return;
            }
            readBack.addData(QByteArrayView(buffer.constData(), n));
            checked += quint64(n);
            emit progress(tr("Verifying"), checked, done);
        }
        if (readBack.result() != written.result()) {
            fail(tr("Verification failed: what's on the drive doesn't match the image. The drive may be faulty."));
            return;
        }
    }

    ::close(m_fd);
    m_fd = -1;
    emit finished(true, m_verify ? tr("Done. The image was written and verified.") : tr("Done. The image was written."));
}
