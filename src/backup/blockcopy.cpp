// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "blockcopy.h"

#include "blockio.h"
#include "zstdseek.h"

#include <QCryptographicHash>
#include <QObject>
#include <QThread>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <unistd.h>
#include <zstd.h>

namespace blockcopy {

namespace {

constexpr size_t kChunk = 4 * 1024 * 1024;

QString errnoText()
{
    return QString::fromLocal8Bit(std::strerror(errno));
}

// Raw data comes in through one of these: plain reads at an offset, or the next bytes
// of a zstd stream (which can only go forward).
class Reader
{
public:
    virtual ~Reader() = default;
    virtual bool read(char *buf, size_t len, quint64 offset, QString *error) = 0;
    virtual bool finish(QString *) { return true; }
};

class RawReader : public Reader
{
public:
    explicit RawReader(int fd) : m_fd(fd) {}
    bool read(char *buf, size_t len, quint64 offset, QString *error) override
    {
        if (blockio::readAt(m_fd, buf, len, offset))
            return true;
        *error = QObject::tr("Couldn't read at %1 MB: %2").arg(offset / 1000000).arg(errno ? errnoText() : QObject::tr("end of the source"));
        return false;
    }

private:
    int m_fd;
};

class ZstdReader : public Reader
{
public:
    explicit ZstdReader(int fd) : m_fd(fd), m_in(new char[ZSTD_DStreamInSize()]) { ZSTD_initDStream(m_ctx); }
    ~ZstdReader() override { ZSTD_freeDStream(m_ctx); }

    bool read(char *buf, size_t len, quint64 offset, QString *error) override
    {
        if (offset < m_position) {
            *error = QObject::tr("A compressed backup can only be read forwards");
            return false;
        }
        // Skip forward to the requested offset by decompressing into the target buffer.
        while (m_position < offset) {
            const size_t skip = size_t(std::min<quint64>(offset - m_position, len));
            if (!next(buf, skip, error))
                return false;
        }
        return next(buf, len, error);
    }

    bool finish(QString *error) override
    {
        // The frame has to end exactly here (zstd checks its checksum as it ends), with
        // nothing after it.
        char extra;
        while (m_lastResult != 0) {
            if (m_input.pos == m_input.size && !refill(error))
                return error->isEmpty() ? fail(error, QObject::tr("The backup file is cut short")) : false;
            ZSTD_outBuffer out{&extra, 1, 0};
            const size_t r = ZSTD_decompressStream(m_ctx, &out, &m_input);
            if (ZSTD_isError(r))
                return fail(error, QObject::tr("The backup file is damaged (%1)").arg(QString::fromLatin1(ZSTD_getErrorName(r))));
            if (out.pos > 0)
                return fail(error, QObject::tr("The backup holds more data than expected"));
            m_lastResult = r;
        }
        // What comes after may only be skippable frames (the seekable format's table at the end):
        // they unpack to nothing. Anything that unpacks to more is data that shouldn't be there.
        while (m_input.pos < m_input.size || refill(error)) {
            while (m_input.pos < m_input.size) {
                ZSTD_outBuffer out{&extra, 1, 0};
                const size_t r = ZSTD_decompressStream(m_ctx, &out, &m_input);
                if (ZSTD_isError(r) || out.pos > 0)
                    return fail(error, QObject::tr("The backup holds more data than expected"));
                m_lastResult = r;
            }
        }
        if (m_lastResult != 0)
            return error->isEmpty() ? fail(error, QObject::tr("The backup file is cut short")) : false;
        return error->isEmpty();
    }

private:
    bool fail(QString *error, const QString &text)
    {
        *error = text;
        return false;
    }

    bool refill(QString *error)
    {
        if (m_eof)
            return false;
        const qint64 n = blockio::readFull(m_fd, m_in.get(), qint64(ZSTD_DStreamInSize()));
        if (n < 0)
            return fail(error, QObject::tr("Couldn't read the backup file: %1").arg(errnoText()));
        if (n == 0) {
            m_eof = true;
            return false;
        }
        m_input = {m_in.get(), size_t(n), 0};
        return true;
    }

