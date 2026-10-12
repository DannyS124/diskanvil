// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ewf.h"

#include "blockio.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QRandomGenerator>
#include <QThread>
#include <QtEndian>

#include <thread>
#include <zlib.h>

namespace ewf {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("ewf", text);
}

constexpr char kSignature[] = "EVF\x09\x0d\x0a\xff\x00";
constexpr int kDescriptor = 76;
constexpr int kSectorsPerChunk = 64;
constexpr int kGroup = 16375; // chunks in each sectors section and its table: what every reader takes

quint32 adler(const QByteArray &data)
{
    return quint32(adler32(adler32(0, nullptr, 0), reinterpret_cast<const Bytef *>(data.constData()), uInt(data.size())));
}

void put32(QByteArray &b, int at, quint32 v)
{
    qToLittleEndian<quint32>(v, b.data() + at);
}

void put64(QByteArray &b, int at, quint64 v)
{
    qToLittleEndian<quint64>(v, b.data() + at);
}

// A section's descriptor: its type, where the next one is, its size (with this), and the
// checksum of the rest.
QByteArray descriptor(const char *type, quint64 next, quint64 size)
{
    QByteArray d(kDescriptor, '\0');
    memcpy(d.data(), type, strlen(type));
    put64(d, 16, next);
    put64(d, 24, size);
    put32(d, 72, adler(d.left(72)));
    return d;
}

QString field(QString text)
{
    // Tabs and line ends separate the header's fields: they can't be in one.
    return text.replace(QLatin1Char('\t'), QLatin1Char(' ')).replace(QLatin1Char('\n'), QLatin1Char(' ')).remove(QLatin1Char('\r')).trimmed();
}

QByteArray packed(const QByteArray &data)
{
    uLongf size = compressBound(uLong(data.size()));
    QByteArray out(qsizetype(size), '\0');
    compress2(reinterpret_cast<Bytef *>(out.data()), &size, reinterpret_cast<const Bytef *>(data.constData()), uLong(data.size()), 6);
    out.resize(qsizetype(size));
    return out;
}

QString os()
{
#ifdef Q_OS_WIN
    return QStringLiteral("Windows");
#else
    return QStringLiteral("Linux");
#endif
}

} // namespace

Acquire::Acquire(int fd, quint64 size, int sectorSize, const QString &path, const Case &details)
    : m_fd(fd)
    , m_size(size)
    , m_sectorSize(sectorSize > 0 ? sectorSize : 512)
    , m_path(path)
    , m_case(details)
{
    qRegisterMetaType<ewf::Result>();
}

Acquire::~Acquire()
{
    if (m_fd >= 0)
        blockio::close(m_fd);
}

