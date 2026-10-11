// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lostscan.h"

#include "applog.h"
#include "blockio.h"
#include "filetypes.h"
#include "lostfs.h"

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
    : m_source(file.from ? file.from : std::move(source))
    , m_extents(file.extents)
    , m_data(file.data)
    , m_size(file.size)
{
}

qint64 lost::FileReader::read(quint64 at, char *buf, qint64 len)
{
    if (at >= m_size || len <= 0)
        return 0;
    len = qint64(std::min<quint64>(quint64(len), m_size - at));
    if (!m_data.isEmpty()) {
        const qint64 n = std::max<qint64>(0, std::min<qint64>(len, m_data.size() - qint64(at)));
        if (n > 0)
            memcpy(buf, m_data.constData() + at, size_t(n));
        if (n < len)
            memset(buf + n, 0, size_t(len - n));
        return len;
    }
    qint64 done = 0;
    quint64 passed = 0; // bytes of the file in the extents before this one
    for (const Extent &e : std::as_const(m_extents)) {
        if (done == len)
            break;
        const quint64 want = at + quint64(done);
        if (want < passed + e.length) {
            const quint64 inside = want - passed;
            const quint64 n = std::min<quint64>(e.length - inside, quint64(len - done));
            const quint64 got = e.zeros() ? 0 : m_source->read(e.start + inside, buf + done, n);
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

lost::QuickScan::QuickScan(std::shared_ptr<Source> source)
    : m_source(std::move(source))
{
}

void lost::QuickScan::addRanges(const Found &file)
{
    for (const Extent &e : file.extents) {
        if (!e.zeros())
            m_ranges.push_back(e);
    }
}

void lost::QuickScan::run()
{
    qCInfo(lcOps).noquote() << "Find Lost Files: reading the file systems";
    QVector<Found> deleted;
    for (const Extent &where : partitionsOf(*m_source)) {
        if (m_cancel)
            break;
        Volume volume;
        // Windows' earlier copies of an NTFS volume: what its records hold now is kept, to tell
        // the copies' files apart from today's.
        volume.copies = vss::copies(*m_source, where, &volume.copiesProblem);
        std::shared_ptr<RecordIndex> records = volume.copies.isEmpty() ? nullptr : std::make_shared<RecordIndex>();
        const bool read = readFileSystem(*m_source, where, [&](QVector<Found> &&batch) {
            QVector<Found> shown;
            for (Found &f : batch) {
                if (!f.extents.isEmpty() && !f.extents.first().zeros())
                    m_starts.push_back(f.extents.first().start);
                // What's still in use is there for sure; what's deleted waits for its check.
                if (f.deleted) {
                    deleted.push_back(std::move(f));
                } else {
                    addRanges(f);
                    shown.push_back(std::move(f));
                }
            }
            if (!shown.isEmpty())
                Q_EMIT found(shown);
        }, m_cancel, &volume, records.get());
        volume.records = records;
        if (read) {
            qCInfo(lcOps).noquote() << "Find Lost Files:" << volume.type << volume.label << "at" << where.start << "has"
                                    << volume.files << "files," << volume.deleted << "deleted";
            m_volumes.push_back(volume);
        }
    }

    // What's in the deleted files: the start of each, and the whole of as many as the budget
    // takes, smallest first (most files for the reading).
    std::stable_sort(deleted.begin(), deleted.end(), [](const Found &a, const Found &b) { return a.size < b.size; });
    quint64 budget = kFullCheckBudget;
    QVector<Found> batch;
    QElapsedTimer sinceBatch;
    sinceBatch.start();
    for (qsizetype i = 0; i < deleted.size() && !m_cancel; ++i) {
        Found &f = deleted[i];
        const bool full = f.size <= kFullCheckLimit && f.size <= budget;
        if (full)
            budget -= f.size;
        checkContents(m_source, f, full);
        if (f.checked && f.condition == Condition::Good)
            addRanges(f);
        batch.push_back(std::move(f));
        if (sinceBatch.elapsed() > 250) {
            Q_EMIT found(std::exchange(batch, {}));
            sinceBatch.restart();
        }
        Q_EMIT progress(quint64(i + 1), quint64(deleted.size()));
    }
    if (!batch.isEmpty())
        Q_EMIT found(batch);

    // Sorted and joined, for the deep scan to walk along.
    std::sort(m_ranges.begin(), m_ranges.end(), [](const Extent &a, const Extent &b) { return a.start < b.start; });
    QVector<Extent> joined;
    for (const Extent &e : std::as_const(m_ranges)) {
        if (!joined.isEmpty() && e.start <= joined.last().end())
            joined.last().length = std::max(joined.last().end(), e.end()) - joined.last().start;
        else
            joined.push_back(e);
    }
    m_ranges = joined;
    qCInfo(lcOps).noquote() << "Find Lost Files: file systems" << (m_cancel ? "stopped" : "done");
    Q_EMIT finished(m_cancel);
}

lost::DeepScan::DeepScan(std::shared_ptr<Source> source, quint64 start, quint64 end)
    : m_source(std::move(source))
    , m_start(start / kSector * kSector)
    , m_end(end == 0 || end > m_source->size() ? m_source->size() : end)
{
}

void lost::DeepScan::skip(QVector<Extent> ranges, QVector<quint64> starts)
{
    m_skipRanges = std::move(ranges);
    m_skipStarts = QSet<quint64>(starts.cbegin(), starts.cend());
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

    qsizetype range = 0; // the first known range that doesn't end before pos
    auto known = [this, &range](quint64 at) {
        while (range < m_skipRanges.size() && m_skipRanges[range].end() <= at)
            ++range;
        return range < m_skipRanges.size() && m_skipRanges[range].start <= at;
    };
    for (quint64 pos = m_start; pos < m_end && !m_cancel;) {
        // Inside a file that was just found whole: no need to read it again.
        if (skipUntil > pos) {
            pos = std::min(skipUntil / kSector * kSector, m_end);
            continue;
        }
        // Inside files the file system listed already, the same.
        if (known(pos)) {
            skipUntil = std::max(skipUntil, (m_skipRanges[range].end() + kSector - 1) / kSector * kSector);
            Q_EMIT progress(std::min(skipUntil, m_end) - m_start, total);
            continue;
        }
        const quint64 n = std::min(kChunk, m_end - pos);
        m_source->read(pos, chunk.data(), n);
        for (quint64 off = 0; off + 32 <= n && !m_cancel; off += kSector) {
            const quint64 at = pos + off;
            if (at < skipUntil || m_skipStarts.contains(at) || known(at))
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

lost::CopyScan::CopyScan(std::shared_ptr<Source> source, QVector<Volume> volumes)
    : m_source(std::move(source))
    , m_volumes(std::move(volumes))
{
}

void lost::CopyScan::run()
{
    int total = 0, done = 0, listed = 0;
    for (const Volume &v : std::as_const(m_volumes))
        total += int(v.copies.size());
    QString problem;
    for (const Volume &v : std::as_const(m_volumes)) {
        for (int i = int(v.copies.size()) - 1; i >= 0 && !m_cancel; --i, ++done) {
            const vss::Copy &copy = v.copies[i];
            Q_EMIT progress(done, total, copy.created);
            const auto source = vss::open(m_source, v.where, i, &problem);
            if (!source)
                continue;
            qCInfo(lcOps).noquote() << "Find Lost Files: looking in Windows' copy" << copy.id << "of" << v.label;
            CopyOptions options;
            options.now = v.records.get();
            options.taken = copy.created;
            // Down to the second: Windows can take two copies within a minute.
            options.top = tr("%1 as it was on %2").arg(v.label, QDateTime::fromMSecsSinceEpoch(copy.created).toString(QStringLiteral("yyyy-MM-dd HH.mm.ss")));
            QVector<Found> files;
            readNtfs(*source, {0, source->size()}, [&files, &source](QVector<Found> &&batch) {
                for (Found &f : batch) {
                    f.from = source;
                    files.push_back(std::move(f));
                }
            }, m_cancel, nullptr, nullptr, &options);
            // Checked like deleted files: an earlier copy keeps what's in them only as far as
            // Windows kept the blocks.
            QVector<Found> batch;
            for (Found &f : files) {
                if (m_cancel)
                    break;
                checkContents(source, f, f.size <= kFullCheckLimit);
                batch.push_back(std::move(f));
                if (batch.size() >= 500)
                    Q_EMIT found(std::exchange(batch, {}));
            }
            if (!batch.isEmpty())
                Q_EMIT found(batch);
            listed += int(files.size());
        }
    }
    Q_EMIT progress(done, total, 0);
    Q_EMIT finished(m_cancel, listed, problem);
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
    QStringList report, problems, namesOnly;
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    qCInfo(lcOps).noquote() << "Find Lost Files: saving" << m_files.size() << "files to" << base;

    for (int position = 0; position < m_files.size(); ++position) {
        const Found &f = m_files[position];
        if (m_cancel)
            break;
        if (f.nameOnly) {
            namesOnly << f.folder + QLatin1Char('/') + f.name;
            continue;
        }
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
        blockio::close(fd);
        if (!ok || m_cancel) {
            QFile::remove(path);
            if (!m_cancel) {
                ++failed;
                problems << tr("%1: couldn't be written (is the drive full?)").arg(name);
            }
            continue;
        }
        ++saved;
        Q_EMIT fileSaved(position, path);
        const QString shownName = path.mid(base.size() + 1);
        report << (f.condition == Condition::Good ? shownName : QStringLiteral("%1  (%2)").arg(shownName, conditionText(f.condition)));
    }

    // Saving more within the same minute lands in the same folder: its list gets added to.
    QFile summary(base + QStringLiteral("/What was saved.txt"));
    if (QDir().mkpath(base) && summary.open(QIODevice::Append | QIODevice::Text)) {
        QStringList text = {tr("Saved by DiskForge's Find Lost Files on %1.").arg(QDateTime::currentDateTime().toString(Qt::ISODate)), QString()};
        if (summary.size() > 0)
            text.prepend(QString());
        if (!problems.isEmpty())
            text << tr("Not saved:") << problems << QString();
        text << tr("Saved (%1):").arg(saved) << report;
        if (!namesOnly.isEmpty())
            text << QString() << tr("Only their names were left, so there was nothing to save:") << namesOnly;
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
