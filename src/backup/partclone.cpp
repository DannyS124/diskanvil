// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "partclone.h"

#include "blockio.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QtEndian>

#include <archive.h>

#include <cstring>
#include <memory>
#include <zlib.h>

namespace partclone {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("partclone", text);
}

QString text(const QByteArray &d, int at, int size)
{
    const QByteArray field = d.mid(at, size);
    const qsizetype end = field.indexOf('\0');
    return QString::fromLatin1(end < 0 ? field : field.left(end));
}

// partclone's CRC-32: the usual one, started at all ones and not inverted at the end; zlib's
// is that, inverted.
quint32 crc(quint32 seed, const char *data, qsizetype size)
{
    return ~quint32(crc32(~seed, reinterpret_cast<const Bytef *>(data), uInt(size)));
}

// The parts of one image, read as one unpacked stream.
class Stream
{
public:
    explicit Stream(const QStringList &files)
    {
        archive_read_support_filter_all(m_a);
        archive_read_support_format_raw(m_a);
        for (const QString &f : files)
            m_names.push_back(QFile::encodeName(f));
        for (const QByteArray &n : m_names)
            m_pointers.push_back(n.constData());
        m_pointers.push_back(nullptr);
        archive_entry *entry = nullptr;
        m_ok = archive_read_open_filenames(m_a, m_pointers.data(), 1024 * 1024) == ARCHIVE_OK && archive_read_next_header(m_a, &entry) == ARCHIVE_OK;
    }
    ~Stream() { archive_read_free(m_a); }

    bool ok() const { return m_ok; }
    QString error() const { return QString::fromLocal8Bit(archive_error_string(m_a)); }

    // Up to `len` bytes, fewer only at the end: how many (0 at the end, -1 when it can't be read).
    qint64 readSome(char *buf, qint64 len)
    {
        qint64 got = 0;
        while (got < len) {
            const la_ssize_t n = archive_read_data(m_a, buf + got, size_t(len - got));
            if (n < 0)
                return -1;
            if (n == 0)
                break;
            got += n;
        }
        return got;
    }

    // Exactly `len` bytes; false when the stream ends first or can't be read.
    bool read(char *buf, qint64 len)
    {
        while (len > 0) {
            const la_ssize_t n = archive_read_data(m_a, buf, size_t(len));
            if (n <= 0)
                return false;
            buf += n;
            len -= n;
        }
        return true;
    }

private:
    archive *m_a = archive_read_new();
    std::vector<QByteArray> m_names;
    std::vector<const char *> m_pointers;
    bool m_ok = false;
};

} // namespace

Header parseHeader(const QByteArray &d)
{
    Header h;
    if (d.size() < kHeaderSize || !d.startsWith("partclone-image")) {
        h.problem = tr("it isn't a partclone image");
        return h;
    }
    const QString format = text(d, 0x1E, 4);
    if (format != QLatin1String("0002")) {
        h.problem = tr("it's in partclone's first format (from before 2013), which DiskForge doesn't read: restore it with Clonezilla");
        return h;
    }
    if (qFromLittleEndian<quint16>(d.constData() + 0x22) != 0xC0DE) {
        h.problem = tr("it was made on a computer that stores numbers the other way round");
        return h;
    }
    h.version = text(d, 0x10, 14);
    h.fs = text(d, 0x24, 16);
    h.deviceSize = qFromLittleEndian<quint64>(d.constData() + 0x34);
    h.totalBlocks = qFromLittleEndian<quint64>(d.constData() + 0x3C);
    h.usedBlocks = qFromLittleEndian<quint64>(d.constData() + 0x44);
    h.blockSize = qFromLittleEndian<quint32>(d.constData() + 0x54);
    h.checksumMode = qFromLittleEndian<quint16>(d.constData() + 0x60);
    h.checksumSize = qFromLittleEndian<quint16>(d.constData() + 0x62);
    h.blocksPerChecksum = qFromLittleEndian<quint32>(d.constData() + 0x64);
    h.reseed = d.at(0x68) != 0;
    const quint8 bitmapMode = quint8(d.at(0x69));
    if (crc(0xFFFFFFFF, d.constData(), 0x6A) != qFromLittleEndian<quint32>(d.constData() + 0x6A))
        h.problem = tr("its header is damaged");
    else if (h.blockSize == 0 || h.blockSize > 64 * 1024 * 1024 || h.totalBlocks == 0 || h.usedBlocks > h.totalBlocks
             || h.totalBlocks > (quint64(1) << 40))
        h.problem = tr("its header doesn't add up");
    else if (bitmapMode != 1)
        h.problem = tr("its kind of block map isn't one DiskForge reads");
    else if (h.checksumMode != 0 && (h.checksumMode != 0x20 || h.checksumSize != 4 || h.blocksPerChecksum == 0))
        h.problem = tr("its kind of checksum isn't one DiskForge reads");
    return h;
}