void Acquire::run()
{
    Result r;
    r.started = QDateTime::currentDateTime();
    QFile out(m_path);
    auto fail = [this, &r, &out](const QString &error) {
        r.error = error;
        out.close();
        QFile::remove(m_path);
        emit finished(r);
    };
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(tr("The evidence file couldn't be made: %1").arg(out.errorString()));
    const auto write = [&out](const QByteArray &data) { return out.write(data) == data.size(); };

    // The file's header: the signature, then segment 1.
    QByteArray head(13, '\0');
    memcpy(head.data(), kSignature, 8);
    head[8] = 1;
    head[9] = 1;
    if (!write(head))
        return fail(out.errorString());

    // The case, twice in header2 (UTF-16) and once in header (ASCII), the way EnCase 6 has it.
    const qint64 epoch = r.started.toSecsSinceEpoch();
    const QString version = QCoreApplication::applicationVersion().left(11);
    const QString when = r.started.toString(QStringLiteral("yyyy M d H m s"));
    const QString header2 = QStringLiteral("3\nmain\na\tc\tn\te\tt\tmd\tsn\tav\tov\tm\tu\tp\tdc\n%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9\t%10\t%10\t0\t\n\n"
                                           "srce\n0\t1\np\tn\tid\tev\ttb\tlo\tpo\tah\tgu\taq\n0\t0\n\t\t\t\t\t-1\t-1\t\t\t\n\n"
                                           "sub\n0\t1\np\tn\tid\tnu\tco\tgu\n0\t0\n\t\t\t\t1\t\n\n")
                                .arg(field(m_case.description), field(m_case.caseNumber), field(m_case.evidenceNumber), field(m_case.examiner),
                                     field(m_case.notes), field(m_case.model), field(m_case.serial), version, os())
                                .arg(epoch);
    QByteArray utf16("\xFF\xFE", 2);
    for (const QChar c : header2) {
        utf16.append(char(c.unicode() & 0xFF));
        utf16.append(char(c.unicode() >> 8));
    }
    const QByteArray header = QStringLiteral("1\r\nmain\r\nc\tn\ta\te\tt\tav\tov\tm\tu\tp\r\n%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%8\t0\r\n\r\n")
                                  .arg(field(m_case.caseNumber), field(m_case.evidenceNumber), field(m_case.description), field(m_case.examiner),
                                       field(m_case.notes), version, os(), when)
                                  .toLatin1();
    for (const auto &[type, text] : {std::pair{"header2", utf16}, std::pair{"header2", utf16}, std::pair{"header", header}}) {
        const QByteArray data = packed(text);
        const quint64 at = quint64(out.pos());
        if (!write(descriptor(type, at + kDescriptor + quint64(data.size()), kDescriptor + quint64(data.size()))) || !write(data))
            return fail(out.errorString());
    }

    // The volume: how the drive is cut into chunks.
    const quint64 chunkSize = quint64(kSectorsPerChunk) * quint64(m_sectorSize);
    const quint64 chunks = (m_size + chunkSize - 1) / chunkSize;
    QByteArray volume(1052, '\0');
    volume[0] = 0x01; // a fixed drive
    put32(volume, 4, quint32(chunks));
    put32(volume, 8, kSectorsPerChunk);
    put32(volume, 12, quint32(m_sectorSize));
    put64(volume, 16, m_size / quint64(m_sectorSize));
    volume[36] = 0x03; // an image of a physical drive
    volume[52] = 0x01; // packed, "good"
    put32(volume, 56, kSectorsPerChunk);
    for (int i = 0; i < 16; ++i)
        volume[64 + i] = char(QRandomGenerator::system()->bounded(256));
    put32(volume, 1048, adler(volume.left(1048)));
    {
        const quint64 at = quint64(out.pos());
        if (!write(descriptor("volume", at + kDescriptor + 1052, kDescriptor + 1052)) || !write(volume))
            return fail(out.errorString());
    }

    QCryptographicHash md5(QCryptographicHash::Md5), sha1(QCryptographicHash::Sha1), sha256(QCryptographicHash::Sha256);
    // Groups of chunks: a sectors section, then its table twice.
    const int threads = std::clamp(QThread::idealThreadCount(), 1, 8);
    const quint64 batchChunks = 64; // read 2 MiB at a time, packed on several threads
    QByteArray buffer;
    quint64 chunk = 0;
    while (chunk < chunks && !m_cancel) {
        const quint64 groupEnd = std::min<quint64>(chunk + kGroup, chunks);
        const quint64 sectorsAt = quint64(out.pos());
        if (!write(QByteArray(kDescriptor, '\0'))) // filled in once its size is known
            return fail(out.errorString());
        QVector<quint32> entries;
        while (chunk < groupEnd && !m_cancel) {
            const quint64 n = std::min<quint64>(batchChunks, groupEnd - chunk);
            const quint64 from = chunk * chunkSize;
            const quint64 len = std::min<quint64>(n * chunkSize, m_size - from);
            buffer.resize(qsizetype(len));
            if (!blockio::readAt(m_fd, buffer.data(), len, from)) {
                // Sector by sector, what can't be read kept as zeros.
                for (quint64 s = 0; s < len; s += quint64(m_sectorSize)) {
                    if (!blockio::readAt(m_fd, buffer.data() + s, quint64(m_sectorSize), from + s)) {
                        memset(buffer.data() + s, 0, size_t(m_sectorSize));
                        const quint64 sector = (from + s) / quint64(m_sectorSize);
                        if (!r.unreadable.isEmpty() && r.unreadable.last().first + r.unreadable.last().second == sector)
                            ++r.unreadable.last().second;
                        else
                            r.unreadable.append({sector, 1});
                    }
                }
            }
            md5.addData(buffer);
            sha1.addData(buffer);
            sha256.addData(buffer);
            // Each chunk packed, or kept as it is (with its checksum) when packing doesn't help.
            // One slot each, written by one thread each (no vector<bool>: its bits share bytes).
            std::vector<QByteArray> stored;
            stored.resize(size_t(n));
            std::vector<char> compressed(size_t(n), 0);
            std::vector<std::thread> workers;
            for (int t = 0; t < threads; ++t) {
                workers.emplace_back([&, t] {
                    for (quint64 i = quint64(t); i < n; i += quint64(threads)) {
                        const QByteArray data = buffer.mid(qsizetype(i * chunkSize), qsizetype(chunkSize));
                        QByteArray z = packed(data);
                        if (z.size() < data.size()) {
                            stored[size_t(i)] = z;
                            compressed[size_t(i)] = 1;
                        } else {
                            QByteArray raw = data;
                            raw.resize(data.size() + 4);
                            put32(raw, int(data.size()), adler(data));
                            stored[size_t(i)] = raw;
                        }
                    }
                });
            }
            for (std::thread &w : workers)
                w.join();
            for (size_t i = 0; i < stored.size(); ++i) {
                const quint64 relative = quint64(out.pos()) - sectorsAt;
                if (relative > 0x7FFFFFFF)
                    return fail(tr("A part of the evidence file grew past what its format can point to."));
                entries << (quint32(relative) | (compressed[i] ? 0x80000000u : 0));
                if (!write(stored[i]))
                    return fail(tr("Writing the evidence file failed: %1").arg(out.errorString()));
            }
            chunk += n;
            r.bytes = from + len;
            emit progress(r.bytes, m_size);
        }
        const quint64 tableAt = quint64(out.pos());
        // The sectors section's descriptor, now that its size is known.
        out.seek(qint64(sectorsAt));
        if (!write(descriptor("sectors", tableAt, tableAt - sectorsAt)))
            return fail(out.errorString());
        out.seek(qint64(tableAt));
        QByteArray table(24, '\0');
        put32(table, 0, quint32(entries.size()));
        put64(table, 8, sectorsAt);
        put32(table, 20, adler(table.left(20)));
        QByteArray list(entries.size() * 4, '\0');
        for (qsizetype i = 0; i < entries.size(); ++i)
            put32(list, int(i * 4), entries[i]);
        table += list;
        QByteArray footer(4, '\0');
        put32(footer, 0, adler(list));
        table += footer;
        const quint64 size = kDescriptor + quint64(table.size());
        if (!write(descriptor("table", tableAt + size, size)) || !write(table) || !write(descriptor("table2", tableAt + 2 * size, size)) || !write(table))
            return fail(out.errorString());
    }
    if (m_cancel)
        return fail(tr("Stopped: the evidence file was removed, being only part done."));

    // The volume again (data), the hashes, and the end.
    r.md5 = md5.result();
    r.sha1 = sha1.result();
    r.sha256 = sha256.result();
    QByteArray digest(80, '\0');
    digest.replace(0, 16, r.md5);
    digest.replace(16, 20, r.sha1);
    put32(digest, 76, adler(digest.left(76)));
    QByteArray hash(36, '\0');
    hash.replace(0, 16, r.md5);
    put32(hash, 32, adler(hash.left(32)));
    for (const auto &[type, data] : {std::pair{"data", volume}, std::pair{"digest", digest}, std::pair{"hash", hash}}) {
        const quint64 at = quint64(out.pos());
        if (!write(descriptor(type, at + kDescriptor + quint64(data.size()), kDescriptor + quint64(data.size()))) || !write(data))
            return fail(out.errorString());
    }
    const quint64 doneAt = quint64(out.pos());
    if (!write(descriptor("done", doneAt, 0)) || !out.flush())
        return fail(out.errorString());
    r.fileSize = quint64(out.size());
    out.close();
    r.ok = true;
    r.finished = QDateTime::currentDateTime();
    emit finished(r);
}

