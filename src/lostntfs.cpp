// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Find Lost Files' NTFS reader. NTFS keeps a record for every file in its Master File Table
// (the MFT), and deleting a file only marks its record free: the name, the folder it was in,
// its times and the exact list of where its pieces are all stay, until the record is reused
// for a new file. So a deleted file's pieces are known, not guessed, and the cluster bitmap
// says whether anything has been written over them since.
//
// Our own code, from Microsoft's published layout and the libyal documentation of it; The
// Sleuth Kit's isn't used (its licence doesn't mix with GPL-3). Every number read is checked
// before it's used.

#include "lostfs.h"

#include "filetypes.h"
#include "format.h"
#include "lostraw.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QLocale>
#include <QSet>

#include <algorithm>
#include <cstring>
#include <functional>

namespace lost {

namespace {

using raw::bytes;
using raw::le16;
using raw::le32;
using raw::le64;

constexpr quint64 kMaxRecords = 64ull * 1024 * 1024; // more than any real volume has
constexpr int kMaxRuns = 200000;                      // pieces of one file
constexpr int kListLiveFilesUpTo = 100000;            // past that, only deleted files are listed
constexpr int kBatch = 2000;
constexpr quint64 kRootRecord = 5;
constexpr quint64 kExtendRecord = 11; // $Extend: NTFS's own files ($ObjId, $Quota, $UsnJrnl...)

QString tr(const char *text)
{
    return QCoreApplication::translate("lostfiles", text);
}

qint64 fileTime(quint64 ft)
{
    // 100 ns steps since 1601; nothing before 1980 or after 2200 is believed.
    constexpr quint64 k1970 = 116444736000000000ull;
    if (ft < k1970 + 315532800ull * 10000000ull || ft > k1970 + 7258118400ull * 10000000ull)
        return 0;
    return qint64((ft - k1970) / 10000);
}

struct Run {
    quint64 vcn = 0;
    qint64 lcn = -1; // -1: sparse, nothing stored
    quint64 length = 0;
};

// The pieces list of a non-resident attribute. Each entry says how long the piece is and
// where it starts, as a step from the previous one.
QVector<Run> decodeRuns(const uchar *p, const uchar *end, quint64 vcn, quint64 clusters, bool *bad)
{
    QVector<Run> runs;
    qint64 lcn = 0;
    *bad = false;
    while (p < end && *p) {
        const int lengthSize = *p & 0x0F, offsetSize = *p >> 4;
        ++p;
        if (lengthSize == 0 || lengthSize > 8 || offsetSize > 8 || p + lengthSize + offsetSize > end || runs.size() >= kMaxRuns) {
            *bad = true;
            break;
        }
        quint64 length = 0;
        for (int i = lengthSize - 1; i >= 0; --i)
            length = length << 8 | p[i];
        p += lengthSize;
        Run r;
        r.vcn = vcn;
        r.length = length;
        if (offsetSize > 0) {
            qint64 delta = (p[offsetSize - 1] & 0x80) ? -1 : 0;
            for (int i = offsetSize - 1; i >= 0; --i)
                delta = qint64(quint64(delta) << 8 | p[i]);
            p += offsetSize;
            lcn += delta;
            if (lcn < 0 || quint64(lcn) + length > clusters) {
                *bad = true;
                break;
            }
            r.lcn = lcn;
        }
        if (length == 0 || length > clusters) {
            *bad = true;
            break;
        }
        runs.push_back(r);
        vcn += length;
    }
    return runs;
}

struct Attribute {
    quint32 type = 0;
    QString name;
    bool resident = true;
    quint16 flags = 0;
    QByteArray value; // resident
    QVector<Run> runs; // non-resident
    quint64 startVcn = 0;
    quint64 size = 0;        // real size
    quint64 initialized = 0; // what's been written; past it reads as zeros
    bool badRuns = false;
};

struct Record {
    bool valid = false;
    bool inUse = false;
    bool directory = false;
    quint16 sequence = 0;
    quint64 base = 0; // for an extension record: the record it belongs to
    QVector<Attribute> attributes;
};

class Ntfs
{
public:
    Ntfs(Source &source, const Extent &where)
        : m_source(source)
        , m_where(where)
    {
    }

    quint64 clusterSize = 0;
    quint64 clusters = 0;
    quint32 recordSize = 0;
    quint64 records = 0;
    QVector<Extent> mft; // where the MFT is, in order