QVector<Image> scanFolder(const QString &folder)
{
    // sda1.ext4-ptcl-img.gz.aa, sda2.vfat-ptcl-img.zst, sda3.dd-img.gz.ab...
    static const QRegularExpression name(QStringLiteral("^([A-Za-z0-9_-]+)\\.(?:([A-Za-z0-9]+)-ptcl-img|(dd)-img)\\.(gz|zst|xz|bz2|lz4|lzo|uncomp)(?:\\.([a-z]{2}))?$"));
    QMap<QString, Image> found;
    const QStringList files = QDir(folder).entryList(QDir::Files, QDir::Name);
    for (const QString &file : files) {
        const QRegularExpressionMatch m = name.match(file);
        if (!m.hasMatch())
            continue;
        const QString key = m.captured(1) + QLatin1Char('.') + (m.captured(2).isEmpty() ? m.captured(3) : m.captured(2));
        Image &image = found[key];
        image.partition = m.captured(1);
        image.fs = m.captured(2);
        image.raw = !m.captured(3).isEmpty();
        image.files << QDir(folder).filePath(file);
    }
    QVector<Image> out;
    for (Image image : std::as_const(found)) {
        image.files.sort();
        if (!image.raw) {
            Stream stream(image.files);
            QByteArray head(kHeaderSize, '\0');
            image.header = stream.ok() && stream.read(head.data(), head.size()) ? parseHeader(head) : Header{};
            if (image.header.problem.isEmpty() && !image.header.valid())
                image.header.problem = tr("it couldn't be read");
        }
        out << image;
    }
    return out;
}

Restore::Restore(const Image &image, int fd, quint64 targetSize)
    : m_image(image)
    , m_fd(fd)
    , m_targetSize(targetSize)
{
    qRegisterMetaType<partclone::Result>();
}

void Restore::run()
{
    Result r;
    const auto finish = [this, &r](const QString &error) {
        r.ok = error.isEmpty() && !m_cancel;
        r.error = m_cancel && error.isEmpty() ? tr("Stopped: the partition is only partly restored.") : error;
        blockio::flush(m_fd);
        emit finished(r);
    };
    Stream stream(m_image.files);
    if (!stream.ok())
        return finish(tr("The image couldn't be opened: %1").arg(stream.error()));
    QElapsedTimer clock;
    clock.start();
    const qint64 chunk = 4 * 1024 * 1024;
    std::unique_ptr<char[]> buf(new char[size_t(chunk)]);

    // A "dd" image: the partition's bytes, as they come.
    if (m_image.raw) {
        while (!m_cancel) {
            const qint64 n = stream.readSome(buf.get(), chunk);
            if (n < 0)
                return finish(tr("The image couldn't be read: %1").arg(stream.error()));
            if (n == 0)
                break;
            if (r.written + quint64(n) > m_targetSize)
                return finish(tr("The image is bigger than the partition it's going to."));
            if (!blockio::writeAt(m_fd, buf.get(), quint64(n), r.written))
                return finish(tr("Writing to the partition failed."));
            r.written += quint64(n);
            if (clock.elapsed() > 200) {
                emit progress(r.written, m_targetSize);
                clock.restart();
            }
        }
        return finish({});
    }

    QByteArray head(kHeaderSize, '\0');
    if (!stream.read(head.data(), head.size()))
        return finish(tr("The image is cut short."));
    const Header h = parseHeader(head);
    if (!h.problem.isEmpty())
        return finish(tr("It can't be restored: %1.").arg(h.problem));
    if (h.deviceSize > m_targetSize)
        return finish(tr("The partition it's going to is smaller than the one it came from."));
    QByteArray bitmap(qsizetype((h.totalBlocks + 7) / 8), '\0');
    char stored[4];
    if (!stream.read(bitmap.data(), bitmap.size()) || !stream.read(stored, 4))
        return finish(tr("The image is cut short."));
    if (crc(0xFFFFFFFF, bitmap.constData(), bitmap.size()) != qFromLittleEndian<quint32>(stored))
        return finish(tr("Its map of used blocks is damaged."));

    const bool checked = h.checksumMode == 0x20;
    quint32 sum = 0xFFFFFFFF;
    quint32 inGroup = 0;
    quint64 done = 0;
    const quint64 total = h.usedBlocks * h.blockSize;
    std::unique_ptr<char[]> block(new char[h.blockSize]);
    // Used blocks one after another; runs of them next to each other are written together.
    quint64 runStart = 0, runLength = 0;
    QByteArray run;
    const auto flushRun = [&]() {
        if (runLength == 0)
            return true;
        const bool ok = blockio::writeAt(m_fd, run.constData(), quint64(run.size()), runStart * h.blockSize);
        r.written += quint64(run.size());
        run.clear();
        runLength = 0;
        return ok;
    };
    for (quint64 b = 0; b < h.totalBlocks && !m_cancel; ++b) {
        if (!(uchar(bitmap[qsizetype(b / 8)]) & (1u << (b % 8))))
            continue;
        if (!stream.read(block.get(), h.blockSize))
            return finish(tr("The image is cut short."));
        if (checked) {
            sum = crc(sum, block.get(), h.blockSize);
            if (++inGroup == h.blocksPerChecksum) {
                if (!stream.read(stored, 4))
                    return finish(tr("The image is cut short."));
                if (sum != qFromLittleEndian<quint32>(stored))
                    return finish(tr("The image is damaged around %1 MB into the partition: it was stopped there.").arg(b * h.blockSize / 1000000));
                inGroup = 0;
                if (h.reseed)
                    sum = 0xFFFFFFFF;
            }
        }
        if (runLength > 0 && (b != runStart + runLength || run.size() >= 8 * 1024 * 1024) && !flushRun())
            return finish(tr("Writing to the partition failed."));
        if (runLength == 0)
            runStart = b;
        run.append(block.get(), h.blockSize);
        ++runLength;
        done += h.blockSize;
        if (clock.elapsed() > 200) {
            emit progress(done, total);
            clock.restart();
        }
    }
    if (!flushRun())
        return finish(tr("Writing to the partition failed."));
    // The last group's checksum, when it wasn't a whole one.
    if (checked && inGroup > 0 && !m_cancel) {
        if (!stream.read(stored, 4) || sum != qFromLittleEndian<quint32>(stored))
            return finish(tr("The image's last part is damaged."));
    }
    finish({});
}

} // namespace partclone