    bool next(char *buf, size_t len, QString *error)
    {
        ZSTD_outBuffer out{buf, len, 0};
        while (out.pos < out.size) {
            if (m_input.pos == m_input.size && !refill(error)) {
                if (error->isEmpty())
                    *error = QObject::tr("The backup file is cut short");
                return false;
            }
            const size_t r = ZSTD_decompressStream(m_ctx, &out, &m_input);
            if (ZSTD_isError(r))
                return fail(error, QObject::tr("The backup file is damaged (%1)").arg(QString::fromLatin1(ZSTD_getErrorName(r))));
            m_lastResult = r;
        }
        m_position += len;
        return true;
    }

    int m_fd;
    ZSTD_DStream *m_ctx = ZSTD_createDStream();
    std::unique_ptr<char[]> m_in;
    ZSTD_inBuffer m_input{nullptr, 0, 0};
    quint64 m_position = 0;
    size_t m_lastResult = 1;
    bool m_eof = false;
};

// And goes out through one of these.
class Writer
{
public:
    virtual ~Writer() = default;
    virtual bool write(const char *buf, size_t len, quint64 offset, QString *error) = 0;
    virtual bool finish(QString *error) = 0;
    QCryptographicHash hash{QCryptographicHash::Sha256};
};

class RawWriter : public Writer
{
public:
    explicit RawWriter(int fd) : m_fd(fd) {}
    bool write(const char *buf, size_t len, quint64 offset, QString *error) override
    {
        if (!blockio::writeAt(m_fd, buf, len, offset)) {
            *error = QObject::tr("Couldn't write at %1 MB: %2").arg(offset / 1000000).arg(errnoText());
            return false;
        }
        hash.addData(QByteArrayView(buf, qsizetype(len)));
        return true;
    }
    bool finish(QString *error) override
    {
        if (blockio::flush(m_fd))
            return true;
        *error = QObject::tr("Couldn't finish writing: %1").arg(errnoText());
        return false;
    }

private:
    int m_fd;
};

// Writes zstd's seekable format (zstdseek.h): a frame for every 8 MiB, and the table of them at
// the end, so the backup can be read from anywhere without unpacking it all.
class ZstdWriter : public Writer
{
public:
    ZstdWriter(int fd, int level, quint64 totalSize)
        : m_fd(fd), m_out(new char[ZSTD_CStreamOutSize()]), m_left(totalSize)
    {
        ZSTD_CCtx_setParameter(m_ctx, ZSTD_c_compressionLevel, level);
        ZSTD_CCtx_setParameter(m_ctx, ZSTD_c_checksumFlag, 1);
        // Uses more cores when libzstd was built with threads; otherwise ignored. Jobs of 2 MiB
        // keep them busy inside each 8 MiB frame.
        ZSTD_CCtx_setParameter(m_ctx, ZSTD_c_nbWorkers, std::clamp(QThread::idealThreadCount() - 1, 0, 4));
        ZSTD_CCtx_setParameter(m_ctx, ZSTD_c_jobSize, 2 * 1024 * 1024);
        startFrame();
    }
    ~ZstdWriter() override { ZSTD_freeCCtx(m_ctx); }

    bool write(const char *buf, size_t len, quint64, QString *error) override
    {
        while (len > 0) {
            const size_t n = std::min<size_t>(len, zstdseek::kFrameSize - m_inFrame);
            ZSTD_inBuffer in{buf, n, 0};
            while (in.pos < in.size) {
                if (!step(&in, ZSTD_e_continue, error))
                    return false;
            }
            m_inFrame += quint32(n);
            buf += n;
            len -= n;
            if (m_inFrame == zstdseek::kFrameSize && !endFrame(error))
                return false;
        }
        return true;
    }

    bool finish(QString *error) override
    {
        if ((m_inFrame > 0 || m_frames.isEmpty()) && !endFrame(error))
            return false;
        const QByteArray table = zstdseek::table(m_frames);
        if (!blockio::writeAll(m_fd, table.constData(), table.size())) {
            *error = QObject::tr("Couldn't write the backup file: %1").arg(errnoText());
            return false;
        }
        hash.addData(table);
        if (!blockio::flush(m_fd)) {
            *error = QObject::tr("Couldn't finish writing: %1").arg(errnoText());
            return false;
        }
        return true;
    }

private:
    // Each frame says how much it holds: 8 MiB, or what's left for the last one.
    void startFrame()
    {
        ZSTD_CCtx_setPledgedSrcSize(m_ctx, std::min<quint64>(m_left, zstdseek::kFrameSize));
        m_frameOut = 0;
    }