    bool open()
    {
        const QByteArray boot = raw::read(m_source, m_where.start, 512);
        if (boot.size() < 512)
            return false;
        const uchar *b = bytes(boot);
        if (std::memcmp(b + 3, "NTFS    ", 8) != 0)
            return false;
        const quint32 sectorSize = le16(b + 11);
        const quint8 perCluster = b[13];
        if (sectorSize < 256 || sectorSize > 4096 || (sectorSize & (sectorSize - 1)))
            return false;
        // Big clusters are written as a power of two: 0xF4 is 2^12 sectors.
        const quint64 sectors = perCluster <= 0x80 ? perCluster : (perCluster >= 0xE1 ? 1ull << (256 - perCluster) : 0);
        if (sectors == 0 || (sectors & (sectors - 1)))
            return false;
        clusterSize = sectors * sectorSize;
        const quint64 totalSectors = le64(b + 0x28);
        clusters = std::min(totalSectors * sectorSize, m_where.length) / clusterSize;
        const qint8 perRecord = qint8(b[0x40]);
        const quint64 size = perRecord > 0 ? quint64(perRecord) * clusterSize : (perRecord >= -31 ? 1ull << -perRecord : 0);
        if (size < 512 || size > 65536 || clusters == 0)
            return false;
        recordSize = quint32(size);
        // The MFT's own record says where the MFT is; the mirror has a copy of it.
        for (const quint64 lcn : {le64(b + 0x30), le64(b + 0x38)}) {
            if (lcn == 0 || lcn >= clusters)
                continue;
            const Record self = parse(raw::read(m_source, m_where.start + lcn * clusterSize, recordSize));
            if (!self.valid)
                continue;
            for (const Attribute &a : self.attributes) {
                if (a.type == 0x80 && a.name.isEmpty() && !a.resident && !a.badRuns) {
                    mft = extentsOf(a.runs, a.size);
                    records = std::min(a.size / recordSize, kMaxRecords);
                }
            }
            if (!mft.isEmpty() && records > kRootRecord)
                return true;
        }
        return false;
    }

    // Where a run list's bytes are on the source, cut to `size`.
    QVector<Extent> extentsOf(const QVector<Run> &runs, quint64 size) const
    {
        QVector<Extent> out;
        quint64 left = size;
        for (const Run &r : runs) {
            if (left == 0)
                break;
            const quint64 n = std::min(r.length * clusterSize, left);
            const Extent e {r.lcn < 0 ? Extent::kZeros : m_where.start + quint64(r.lcn) * clusterSize, n};
            if (!out.isEmpty() && !e.zeros() && !out.last().zeros() && out.last().end() == e.start)
                out.last().length += n;
            else if (!out.isEmpty() && e.zeros() && out.last().zeros())
                out.last().length += n;
            else
                out.push_back(e);
            left -= n;
        }
        return out;
    }

    // One MFT record, with its fixups put back. Not valid when it isn't a record at all.
    Record parse(QByteArray data) const
    {
        Record r;
        if (data.size() < qsizetype(recordSize) || std::memcmp(data.constData(), "FILE", 4) != 0)
            return r;
        uchar *d = reinterpret_cast<uchar *>(data.data());
        // Every 512 bytes ends with a check number, the real bytes kept in the update array.
        const quint16 usaOffset = le16(d + 4), usaCount = le16(d + 6);
        if (usaCount == 0 || usaOffset + quint32(usaCount) * 2 > recordSize || quint32(usaCount - 1) * 512 > recordSize)
            return r;
        for (quint16 i = 1; i < usaCount; ++i) {
            uchar *end = d + i * 512 - 2;
            if (std::memcmp(end, d + usaOffset, 2) != 0)
                return r; // torn: half written
            std::memcpy(end, d + usaOffset + i * 2, 2);
        }
        r.sequence = le16(d + 0x10);
        const quint16 flags = le16(d + 0x16);
        r.inUse = flags & 1;
        r.directory = flags & 2;
        r.base = le64(d + 0x20) & 0x0000FFFFFFFFFFFFull;
        quint32 at = le16(d + 0x14);
        const quint32 used = std::min<quint32>(le32(d + 0x18), recordSize);
        for (int n = 0; n < 64 && at + 16 <= used; ++n) {
            const uchar *a = d + at;
            const quint32 type = le32(a);
            if (type == 0xFFFFFFFF)
                break;
            const quint32 length = le32(a + 4);
            if (length < 16 || at + length > used || (length & 7))
                break;
            Attribute attr;
            attr.type = type;
            attr.resident = a[8] == 0;
            attr.flags = le16(a + 0x0C);
            const int nameLength = a[9];
            const quint16 nameOffset = le16(a + 0x0A);
            if (nameLength && nameOffset + quint32(nameLength) * 2 <= length)
                attr.name = raw::utf16le(a + nameOffset, nameLength);
            if (attr.resident) {
                const quint32 valueLength = le32(a + 0x10);
                const quint16 valueOffset = le16(a + 0x14);
                if (valueOffset + quint64(valueLength) <= length) {
                    attr.value = QByteArray(reinterpret_cast<const char *>(a + valueOffset), qsizetype(valueLength));
                    attr.size = attr.initialized = valueLength;
                }
            } else if (length >= 0x40) {
                attr.startVcn = le64(a + 0x10);
                const quint16 runsAt = le16(a + 0x20);
                attr.size = le64(a + 0x30);
                attr.initialized = le64(a + 0x38);
                if (runsAt < length)
                    attr.runs = decodeRuns(a + runsAt, a + length, attr.startVcn, clusters, &attr.badRuns);
                else
                    attr.badRuns = true;
            }
            r.attributes.push_back(attr);
            at += length;
        }
        r.valid = true;
        return r;
    }