namespace {

struct Chunk {
    int segment = 0;
    quint64 offset = 0;
    quint32 size = 0;
    bool compressed = false;
};

// The segment files: name.E01, then .E02... (.E99, then .EAA...).
QStringList segments(const QString &first)
{
    QStringList out{first};
    const QString base = first.left(first.size() - 2);
    const bool lower = first.at(first.size() - 3).isLower();
    for (int n = 2; n < 100; ++n) {
        const QString next = base + QStringLiteral("%1").arg(n, 2, 10, QLatin1Char('0'));
        if (!QFileInfo::exists(next))
            return out;
        out << next;
    }
    for (char a = 'A'; a <= 'Z'; ++a) {
        for (char b = 'A'; b <= 'Z'; ++b) {
            QString next = first.left(first.size() - 2) + QLatin1Char(a) + QLatin1Char(b);
            if (lower)
                next = next.left(next.size() - 2) + next.right(2).toLower();
            if (!QFileInfo::exists(next))
                return out;
            out << next;
        }
    }
    return out;
}

void readHeader(const QString &text, Info *info)
{
    // The "main" category: a line of names, then a line of values.
    const QStringList lines = QString(text).remove(QLatin1Char('\r')).split(QLatin1Char('\n'));
    const int main = int(lines.indexOf(QStringLiteral("main")));
    if (main < 0 || main + 2 >= lines.size())
        return;
    const QStringList names = lines[main + 1].split(QLatin1Char('\t')), values = lines[main + 2].split(QLatin1Char('\t'));
    const auto get = [&](const char *name) { return values.value(names.indexOf(QLatin1String(name))); };
    info->caseNumber = get("c");
    info->evidenceNumber = get("n");
    info->description = get("a");
    info->examiner = get("e");
    info->notes = get("t");
    if (names.contains(QStringLiteral("md")))
        info->model = get("md");
    if (names.contains(QStringLiteral("sn")))
        info->serial = get("sn");
}

class Reader : public lost::Layer
{
public:
    QVector<QFile *> files;
    QVector<Chunk> chunks;
    quint64 total = 0;
    quint64 chunkSize = 32768;

