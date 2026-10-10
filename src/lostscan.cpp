// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lostscan.h"

#include "applog.h"
#include "blockio.h"
#include "filetypes.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>

#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace {

constexpr quint64 kSector = 512;
constexpr quint64 kChunk = 4 * 1024 * 1024;

QString tr(const char *text)
{
    return QCoreApplication::translate("lostfiles", text);
}

QString conditionText(lost::Condition condition)
{
    switch (condition) {
    case lost::Condition::Good:
        return tr("looks whole");
    case lost::Condition::MaybeDamaged:
        return tr("may be damaged");
    case lost::Condition::Overwritten:
        return tr("partly overwritten");
    case lost::Condition::Unreadable:
        break;
    }
    return tr("partly unreadable");
}

} // namespace

lost::FileReader::FileReader(std::shared_ptr<Source> source, const Found &file)
    : m_source(std::move(source))
    , m_extents(file.extents)
    , m_size(file.size)
{
}

qint64 lost::FileReader::read(quint64 at, char *buf, qint64 len)
{
    if (at >= m_size || len <= 0)
        return 0;
    len = qint64(std::min<quint64>(quint64(len), m_size - at));
    qint64 done = 0;
    quint64 passed = 0; // bytes of the file in the extents before this one
    for (const Extent &e : std::as_const(m_extents)) {
        if (done == len)
            break;
        const quint64 want = at + quint64(done);
        if (want < passed + e.length) {
            const quint64 inside = want - passed;
            const quint64 n = std::min<quint64>(e.length - inside, quint64(len - done));
            const quint64 got = m_source->read(e.start + inside, buf + done, n);
            if (got < n)
                memset(buf + done + got, 0, n - got);
            done += qint64(n);
        }
        passed += e.length;
    }
    // A file shorter in extents than its size (it happens with damaged file systems): zeros.
    if (done < len)
        memset(buf + done, 0, size_t(len - done));
    return len;
}

QByteArray lost::FileReader::head(qint64 len)
{
    QByteArray data(qint64(std::min<quint64>(quint64(len), m_size)), Qt::Uninitialized);
    read(0, data.data(), data.size());
    return data;
}

lost::DeepScan::DeepScan(std::shared_ptr<Source> source, quint64 start, quint64 end)
    : m_source(std::move(source))
    , m_start(start / kSector * kSector)
    , m_end(end == 0 || end > m_source->size() ? m_source->size() : end)
{
}

void lost::DeepScan::run()
{
    const QVector<filetypes::Type> &types = filetypes::types();
    QVector<int> perType(types.size(), 0);
    QHash<int, int> perCategory;
    QVector<Found> batch;
    QElapsedTimer sinceBatch;
    sinceBatch.start();
    QByteArray chunk(qsizetype(kChunk), Qt::Uninitialized);
    quint64 skipUntil = 0;
    int foundCount = 0;
    const quint64 total = m_end - m_start;
    qCInfo(lcOps).noquote() << "Find Lost Files: deep scan of" << total << "bytes";

    for (quint64 pos = m_start; pos < m_end && !m_cancel;) {
        // Inside a file that was just found whole: no need to read it again.
        if (skipUntil > pos) {
            pos = std::min(skipUntil / kSector * kSector, m_end);
            continue;
        }
        const quint64 n = std::min(kChunk, m_end - pos);
        m_source->read(pos, chunk.data(), n);
        for (quint64 off = 0; off + 32 <= n && !m_cancel; off += kSector) {
            const quint64 at = pos + off;
            if (at < skipUntil)
                continue;
            const QVector<int> candidates = filetypes::candidates(reinterpret_cast<const uchar *>(chunk.constData()) + off, int(std::min(kSector, n - off)));
            for (int type : candidates) {
                const quint64 end = m_end;
                Source *source = m_source.get();
                const filetypes::Reader reader = [source, at, end](quint64 offset, char *buf, qint64 len) -> qint64 {
                    if (at + offset >= end)
                        return 0;
                    return qint64(source->read(at + offset, buf, qint64(std::min<quint64>(quint64(len), end - at - offset))));
                };
                const filetypes::Measured m = filetypes::measure(type, reader);
                if (m.size == 0 || m.type < 0)
                    continue;
                if (perType[m.type] >= kPerTypeLimit || foundCount >= kTotalLimit)
                    break;
                ++perType[m.type];
                ++foundCount;
                Found f;
                f.type = m.type;
                f.size = m.size;
                f.extents = {{at, m.size}};
                f.origin = Origin::Contents;
                f.condition = m.complete ? Condition::Good : Condition::MaybeDamaged;
                if (m_source->unreadable(at, m.size))
                    f.condition = Condition::Unreadable;
                f.name = carvedName(m.type, ++perCategory[int(types[m.type].category)]);
                batch << f;
                // A whole file isn't searched again inside: a JPEG's own preview picture
                // shouldn't come back as a second photo.
                if (m.complete)
                    skipUntil = std::max(skipUntil, (at + m.size + kSector - 1) / kSector * kSector);
                break;
            }
        }
        pos += n;
        if (!batch.isEmpty() && sinceBatch.elapsed() > 250) {
            Q_EMIT found(batch);
            batch.clear();
            sinceBatch.restart();
        }
        Q_EMIT progress(std::min(pos, m_end) - m_start, total);
    }
    if (!batch.isEmpty())
        Q_EMIT found(batch);
    qCInfo(lcOps).noquote() << "Find Lost Files: deep scan" << (m_cancel ? "stopped" : "done");
    Q_EMIT finished(m_cancel);
}

lost::Saver::Saver(std::shared_ptr<Source> source, QVector<Found> files, QString folder)
    : m_source(std::move(source))
    , m_files(std::move(files))
    , m_folder(std::move(folder))
{
}