    bool endFrame(QString *error)
    {
        ZSTD_inBuffer in{nullptr, 0, 0};
        size_t remaining = 1;
        while (remaining != 0) {
            remaining = 0;
            if (!step(&in, ZSTD_e_end, error, &remaining))
                return false;
        }
        m_frames.push_back({m_frameOut, m_inFrame});
        m_left -= std::min<quint64>(m_left, m_inFrame);
        m_inFrame = 0;
        startFrame();
        return true;
    }

    bool step(ZSTD_inBuffer *in, ZSTD_EndDirective mode, QString *error, size_t *remaining = nullptr)
    {
        ZSTD_outBuffer out{m_out.get(), ZSTD_CStreamOutSize(), 0};
        const size_t r = ZSTD_compressStream2(m_ctx, &out, in, mode);
        if (ZSTD_isError(r)) {
            *error = QObject::tr("Couldn't compress: %1").arg(QString::fromLatin1(ZSTD_getErrorName(r)));
            return false;
        }
        if (remaining)
            *remaining = r;
        if (out.pos > 0) {
            if (!blockio::writeAll(m_fd, m_out.get(), qint64(out.pos))) {
                *error = QObject::tr("Couldn't write the backup file: %1").arg(errnoText());
                return false;
            }
            hash.addData(QByteArrayView(m_out.get(), qsizetype(out.pos)));
            m_frameOut += quint32(out.pos);
        }
        return true;
    }

    int m_fd;
    ZSTD_CCtx *m_ctx = ZSTD_createCCtx();
    std::unique_ptr<char[]> m_out;
    quint64 m_left;           // what's still to come
    quint32 m_inFrame = 0;    // unpacked bytes in the frame being made
    quint32 m_frameOut = 0;   // its compressed bytes so far
    QVector<QPair<quint32, quint32>> m_frames;
};

} // namespace

quint64 totalLength(const QVector<Extent> &extents)
{
    quint64 total = 0;
    for (const Extent &e : extents)
        total += e.length;
    return total;
}

Result copy(const Options &o, const std::atomic<bool> &cancel, const Progress &progress)
{
    Result r;
    const quint64 total = totalLength(o.extents);
    std::unique_ptr<Reader> reader;
    if (o.sourceFormat == Format::Zstd)
        reader = std::make_unique<ZstdReader>(o.sourceFd);
    else
        reader = std::make_unique<RawReader>(o.sourceFd);
    std::unique_ptr<Writer> writer;
    if (o.targetFd >= 0) {
        if (o.targetFormat == Format::Zstd)
            writer = std::make_unique<ZstdWriter>(o.targetFd, o.level, total);
        else
            writer = std::make_unique<RawWriter>(o.targetFd);
    }

    blockio::Buffer buf = blockio::alignedBuffer(kChunk);
    if (!buf) {
        r.error = QObject::tr("Out of memory");
        return r;
    }
    QCryptographicHash data(QCryptographicHash::Sha256);
    for (const Extent &e : o.extents) {
        for (quint64 done = 0; done < e.length;) {
            if (cancel) {
                r.cancelled = true;
                r.error = QObject::tr("Cancelled");
                return r;
            }
            const size_t len = size_t(std::min<quint64>(kChunk, e.length - done));
            if (!reader->read(buf.get(), len, e.source + done, &r.error))
                return r;
            data.addData(QByteArrayView(buf.get(), qsizetype(len)));
            if (writer && !writer->write(buf.get(), len, e.target + done, &r.error))
                return r;
            done += len;
            r.bytes += len;
            if (progress)
                progress(r.bytes, total);
        }
    }
    if (!reader->finish(&r.error))
        return r;
    if (writer && !writer->finish(&r.error))
        return r;
    r.dataSha256 = data.result();
    r.fileSha256 = writer ? writer->hash.result() : r.dataSha256;
    r.ok = true;
    return r;
}

Result readBack(int fd, const QVector<Extent> &extents, const std::atomic<bool> &cancel, const Progress &progress)
{
    // Make the reads come from the device, not from what we just wrote into the cache.
    blockio::dropCache(fd);
    QVector<Extent> back;
    for (const Extent &e : extents)
        back.append({e.target, e.length, 0});
    Options o;
    o.sourceFd = fd;
    o.extents = back;
    return copy(o, cancel, progress);
}

} // namespace blockcopy