    ~Reader() override { qDeleteAll(files); }
    quint64 size() const override { return total; }

    bool read(quint64 offset, char *buf, quint64 len) override
    {
        QMutexLocker locker(&m_lock);
        while (len > 0) {
            const quint64 index = offset / chunkSize;
            const QByteArray *data = chunk(index);
            if (!data)
                return false;
            const quint64 from = offset % chunkSize, n = std::min<quint64>(len, quint64(data->size()) - from);
            if (from >= quint64(data->size()))
                return false;
            memcpy(buf, data->constData() + from, n);
            offset += n;
            buf += n;
            len -= n;
        }
        return true;
    }

private:
    const QByteArray *chunk(quint64 index)
    {
        if (index >= quint64(chunks.size()))
            return nullptr;
        if (m_cache.contains(index))
            return &m_cache[index];
        const Chunk &c = chunks.at(qsizetype(index));
        QFile *f = files.at(c.segment);
        if (!f->seek(qint64(c.offset)))
            return nullptr;
        const QByteArray stored = f->read(c.size);
        const quint64 want = std::min<quint64>(chunkSize, total - index * chunkSize);
        QByteArray data;
        if (c.compressed) {
            data.resize(qsizetype(want));
            uLongf size = uLongf(want);
            if (uncompress(reinterpret_cast<Bytef *>(data.data()), &size, reinterpret_cast<const Bytef *>(stored.constData()), uLong(stored.size())) != Z_OK)
                return nullptr;
            data.resize(qsizetype(size));
        } else {
            data = stored.left(qsizetype(want));
        }
        if (m_order.size() >= 64)
            m_cache.remove(m_order.takeFirst());
        m_order << index;
        m_cache.insert(index, data);
        return &m_cache[index];
    }

    QHash<quint64, QByteArray> m_cache;
    QVector<quint64> m_order;
    QMutex m_lock;
};

} // namespace

