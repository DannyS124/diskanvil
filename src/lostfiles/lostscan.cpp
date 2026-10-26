// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lostscan.h"

#include "applog.h"
#include "blockio.h"
#include "filetypes.h"
#include "fragments.h"
#include "lostfs.h"
#include "naming.h"
#include "thumbcache.h"

#include <QCoreApplication>
#include <QSet>
#include <QStringEncoder>
#include <QStringDecoder>
#include <QByteArrayMatcher>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
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
    if (file.packing.packed()) {
        // Kept compressed: read through the unpacking. One that can't be unpacked reads as
        // unreadable, all zeros.
        std::unique_ptr<Layer> unpacked = unpack::layer(m_source, m_extents, m_data, file.packing, m_size);
        m_extents = {{0, unpacked ? m_size : 0}};
        m_source = unpacked ? Source::fromLayer(std::move(unpacked)) : Source::fromMemory(QByteArray());
        m_data.clear();
    }
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
    // A tar keeps whole files as they are: the deep scan still looks inside (it's not listed
    // twice: its start is known).
    static const int tar = filetypes::typeNumber(QStringLiteral("tar"));
    if (file.type == tar)
        return;
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
                    // The page file, swap and hibernation file hold pieces of what was open
                    // (pictures, documents): the deep scan looks through them too.
                    static const QStringList swap = {QStringLiteral("pagefile.sys"), QStringLiteral("swapfile.sys"),
                                                     QStringLiteral("hiberfil.sys"), QStringLiteral("swapfile"), QStringLiteral("swap.img")};
                    if (!swap.contains(f.name, Qt::CaseInsensitive))
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
    QVector<LeftoverFolder> leftovers;
    quint64 leftoverUntil = 0;
    QVector<quint64> leftoverRecords; // NTFS file records outside today's MFT
    // Films (all) and photos whose data breaks off wait for the end of the scan: then, with
    // everything else found, their other pieces are looked for (fragments.h).
    QVector<Found> pending;
    QVector<Extent> whole; // what files found whole take up
    QVector<quint64> indexes;
    static const int jpg = filetypes::typeNumber(QStringLiteral("jpg")), mp4 = filetypes::typeNumber(QStringLiteral("mp4"));
    // Inside a file whose kind only says how long it is (a PSD, a film): looked through still,
    // at 4 KiB steps past its first 64 KiB, for whole files only. A fragmented one measured
    // whole would otherwise hide the files in its range.
    quint64 insideFrom = 0, insideUntil = 0, insideBase = 0;
    qCInfo(lcOps).noquote() << "Find Lost Files: deep scan of" << total << "bytes";

    qsizetype range = 0; // the first known range that doesn't end before pos
    auto known = [this, &range](quint64 at) {
        while (range < m_skipRanges.size() && m_skipRanges[range].end() <= at)
            ++range;
        return range < m_skipRanges.size() && m_skipRanges[range].start <= at;
    };
    for (quint64 pos = m_start; pos < m_end && !m_cancel;) {
        // Inside a file that was just found whole: no need to look for files there again, but
        // a film's index can be in the middle of it (when that file's in pieces itself).
        if (skipUntil > pos) {
            const quint64 to = std::min(skipUntil / kSector * kSector, m_end);
            for (quint64 at = pos; at < to && !m_cancel; at += kChunk) {
                const quint64 len = std::min(kChunk, to - at);
                m_source->read(at, chunk.data(), len);
                fragments::noteIndexes(reinterpret_cast<const uchar *>(chunk.constData()), qsizetype(len), at, &indexes);
            }
            pos = to;
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
        fragments::noteIndexes(reinterpret_cast<const uchar *>(chunk.constData()), qsizetype(n), pos, &indexes);
        for (quint64 off = 0; off + 32 <= n && !m_cancel; off += kSector) {
            const quint64 at = pos + off;
            if (at < skipUntil || m_skipStarts.contains(at) || known(at))
                continue;
            const bool inside = at >= insideFrom && at < insideUntil;
            if (inside && (at - insideBase) % 4096 != 0)
                continue;
            const uchar *head = reinterpret_cast<const uchar *>(chunk.constData()) + off;
            // An NTFS file record outside today's MFT: listed at the end, by name.
            if (leftoverRecordAt(head, int(std::min(kSector, n - off)))) {
                if (leftoverRecords.size() < kLeftoverFolderLimit)
                    leftoverRecords << at;
                continue;
            }
            // A folder a quick format left behind: its files are listed at the end, by name.
            if (at >= leftoverUntil && leftovers.size() < kLeftoverFolderLimit) {
                LeftoverFolder folder;
                if (leftoverFolderAt(*m_source, at, head, int(std::min(kSector, n - off)), &folder)) {
                    leftoverUntil = at + quint64(folder.data.size());
                    leftovers << std::move(folder);
                    continue;
                }
            }
            QVector<int> candidates = filetypes::candidates(head, int(std::min(kSector, n - off)));
            // Plain text, which has no mark of its own, is looked for last.
            static const int text = filetypes::typeNumber(QStringLiteral("txt"));
            if (filetypes::plainTextStart(head, int(std::min(kSector, n - off))))
                candidates << text;
            for (int type : candidates) {
                const quint64 end = m_end;
                Source *source = m_source.get();
                const filetypes::Reader reader = [source, at, end](quint64 offset, char *buf, qint64 len) -> qint64 {
                    if (at + offset >= end)
                        return 0;
                    return qint64(source->read(at + offset, buf, qint64(std::min<quint64>(quint64(len), end - at - offset))));
                };
                const filetypes::Measured m = filetypes::measure(type, reader);
                if (m.size == 0 || m.type < 0 || (inside && (!m.complete || m.type == text)))
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
                if (f.condition != Condition::Unreadable) // a failing drive isn't read there again
                    nameFromContents(f, *m_source);
                // Exactly the size of a file a file system only has the name of, and the only one
                // like it: that's its name and folder.
                if (const auto name = m_names.constFind({filetypes::startType(m.type), m.size}); name != m_names.cend()) {
                    f.name = name->name;
                    f.folder = name->folder;
                    f.origin = Origin::FileSystem;
                    f.deleted = true;
                    f.created = name->created;
                    f.modified = name->modified;
                    f.accessed = name->accessed;
                    f.changed = name->changed;
                    f.deletedAt = name->deletedAt;
                    f.note = tr("Named from the file system: it still had a deleted file called that, in that folder, exactly this size.");
                    m_names.erase(name);
                }
                const filetypes::Category category = types[m.type].category;
                const bool film = filetypes::startType(m.type) == mp4 && (category == filetypes::Category::Video || category == filetypes::Category::Music);
                if (f.condition != Condition::Unreadable && (film || (m.type == jpg && !m.complete)))
                    pending << f;
                else
                    batch << f;
                if (m.complete && !film)
                    whole << f.extents.first();
                // A whole file isn't searched again inside: a JPEG's own preview picture
                // shouldn't come back as a second photo. Nor is text: its middle would come
                // back as more text files. A tar is, though: it keeps whole files as they are,
                // and they're easier to use found on their own too.
                static const int tar = filetypes::typeNumber(QStringLiteral("tar"));
                if ((m.complete || m.type == text) && m.type != tar) {
                    const quint64 end = (at + m.size + kSector - 1) / kSector * kSector;
                    if (m.type == text || filetypes::checksInside(m.type)) {
                        skipUntil = std::max(skipUntil, end);
                    } else {
                        skipUntil = std::max(skipUntil, std::min(end, at + 64 * 1024));
                        if (end > insideUntil) {
                            insideFrom = at + 64 * 1024;
                            insideUntil = end;
                            insideBase = at;
                        }
                    }
                }
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
    // Films and broken photos: their other pieces, if they're somewhere else on the drive.
    if (!pending.isEmpty() && !m_cancel) {
        QVector<Extent> taken = m_skipRanges + whole;
        std::sort(taken.begin(), taken.end(), [](const Extent &a, const Extent &b) { return a.start < b.start; });
        quint64 cluster = 4096;
        for (const Volume &v : std::as_const(m_volumes)) {
            if (v.clusterSize >= 512 && v.clusterSize <= 1024 * 1024) {
                cluster = v.clusterSize;
                break;
            }
        }
        fragments::Context context{*m_source, cluster, [&taken](quint64 at) {
                                        auto it = std::upper_bound(taken.cbegin(), taken.cend(), at, [](quint64 v, const Extent &e) { return v < e.start; });
                                        return it != taken.cbegin() && std::prev(it)->end() > at;
                                    },
                                    indexes, m_cancel};
        for (Found &f : pending) {
            if (m_cancel)
                break;
            if (f.type == jpg)
                fragments::jpeg(context, f);
            else
                fragments::mp4(context, f);
            batch << std::move(f);
        }
    }
    // The files in folders and records from before a format, but not the ones a file system
    // lists now.
    if (!m_cancel && (!leftovers.isEmpty() || !leftoverRecords.isEmpty())) {
        QVector<Found> old = filesInLeftoverFolders(*m_source, leftovers, m_volumes);
        old += filesInLeftoverRecords(*m_source, leftoverRecords, m_volumes);
        for (Found &f : old) {
            if (m_cancel)
                break;
            if (!f.extents.isEmpty() && m_skipStarts.contains(f.extents.first().start))
                continue;
            checkContents(m_source, f, f.size <= kFullCheckLimit);
            batch << std::move(f);
        }
    }
    if (!batch.isEmpty())
        Q_EMIT found(batch);
    qCInfo(lcOps).noquote() << "Find Lost Files: deep scan" << (m_cancel ? "stopped" : "done");
    Q_EMIT finished(m_cancel);
}

QVector<int> lost::Merger::add(QVector<Found> &list, const QVector<Found> &files)
{
    QVector<int> replaced;
    for (const Found &f : files) {
        const QString path = f.folder + QLatin1Char('/') + f.name;
        const quint64 start = f.extents.isEmpty() || f.extents.first().zeros() ? ~quint64(0) : f.extents.first().start;
        int place = -1;
        if (f.nameOnly) {
            if (m_nameOnly.contains(path))
                continue;
        } else if (start != ~quint64(0) && f.origin == Origin::FileSystem && m_contentsAt.contains(start)) {
            place = m_contentsAt.take(start); // named now: it takes the place of the one found by contents
        } else if (start != ~quint64(0) && f.origin == Origin::Contents && m_systemAt.contains(start)) {
            continue; // already listed with its name
        } else if (m_nameOnly.contains(path)) {
            place = m_nameOnly.take(path); // what was only a name, now with what was in it
        }
        if (place >= 0) {
            list[place] = f;
            replaced << place;
        } else {
            place = int(list.size());
            list << f;
        }
        if (f.nameOnly)
            m_nameOnly.insert(path, place);
        else if (start != ~quint64(0))
            (f.origin == Origin::FileSystem ? m_systemAt : m_contentsAt).insert(start, place);
    }
    return replaced;
}

void lost::Merger::clear()
{
    m_contentsAt.clear();
    m_systemAt.clear();
    m_nameOnly.clear();
}

void lost::DeepScan::setNames(const QVector<Found> &files)
{
    QHash<QPair<int, quint64>, int> count;
    for (const Found &f : files) {
        if (!f.nameOnly || f.type < 0 || f.size < 4096)
            continue;
        const QPair<int, quint64> key{filetypes::startType(f.type), f.size};
        if (++count[key] == 1)
            m_names.insert(key, f);
        else
            m_names.remove(key); // two names that size: can't tell which
    }
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
    QList<QByteArray> sums;
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
        QCryptographicHash sha256(QCryptographicHash::Sha256);
        bool ok = true;
        for (quint64 at = 0; at < f.size && ok && !m_cancel;) {
            const qint64 n = reader.read(at, buffer.data(), buffer.size());
            if (n > 0)
                sha256.addData(QByteArrayView(buffer.constData(), n));
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
        sums << sha256.result().toHex() + "  " + shownName.toUtf8();
    }
    // Each file's SHA-256, the way sha256sum writes them: proof later that they're unchanged.
    QFile fingerprints(base + QStringLiteral("/SHA256SUMS"));
    if (!sums.isEmpty() && QDir().mkpath(base) && fingerprints.open(QIODevice::Append | QIODevice::Text))
        fingerprints.write(sums.join('\n') + '\n');

    // Saving more within the same minute lands in the same folder: its list gets added to.
    QFile summary(base + QStringLiteral("/What was saved.txt"));
    if (QDir().mkpath(base) && summary.open(QIODevice::Append | QIODevice::Text)) {
        QStringList text = {tr("Saved by DiskAnvil's Find Lost Files on %1.").arg(QDateTime::currentDateTime().toString(Qt::ISODate)), QString()};
        if (summary.size() > 0)
            text.prepend(QString());
        if (!problems.isEmpty())
            text << tr("Not saved:") << problems << QString();
        text << tr("Saved (%1):").arg(saved) << report;
        if (saved > 0)
            text << QString() << tr("Each file's SHA-256 fingerprint is in SHA256SUMS, to show later that they haven't changed (on Linux: "
                                    "sha256sum -c SHA256SUMS, in this folder).");
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

QVector<lost::Extent> lost::slice(const QVector<Extent> &extents, quint64 from, quint64 length)
{
    QVector<Extent> out;
    quint64 pos = 0;
    for (const Extent &e : extents) {
        const quint64 end = pos + e.length;
        if (end > from && pos < from + length) {
            const quint64 a = std::max(from, pos), b = std::min(from + length, end);
            out.push_back({e.zeros() ? Extent::kZeros : e.start + (a - pos), b - a});
        }
        pos = end;
        if (pos >= from + length)
            break;
    }
    return out;
}

QVector<lost::Found> lost::thumbnailsIn(const std::shared_ptr<Source> &source, const Found &cache)
{
    QVector<Found> out;
    if (cache.nameOnly || cache.size == 0 || cache.size > 1024ull * 1024 * 1024)
        return out;
    FileReader reader(cache.from ? cache.from : source, cache);
    const QByteArray bytes = reader.head(qint64(cache.size));
    const QString where = cache.folder.isEmpty() ? cache.name : cache.folder + QLatin1Char('/') + cache.name;
    int number = 0;
    for (const thumbcache::Thumb &t : thumbcache::read(bytes)) {
        Found f;
        ++number;
        const QString base = t.name.isEmpty() ? QCoreApplication::translate("lostscan", "Small picture %1").arg(number, 5, 10, QLatin1Char('0'))
                                              : QCoreApplication::translate("lostscan", "%1 (small copy)").arg(QFileInfo(t.name).completeBaseName());
        f.name = cleanName(base + QLatin1Char('.') + t.kind);
        f.folder = QCoreApplication::translate("lostscan", "Small pictures Windows kept");
        f.type = filetypes::typeForName(f.name);
        f.size = t.size;
        if (t.data.isEmpty() && cache.packing.packed()) {
            f.data = bytes.mid(qsizetype(t.offset), qsizetype(t.size)); // Windows keeps the cache compressed: from what was unpacked
        } else if (t.data.isEmpty()) {
            f.extents = slice(cache.extents, t.offset, t.size);
            f.from = cache.from;
        } else {
            f.data = t.data;
        }
        f.origin = Origin::FileSystem;
        f.deleted = true;
        f.condition = cache.condition;
        f.modified = cache.modified;
        f.note = QCoreApplication::translate("lostscan", "Windows kept this small copy of a photo or film in %1. The photo itself may be gone.")
                     .arg(where);
        out.push_back(f);
    }
    return out;
}

lost::WordSearch::WordSearch(std::shared_ptr<Source> source, QStringList words, QVector<Found> known)
    : m_source(std::move(source))
    , m_words(std::move(words))
    , m_known(std::move(known))
{
}

namespace {

// ASCII letters in lower case: the search ignores capitals (other letters are matched as they are).
void lowerAscii(char *p, qsizetype n)
{
    for (qsizetype i = 0; i < n; ++i) {
        if (p[i] >= 'A' && p[i] <= 'Z')
            p[i] = char(p[i] + 32);
    }
}

bool textLike(uchar c)
{
    return c >= 0x20 || c == '\t' || c == '\n' || c == '\r';
}

} // namespace

void lost::WordSearch::run()
{
    // Each word, in the ways text is kept: UTF-8, UTF-16 (Windows, Word), and Windows' western code page.
    struct Pattern {
        QByteArrayMatcher matcher;
        int word;
        bool wide;
        qsizetype length;
    };
    QVector<Pattern> patterns;
    QStringEncoder western("windows-1252");
    for (int w = 0; w < m_words.size(); ++w) {
        const QString word = m_words[w];
        QByteArray utf8 = word.toUtf8(), wide(reinterpret_cast<const char *>(word.utf16()), word.size() * 2);
        lowerAscii(utf8.data(), utf8.size());
        lowerAscii(wide.data(), wide.size());
        patterns.push_back({QByteArrayMatcher(utf8), w, false, utf8.size()});
        patterns.push_back({QByteArrayMatcher(wide), w, true, wide.size()});
        if (western.isValid()) {
            QByteArray old = western(word);
            lowerAscii(old.data(), old.size());
            if (!western.hasError() && old != utf8)
                patterns.push_back({QByteArrayMatcher(old), w, false, old.size()});
        }
    }
    qsizetype longest = 0;
    for (const Pattern &p : std::as_const(patterns))
        longest = std::max(longest, p.length);
    struct Hit {
        quint64 at;
        int word;
        bool wide;
    };
    QVector<Hit> hits;
    const quint64 size = m_source->size();
    QByteArray chunk(qsizetype(kChunk + quint64(longest)), Qt::Uninitialized);
    for (quint64 pos = 0; pos < size && !m_cancel && hits.size() < 1000000; pos += kChunk) {
        const qsizetype n = qsizetype(std::min<quint64>(quint64(chunk.size()), size - pos));
        m_source->read(pos, chunk.data(), quint64(n));
        lowerAscii(chunk.data(), n);
        const QByteArrayView view(chunk.constData(), n);
        for (const Pattern &p : std::as_const(patterns)) {
            for (qsizetype at = p.matcher.indexIn(view); at >= 0 && quint64(at) < kChunk; at = p.matcher.indexIn(view, at + 1))
                hits.push_back({pos + quint64(at), p.word, p.wide});
        }
        Q_EMIT progress(std::min(pos + kChunk, size), size);
    }
    std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) { return a.at < b.at; });
    // Places where every word is, close together (within 8 KiB of the one before).
    QVector<Found> out;
    int count = 0;
    for (qsizetype i = 0; i < hits.size() && !m_cancel;) {
        qsizetype j = i + 1;
        while (j < hits.size() && hits[j].at - hits[j - 1].at <= 8192)
            ++j;
        QSet<int> words;
        bool wide = false;
        for (qsizetype k = i; k < j; ++k) {
            words.insert(hits[k].word);
            wide = wide || hits[k].wide;
        }
        if (words.size() == m_words.size()) {
            // The text around them, to where it stops being text (256 KiB each way at most).
            const quint64 first = hits[i].at, last = hits[j - 1].at;
            const quint64 from = first > 256 * 1024 ? first - 256 * 1024 : 0;
            const quint64 to = std::min(size, last + 256 * 1024);
            QByteArray around(qsizetype(to - from), Qt::Uninitialized);
            m_source->read(from, around.data(), quint64(around.size()));
            qsizetype start = qsizetype(first - from), end = qsizetype(last - from);
            const int step = wide ? 2 : 1;
            const auto text = [&around, wide](qsizetype at) {
                if (at < 0 || at + (wide ? 1 : 0) >= around.size())
                    return false;
                return wide ? textLike(uchar(around[at])) && uchar(around[at + 1]) < 0x20 && (uchar(around[at + 1]) == 0 || uchar(around[at]) >= 0x20)
                            : textLike(uchar(around[at]));
            };
            while (text(start - step))
                start -= step;
            while (text(end))
                end += step;
            QString content;
            if (wide) {
                content = QString::fromUtf16(reinterpret_cast<const char16_t *>(around.constData() + start), (end - start) / 2);
            } else {
                QStringDecoder utf8(QStringDecoder::Utf8);
                content = utf8(around.mid(start, end - start));
                if (utf8.hasError())
                    content = QString::fromLatin1(around.mid(start, end - start));
            }
            Found f;
            ++count;
            f.name = cleanName(QStringLiteral("%1 %2").arg(QCoreApplication::translate("lostscan", "Text with"), m_words.join(QLatin1Char(' ')))
                                   .left(80))
                   + QStringLiteral(" %1.txt").arg(count, 4, 10, QLatin1Char('0'));
            f.folder = QCoreApplication::translate("lostscan", "Found by their words");
            f.data = QByteArray("\xEF\xBB\xBF") + content.toUtf8(); // as UTF-8 text, whatever it was kept as
            f.size = quint64(f.data.size());
            f.extents = {{from + quint64(start), quint64(end - start)}};
            f.origin = Origin::FileSystem;
            f.deleted = true;
            f.condition = Condition::Good;
            // Inside a file that was found already?
            for (const Found &known : std::as_const(m_known)) {
                for (const Extent &e : known.extents) {
                    if (!e.zeros() && first >= e.start && first < e.end()) {
                        f.note = QCoreApplication::translate("lostscan", "It's inside \"%1\", which was found already.").arg(known.name);
                        break;
                    }
                }
                if (!f.note.isEmpty())
                    break;
            }
            if (f.note.isEmpty())
                f.note = QCoreApplication::translate("lostscan", "The text found around the words, %1 into the drive%2.")
                             .arg(first)
                             .arg(wide ? QCoreApplication::translate("lostscan", " (kept the way Windows and Word keep text)") : QString());
            out.push_back(f);
            if (out.size() >= 250) {
                Q_EMIT found(std::exchange(out, {}));
            }
        }
        i = j;
    }
    if (!out.isEmpty())
        Q_EMIT found(out);
    Q_EMIT finished(m_cancel, count);
}

lost::CopyFinder::CopyFinder(std::shared_ptr<Source> source, QVector<Found> files, QVector<int> indexes)
    : m_source(std::move(source))
    , m_files(std::move(files))
    , m_indexes(std::move(indexes))
{
}

void lost::CopyFinder::run()
{
    // Only sizes more than one file has can have copies.
    QHash<quint64, QVector<int>> bySize;
    for (int i = 0; i < m_files.size(); ++i) {
        const Found &f = m_files[i];
        if (!f.nameOnly && f.size > 0 && f.condition != Condition::Unreadable)
            bySize[f.size] << i;
    }
    QVector<QVector<int>> groups;
    quint64 total = 0, done = 0;
    for (auto it = bySize.cbegin(); it != bySize.cend(); ++it) {
        if (it.value().size() > 1) {
            groups << it.value();
            total += it.key() * quint64(it.value().size());
        }
    }
    const auto digest = [this, &done, total](const Found &f, qint64 most) {
        FileReader reader(m_source, f);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        QByteArray buffer(1 << 20, Qt::Uninitialized);
        const quint64 end = std::min<quint64>(f.size, quint64(most));
        for (quint64 at = 0; at < end && !m_cancel;) {
            const qint64 n = reader.read(at, buffer.data(), qint64(std::min<quint64>(quint64(buffer.size()), end - at)));
            if (n <= 0)
                return QByteArray(); // can't be read: not called a copy of anything
            hash.addData(QByteArrayView(buffer.constData(), n));
            at += quint64(n);
            if (most > 65536) {
                done += quint64(n);
                Q_EMIT progress(done, total);
            }
        }
        return hash.result();
    };
    QHash<int, int> copyOf;
    quint64 copyBytes = 0;
    for (const QVector<int> &group : std::as_const(groups)) {
        // Their starts first: most files of one size differ there already.
        QHash<QByteArray, QVector<int>> byStart;
        for (int i : group) {
            const QByteArray start = digest(m_files[i], 65536);
            if (!start.isEmpty())
                byStart[start] << i;
        }
        for (auto same = byStart.cbegin(); same != byStart.cend(); ++same) {
            if (same->size() < 2)
                continue;
            QHash<QByteArray, int> first;
            for (int i : *same) {
                // A small file's start is all of it.
                const QByteArray whole = m_files[i].size <= 65536 ? same.key() : digest(m_files[i], std::numeric_limits<qint64>::max());
                if (whole.isEmpty() || m_cancel)
                    continue;
                if (const auto it = first.constFind(whole); it != first.cend()) {
                    copyOf.insert(m_indexes[i], m_indexes[*it]);
                    copyBytes += m_files[i].size;
                } else {
                    first.insert(whole, i);
                }
            }
        }
        if (m_cancel)
            break;
    }
    Q_EMIT finished(copyOf, copyBytes, m_cancel);
}

void lost::nameFromContents(Found &file, Source &source)
{
    if (file.extents.size() != 1 || file.type < 0 || file.packing.packed())
        return;
    const quint64 start = file.extents.first().start, size = file.size;
    const naming::Read read = [&source, start, size](quint64 at, qint64 len) {
        if (at >= size || len <= 0)
            return QByteArray();
        QByteArray out(qsizetype(std::min<quint64>(quint64(len), size - at)), Qt::Uninitialized);
        const qint64 n = qint64(source.read(start + at, out.data(), quint64(out.size())));
        out.truncate(qsizetype(std::max<qint64>(0, n)));
        return out;
    };
    const naming::Inside inside = naming::fromContents(file.type, size, read);
    if (!inside.name.trimmed().isEmpty()) {
        // "Re: Photos" and "14/07" read better as "Re - Photos" and "14-07" than with the "_" cleanName puts there.
        QString name = inside.name;
        name.replace(QStringLiteral(": "), QStringLiteral(" - ")).replace(QLatin1Char(':'), QLatin1Char('-')).replace(QLatin1Char('/'), QLatin1Char('-'));
        file.name = cleanName(name) + QLatin1Char('.') + filetypes::types()[file.type].id;
    }
    file.inside = inside.when;
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