    // Records first..first+count, read in as few pieces as the MFT is in. Missing parts are zeros.
    QByteArray readRecords(quint64 first, quint64 count) const
    {
        QByteArray out(qsizetype(count * recordSize), '\0');
        const quint64 from = first * recordSize, to = (first + count) * recordSize;
        quint64 passed = 0; // bytes of the MFT in the extents before this one
        for (const Extent &e : mft) {
            const quint64 lo = std::max(from, passed), hi = std::min(to, passed + e.length);
            if (lo < hi && !e.zeros()) {
                const QByteArray part = raw::read(m_source, e.start + (lo - passed), hi - lo);
                std::memcpy(out.data() + (lo - from), part.constData(), size_t(part.size()));
            }
            passed += e.length;
            if (passed >= to)
                break;
        }
        return out;
    }

    void loadBitmap(const QVector<Run> &runs, quint64 size)
    {
        if (size > clusters / 8 + clusterSize)
            return;
        for (const Extent &e : extentsOf(runs, size))
            m_bitmap += e.zeros() ? QByteArray(qsizetype(e.length), '\0') : raw::read(m_source, e.start, e.length);
    }

    bool inUse(quint64 lcn) const
    {
        if (lcn / 8 >= quint64(m_bitmap.size()))
            return true; // without the bitmap, assume the worst
        return bytes(m_bitmap)[lcn / 8] & (1u << (lcn % 8));
    }