std::unique_ptr<lost::Layer> open(const QString &path, Info *info)
{
    auto reader = std::make_unique<Reader>();
    quint64 sectors = 0;
    for (const QString &name : segments(path)) {
        auto *f = new QFile(name);
        reader->files << f;
        if (!f->open(QIODevice::ReadOnly) || f->read(8) != QByteArray(kSignature, 8)) {
            info->problem = tr("%1 isn't an evidence file (E01).").arg(QFileInfo(name).fileName());
            return nullptr;
        }
        const int segment = int(reader->files.size() - 1);
        quint64 at = 13, sectorsStart = 0, sectorsEnd = 0;
        for (int guard = 0; guard < 1000000; ++guard) {
            if (!f->seek(qint64(at)))
                break;
            const QByteArray d = f->read(kDescriptor);
            if (d.size() < kDescriptor)
                break;
            const QByteArray type = d.left(16).left(d.left(16).indexOf('\0') < 0 ? 16 : d.left(16).indexOf('\0'));
            const quint64 next = qFromLittleEndian<quint64>(d.constData() + 16), size = qFromLittleEndian<quint64>(d.constData() + 24);
            const QByteArray body = (type == "sectors" || size <= kDescriptor || size > 64 * 1024 * 1024) ? QByteArray() : f->read(qint64(size - kDescriptor));
            if (type == "header2" || type == "header") {
                QByteArray text(16 * 1024 * 1024, '\0');
                uLongf n = uLongf(text.size());
                if (uncompress(reinterpret_cast<Bytef *>(text.data()), &n, reinterpret_cast<const Bytef *>(body.constData()), uLong(body.size())) == Z_OK
                    && info->caseNumber.isEmpty() && info->description.isEmpty()) {
                    text.resize(qsizetype(n));
                    readHeader(type == "header2" ? QString::fromUtf16(reinterpret_cast<const char16_t *>(text.constData() + 2), (text.size() - 2) / 2)
                                                 : QString::fromLatin1(text),
                               info);
                }
            } else if ((type == "volume" || type == "disk") && body.size() >= 24) {
                // Sectors of 512 to 4096 bytes, chunks of up to 32768 of them (what EWF tools make); a damaged
                // file's other values would ask for any amount of memory.
                const quint32 perChunk = qFromLittleEndian<quint32>(body.constData() + 8), bytes = qFromLittleEndian<quint32>(body.constData() + 12);
                const bool sane = bytes >= 512 && bytes <= 4096 && (bytes & (bytes - 1)) == 0 && perChunk >= 1 && perChunk <= 32768;
                reader->chunkSize = sane ? quint64(perChunk) * bytes : 0;
                info->sectorSize = sane ? int(bytes) : 512;
                sectors = qFromLittleEndian<quint64>(body.constData() + 16);
            } else if (type == "sectors") {
                sectorsStart = at;
                sectorsEnd = at + size;
            } else if (type == "table" && body.size() >= 28) {
                const quint32 count = qFromLittleEndian<quint32>(body.constData());
                const quint64 base = qFromLittleEndian<quint64>(body.constData() + 8);
                for (quint32 i = 0; i < count && 24 + (i + 1) * 4 <= quint32(body.size()); ++i) {
                    const quint32 e = qFromLittleEndian<quint32>(body.constData() + 24 + i * 4);
                    Chunk c;
                    c.segment = segment;
                    c.compressed = e & 0x80000000u;
                    c.offset = base + (e & 0x7FFFFFFF);
                    quint64 end = sectorsEnd;
                    if (i + 1 < count)
                        end = base + (qFromLittleEndian<quint32>(body.constData() + 24 + (i + 1) * 4) & 0x7FFFFFFF);
                    c.size = quint32(end > c.offset ? end - c.offset : 0);
                    reader->chunks << c;
                }
                Q_UNUSED(sectorsStart)
            } else if (type == "digest" && body.size() >= 36) {
                info->md5 = body.left(16);
                info->sha1 = body.mid(16, 20);
            } else if (type == "hash" && body.size() >= 16 && info->md5.isEmpty()) {
                info->md5 = body.left(16);
            }
            if (type == "done" || next == at || next == 0)
                break;
            at = next;
        }
    }
    if (reader->chunks.isEmpty() || reader->chunkSize == 0) {
        info->problem = tr("Its list of chunks couldn't be read.");
        return nullptr;
    }
    reader->total = sectors * quint64(info->sectorSize);
    info->size = reader->total;
    return reader;
}

QString verify(const QString &path, const std::atomic<bool> *cancel)
{
    Info info;
    const std::unique_ptr<lost::Layer> layer = open(path, &info);
    if (!layer)
        return info.problem;
    if (info.md5.isEmpty())
        return tr("It keeps no MD5 to check against.");
    QCryptographicHash md5(QCryptographicHash::Md5);
    QByteArray buf(4 * 1024 * 1024, '\0');
    for (quint64 at = 0; at < layer->size(); at += quint64(buf.size())) {
        if (cancel && *cancel)
            return tr("Stopped.");
        const quint64 n = std::min<quint64>(quint64(buf.size()), layer->size() - at);
        if (!layer->read(at, buf.data(), n))
            return tr("Part of it can't be read back (around %1 MB).").arg(at / 1000000);
        md5.addData(QByteArrayView(buf.constData(), qsizetype(n)));
    }
    return md5.result() == info.md5 ? QString() : tr("What it holds doesn't match the MD5 it keeps: it's damaged.");
}

} // namespace ewf