void lost::Saver::run()
{
    const QString base = m_folder + QStringLiteral("/Recovered ") + QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH.mm"));
    quint64 total = 0, done = 0;
    for (const Found &f : std::as_const(m_files))
        total += f.size;
    int saved = 0, failed = 0;
    QStringList report, problems;
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    qCInfo(lcOps).noquote() << "Find Lost Files: saving" << m_files.size() << "files to" << base;

    for (const Found &f : std::as_const(m_files)) {
        if (m_cancel)
            break;
        QString folder = base;
        if (f.origin == Origin::FileSystem && !f.folder.isEmpty()) {
            for (const QString &part : f.folder.split(QLatin1Char('/'), Qt::SkipEmptyParts))
                folder += QLatin1Char('/') + cleanName(part);
        } else if (f.type >= 0) {
            folder += QLatin1Char('/') + filetypes::categoryName(filetypes::types()[f.type].category);
        }
        QDir().mkpath(folder);
        // A new file every time: a name that's taken gets " (2)", " (3)"...
        const QString name = cleanName(f.name);
        const QString stem = name.contains(QLatin1Char('.')) ? name.section(QLatin1Char('.'), 0, -2) : name;
        const QString ext = name.contains(QLatin1Char('.')) ? QLatin1Char('.') + name.section(QLatin1Char('.'), -1) : QString();
        int fd = -1;
        QString path;
        for (int n = 1; n < 10000 && fd < 0; ++n) {
            path = folder + QLatin1Char('/') + (n == 1 ? name : QStringLiteral("%1 (%2)%3").arg(stem).arg(n).arg(ext));
            fd = blockio::openFile(path, blockio::FileMode::CreateNew);
            if (fd < 0 && errno != EEXIST)
                break;
        }
        if (fd < 0) {
            ++failed;
            problems << tr("%1: couldn't be saved (%2)").arg(name, QString::fromLocal8Bit(strerror(errno)));
            done += f.size;
            continue;
        }
        FileReader reader(m_source, f);
        bool ok = true;
        for (quint64 at = 0; at < f.size && ok && !m_cancel;) {
            const qint64 n = reader.read(at, buffer.data(), buffer.size());
            ok = n > 0 && blockio::writeAll(fd, buffer.constData(), n);
            at += quint64(std::max<qint64>(n, 1));
            done += quint64(std::max<qint64>(n, 0));
            Q_EMIT progress(done, total);
        }
        ok = ok && blockio::flush(fd);
        if (ok && f.modified > 0)
            blockio::setFileTime(fd, f.modified);
        ::close(fd);
        if (!ok || m_cancel) {
            ::unlink(QFile::encodeName(path).constData());
            if (!m_cancel) {
                ++failed;
                problems << tr("%1: couldn't be written (is the drive full?)").arg(name);
            }
            continue;
        }
        ++saved;
        const QString shownName = path.mid(base.size() + 1);
        report << (f.condition == Condition::Good ? shownName : QStringLiteral("%1  (%2)").arg(shownName, conditionText(f.condition)));
    }

    QFile summary(base + QStringLiteral("/What was saved.txt"));
    if (QDir().mkpath(base) && summary.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QStringList text = {tr("Saved by DiskForge's Find Lost Files on %1.").arg(QDateTime::currentDateTime().toString(Qt::ISODate)), QString()};
        if (!problems.isEmpty())
            text << tr("Not saved:") << problems << QString();
        text << tr("Saved (%1):").arg(saved) << report;
        summary.write(text.join(QLatin1Char('\n')).toUtf8() + '\n');
    }
    const QString message = m_cancel ? tr("Stopped. %1 files were saved before that.").arg(saved) : QString();
    qCInfo(lcOps).noquote() << "Find Lost Files: saved" << saved << "failed" << failed;
    Q_EMIT finished(saved, failed, base, message);
}

QString lost::cleanName(const QString &name)
{
    QString clean;
    clean.reserve(name.size());
    for (const QChar c : name) {
        if (c.unicode() < 0x20 || c == QLatin1Char(0x7f) || QStringLiteral("/\\:*?\"<>|").contains(c))
            clean += QLatin1Char('_');
        else
            clean += c;
    }
    // No leading or trailing dots and spaces (Windows drops them), no "." or "..".
    while (!clean.isEmpty() && (clean.front() == QLatin1Char(' ') || clean.front() == QLatin1Char('.')))
        clean.remove(0, 1);
    while (!clean.isEmpty() && (clean.back() == QLatin1Char(' ') || clean.back() == QLatin1Char('.')))
        clean.chop(1);
    if (clean.isEmpty())
        clean = QStringLiteral("file");
    // 255 bytes is the most most file systems take; the extension is kept.
    while (clean.toUtf8().size() > 200) {
        const qsizetype dot = clean.lastIndexOf(QLatin1Char('.'));
        const qsizetype cut = dot > 0 && clean.size() - dot <= 12 ? dot - 1 : clean.size() - 1;
        clean.remove(cut, 1);
    }
    return clean;
}

QString lost::carvedName(int type, int number)
{
    const filetypes::Type &t = filetypes::types().value(type);
    QString word;
    switch (t.category) {
    case filetypes::Category::Picture:
        word = tr("Picture");
        break;
    case filetypes::Category::Document:
        word = tr("Document");
        break;
    case filetypes::Category::Video:
        word = tr("Video");
        break;
    case filetypes::Category::Music:
        word = tr("Sound");
        break;
    case filetypes::Category::Archive:
        word = tr("Archive");
        break;
    case filetypes::Category::Other:
        word = tr("File");
        break;
    }
    return QStringLiteral("%1 %2.%3").arg(word).arg(number, 6, 10, QLatin1Char('0')).arg(t.id);
}