    Source &source() const { return m_source; }

private:
    Source &m_source;
    Extent m_where;
    QByteArray m_bitmap;
};

// What's kept about each record while the MFT is read: enough to build the paths, and the
// whole of what's needed for the files that will be listed.
struct Node {
    QString name;
    quint64 parent = 0;
    quint16 parentSequence = 0;
    quint16 sequence = 0;
    bool inUse = false;
    bool directory = false;
};

struct Candidate {
    quint64 record = 0;
    QString name;
    quint64 parent = 0;
    quint16 parentSequence = 0;
    bool deleted = false;
    qint64 modified = 0;
    QVector<Attribute> data; // the unnamed $DATA, maybe in pieces across extension records
    quint16 dataFlags = 0;
};

// The name to show from a record's $FILE_NAME attributes: the long one over the 8.3 one.
bool pickName(const Record &r, QString *name, quint64 *parent, quint16 *parentSequence)
{
    int best = -1;
    for (const Attribute &a : r.attributes) {
        if (a.type != 0x30 || a.value.size() < 0x42)
            continue;
        const uchar *v = bytes(a.value);
        const int length = v[0x40], space = v[0x41];
        if (0x42 + length * 2 > a.value.size() || length == 0)
            continue;
        const int rank = space == 2 ? 0 : 1; // 2: the DOS 8.3 name, only if there's nothing else
        if (rank <= best)
            continue;
        best = rank;
        *name = raw::utf16le(v + 0x42, length);
        const quint64 ref = le64(v);
        *parent = ref & 0x0000FFFFFFFFFFFFull;
        *parentSequence = quint16(ref >> 48);
    }
    return best >= 0;
}

// Windows' Recycle Bin: a deleted file becomes $Rxxxxxx.ext in C:\$Recycle.Bin\<user>\, and
// $Ixxxxxx.ext next to it says where it was and when it was deleted.
struct Binned {
    QString path; // where it was, C:\Users\...
    qint64 deleted = 0;
};

Binned readRecycleInfo(const QByteArray &info)
{
    Binned b;
    if (info.size() < 24)
        return b;
    const uchar *p = bytes(info);
    const quint64 version = le64(p);
    b.deleted = fileTime(le64(p + 16));
    if (version == 1 && info.size() >= 24 + 2) {
        const int chars = int(std::min<qsizetype>(260, (info.size() - 24) / 2));
        b.path = raw::utf16le(p + 24, chars);
    } else if (version == 2 && info.size() >= 28) {
        const quint32 chars = le32(p + 24);
        if (chars > 0 && 28 + qsizetype(chars) * 2 <= info.size())
            b.path = raw::utf16le(p + 28, int(chars));
    }
    const qsizetype zero = b.path.indexOf(QChar(0));
    if (zero >= 0)
        b.path.truncate(zero);
    return b;
}

} // namespace

bool readNtfs(Source &source, const Extent &where, const Batch &out, const std::atomic<bool> &cancel, Volume *volume)
{
    Ntfs fs(source, where);
    if (!fs.open())
        return false;

    QString label;
    QHash<quint64, Node> nodes;
    QVector<Candidate> candidates;
    QHash<quint64, QVector<Attribute>> extensionData; // $DATA pieces kept in extension records
    QHash<quint64, QByteArray> recycleInfo;           // $I files' contents, by record
    int liveFiles = 0;

    // The MFT, four megabytes at a time.
    const quint64 perChunk = std::max<quint64>(1, 4 * 1024 * 1024 / fs.recordSize);
    for (quint64 first = 0; first < fs.records && !cancel; first += perChunk) {
        const quint64 count = std::min(perChunk, fs.records - first);
        const QByteArray chunk = fs.readRecords(first, count);
        for (quint64 i = 0; i < count; ++i) {
            const quint64 n = first + i;
            const Record r = fs.parse(chunk.mid(qsizetype(i * fs.recordSize), fs.recordSize));
            if (!r.valid)
                continue;
            if (n == 3) { // $Volume: the label
                for (const Attribute &a : r.attributes) {
                    if (a.type == 0x60)
                        label = raw::utf16le(bytes(a.value), int(a.value.size() / 2));
                }
            }
            if (n == 6) { // $Bitmap: which clusters are in use
                for (const Attribute &a : r.attributes) {
                    if (a.type == 0x80 && !a.resident && !a.badRuns)
                        fs.loadBitmap(a.runs, a.size);
                }
            }
            if (r.base != 0) {
                for (const Attribute &a : r.attributes) {
                    if (a.type == 0x80 && a.name.isEmpty())
                        extensionData[r.base].push_back(a);
                }
                continue;
            }
            Node node;
            if (!pickName(r, &node.name, &node.parent, &node.parentSequence)) {
                // Linux's NTFS driver takes the name away before it frees a record. The pieces
                // are still listed, so the file is too, named after what's in it.
                bool hasData = false;
                for (const Attribute &a : r.attributes)
                    hasData = hasData || (a.type == 0x80 && a.name.isEmpty());
                if (r.inUse || r.directory || n < 16 || !hasData)
                    continue;
                node.parent = ~quint64(0);
            } else {
                node.sequence = r.sequence;
                node.inUse = r.inUse;
                node.directory = r.directory;
                nodes.insert(n, node);
            }
            if (r.directory || n < 16 || node.parent == kExtendRecord)
                continue; // folders, and NTFS's own files ($MFT, $LogFile...)
            // On a big drive only deleted files are listed (and the Recycle Bin's): the rest
            // isn't even kept, so the memory goes as far as the drive does.
            const bool binName = node.name.startsWith(QLatin1String("$R")) || node.name.startsWith(QLatin1String("$I"));
            if (r.inUse && ++liveFiles > kListLiveFilesUpTo && !binName) {
                if (liveFiles == kListLiveFilesUpTo + 1) {
                    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const Candidate &k) {
                                         return !k.deleted && !k.name.startsWith(QLatin1String("$R")) && !k.name.startsWith(QLatin1String("$I"));
                                     }),
                                     candidates.end());
                }
                continue;
            }
            Candidate c;
            c.record = n;
            c.name = node.name;
            c.parent = node.parent;
            c.parentSequence = node.parentSequence;
            c.deleted = !r.inUse;
            for (const Attribute &a : r.attributes) {
                if (a.type == 0x10 && a.value.size() >= 16)
                    c.modified = fileTime(le64(bytes(a.value) + 8));
                if (a.type == 0x80 && a.name.isEmpty()) {
                    c.data.push_back(a);
                    c.dataFlags |= a.flags;
                }
            }
            // The Recycle Bin's $I files are tiny and always kept inside the record.
            if (r.inUse && c.name.startsWith(QLatin1String("$I")) && !c.data.isEmpty() && c.data.first().resident)
                recycleInfo.insert(n, c.data.first().value);
            candidates.push_back(std::move(c));
        }
    }
    if (cancel)
        return true;

    const QString top = !label.isEmpty() ? label : tr("NTFS partition at %1").arg(formatSize(where.start));
    if (volume) {
        volume->type = QStringLiteral("ntfs");
        volume->label = top;
        volume->where = where;
    }

    // A folder's path from the root, through the records' parents. A deleted folder whose
    // record has been reused since is lost: what was in it goes under "(folder unknown)".
    QHash<quint64, QString> paths;
    paths.insert(kRootRecord, top);
    const QString unknown = top + QLatin1Char('/') + tr("(folder unknown)");
    std::function<QString(quint64, quint16, int)> pathOf = [&](quint64 record, quint16 sequence, int depth) -> QString {
        const auto known = paths.constFind(record);
        const auto node = nodes.constFind(record);
        if (node == nodes.cend() || !node->directory || depth > 256)
            return unknown;
        // NTFS counts a record's reuses; a folder deleted since counts one more.
        if (sequence != 0 && node->sequence != sequence && !(!node->inUse && node->sequence == quint16(sequence + 1)))
            return unknown;
        if (known != paths.cend())
            return *known;
        const QString parent = record == kRootRecord ? QString() : pathOf(node->parent, node->parentSequence, depth + 1);
        const QString path = parent + QLatin1Char('/') + lost::cleanName(node->name);
        paths.insert(record, path);
        return path;
    };

    // What the Recycle Bin's files were called, by the $R name that stands in for them.
    QHash<QString, Binned> recycled; // "<folder>/$Rxxxxxx.ext" -> where it was
    for (auto it = recycleInfo.cbegin(); it != recycleInfo.cend(); ++it) {
        const Node &n = nodes[it.key()];
        const Binned b = readRecycleInfo(it.value());
        if (!b.path.isEmpty())
            recycled.insert(pathOf(n.parent, n.parentSequence, 0) + QStringLiteral("/$R") + n.name.mid(2), b);
    }

    const bool listLive = liveFiles <= kListLiveFilesUpTo;
    QVector<Found> batch;
    int listed = 0, deleted = 0;
    for (Candidate &c : candidates) {
        if (cancel)
            break;
        QString folder = pathOf(c.parent, c.parentSequence, 0);
        QString name = c.name;
        QString note;
        bool binned = false;
        const qsizetype bin = folder.indexOf(QLatin1String("/$Recycle.Bin/"), 0, Qt::CaseInsensitive);
        if (bin >= 0 && !c.deleted) {
            // In the Recycle Bin: its real name and where it was, from its $I file. A whole
            // folder in the bin is a $R folder with the files inside.
            const QStringList parts = folder.mid(bin + 1).split(QLatin1Char('/'));
            QString base = folder.left(bin) + QLatin1Char('/') + parts.mid(0, 2).join(QLatin1Char('/'));
            QString inside;
            Binned b = recycled.value(folder + QLatin1Char('/') + name);
            if (b.path.isEmpty() && parts.size() > 2) {
                b = recycled.value(base + QLatin1Char('/') + parts[2]);
                inside = parts.mid(3).join(QLatin1Char('/'));
            }
            if (name.startsWith(QLatin1String("$I")) || b.path.isEmpty())
                continue; // the bin's own bookkeeping
            const QString was = b.path.section(QLatin1Char('\\'), 0, -2);
            if (inside.isEmpty()) {
                name = b.path.section(QLatin1Char('\\'), -1);
            } else {
                inside = b.path.section(QLatin1Char('\\'), -1) + QLatin1Char('/') + inside;
            }
            folder = top + QLatin1Char('/') + tr("Recycle Bin") + (inside.isEmpty() ? QString() : QLatin1Char('/') + inside);
            note = tr("In the Recycle Bin. It was in %1, and was deleted %2.")
                       .arg(was, b.deleted ? QLocale().toString(QDateTime::fromMSecsSinceEpoch(b.deleted), QLocale::ShortFormat) : tr("at some point"));
            binned = true;
        }
        if (!c.deleted && !binned && !listLive)
            continue;

        Found f;
        f.name = name;
        f.folder = folder;
        f.origin = Origin::FileSystem;
        f.deleted = c.deleted || binned;
        f.modified = c.modified;
        f.note = note;
        f.type = filetypes::typeForName(name);
        // The data: kept in the record itself, or pieces on the drive (maybe listed across
        // extension records, each with the part of the file it starts at).
        QVector<Attribute> data = c.data + extensionData.value(c.record);
        std::sort(data.begin(), data.end(), [](const Attribute &a, const Attribute &b) { return a.startVcn < b.startVcn; });
        bool broken = false;
        if (!data.isEmpty() && data.first().resident) {
            f.data = data.first().value;
            f.size = quint64(f.data.size());
        } else if (!data.isEmpty()) {
            QVector<Run> runs;
            quint64 expect = 0;
            for (const Attribute &a : std::as_const(data)) {
                if (a.resident || a.startVcn != expect || a.badRuns)
                    broken = true;
                runs += a.runs;
                expect = a.startVcn;
                for (const Run &r : a.runs)
                    expect = r.vcn + r.length;
            }
            f.size = data.first().size;
            const quint64 initialized = std::min(data.first().initialized, f.size);
            f.extents = fs.extentsOf(runs, initialized);
            if (initialized < f.size)
                f.extents.push_back({Extent::kZeros, f.size - initialized}); // never written: zeros
            quint64 covered = 0;
            for (const Extent &e : std::as_const(f.extents))
                covered += e.length;
            if (covered < f.size)
                broken = true;
            if (c.deleted) {
                // Deleted, and part of its clusters in use again: written over since.
                for (const Run &r : std::as_const(runs)) {
                    if (r.lcn < 0)
                        continue;
                    bool taken = false;
                    for (quint64 k = 0; k < std::min<quint64>(r.length, 1 << 20) && !taken; ++k)
                        taken = fs.inUse(quint64(r.lcn) + k);
                    if (taken) {
                        f.condition = Condition::Overwritten;
                        break;
                    }
                }
            }
        }
        if (broken && f.condition == Condition::Good)
            f.condition = Condition::MaybeDamaged;
        if (name.isEmpty()) {
            // No name: one from what kind of file its start says it is.
            f.type = -1;
            const QByteArray start = f.data.isEmpty() ? (f.extents.isEmpty() || f.extents.first().zeros()
                                                              ? QByteArray()
                                                              : raw::read(source, f.extents.first().start, std::min<quint64>(f.extents.first().length, 4096)))
                                                       : f.data.left(4096);
            const QVector<int> kinds = start.size() >= 32 ? filetypes::candidates(bytes(start), int(start.size())) : QVector<int>();
            if (!kinds.isEmpty())
                f.type = kinds.first();
            f.name = tr("Nameless file %1").arg(c.record) + (f.type >= 0 ? QLatin1Char('.') + filetypes::types()[f.type].id : QString());
            f.folder = unknown;
            f.nameGuessed = true;
            f.note = tr("NTFS doesn't have its name or folder anymore (Linux's NTFS driver takes them away when it deletes a "
                        "file), so it's named after what's in it.");
        }
        if (c.dataFlags & 0x0001) {
            f.condition = Condition::MaybeDamaged;
            f.note = tr("Windows kept it compressed. DiskForge can't unpack NTFS compression yet, so it comes back as it is on the drive.");
        } else if (c.dataFlags & 0x4000) {
            f.condition = Condition::MaybeDamaged;
            f.note = tr("Windows kept it encrypted (EFS). It comes back still encrypted: only the Windows account that encrypted it "
                        "can open it.");
        }
        batch.push_back(std::move(f));
        ++listed;
        deleted += c.deleted || binned;
        if (batch.size() >= kBatch)
            out(std::exchange(batch, {}));
    }
    if (!batch.isEmpty())
        out(std::move(batch));
    if (volume) {
        volume->files = listed;
        volume->deleted = deleted;
    }
    return true;
}

} // namespace lost
