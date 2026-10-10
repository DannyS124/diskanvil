// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lostfs.h"

#include "filetypes.h"
#include "format.h"
#include "lostraw.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QTimeZone>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace lost {

namespace {

constexpr int kMaxDepth = 64;             // folders inside folders
constexpr int kMaxListed = 2000000;       // files listed from one file system
constexpr quint64 kMaxFolderBytes = 4 * 1024 * 1024; // FAT's own limit is 2 MiB of entries
constexpr quint64 kMaxExfatFolderBytes = 256 * 1024 * 1024;
constexpr int kMaxDeletedFolderClusters = 64;
constexpr int kBatch = 2000;
constexpr quint64 kFatPiece = 64 * 1024;

QString tr(const char *text)
{
    return QCoreApplication::translate("lostfiles", text);
}

using raw::bytes;
using raw::le16;
using raw::le32;
using raw::le64;
using raw::utf16le;

QByteArray readBytes(Source &source, quint64 at, quint64 len)
{
    return raw::read(source, at, len);
}

// What a folder entry says, whichever file system it came from.
struct Entry {
    QString name;
    bool nameGuessed = false;
    bool isDir = false;
    bool deleted = false;
    quint32 first = 0;
    quint64 size = 0;
    qint64 modified = 0;
    bool contiguous = false; // exFAT's "no FAT chain": one piece, exactly where it says
};

// A file system's clusters: where each one is, what the table says comes after it, whether
// it's in use now. FAT and exFAT share all of it except how the table and "in use" are read.
class Clusters
{
public:
    Clusters(Source &source, const Extent &where)
        : m_source(source)
        , m_where(where)
    {
    }
    virtual ~Clusters() = default;

    quint64 clusterSize = 0;
    quint32 count = 0; // clusters, numbered from 2
    quint64 heap = 0;  // where cluster 2 starts, from the start of the file system
    QString label;

    bool valid(quint32 c) const { return c >= 2 && c - 2 < count; }
    quint64 offsetOf(quint32 c) const { return m_where.start + heap + quint64(c - 2) * clusterSize; }
    virtual quint32 next(quint32 c) = 0;     // what the table says follows c
    virtual bool isEnd(quint32 value) const = 0;
    virtual bool inUse(quint32 c) = 0;

    // As many clusters as `bytes` takes, but at least one.
    quint32 clustersFor(quint64 bytes) const { return quint32(std::max<quint64>(1, (bytes + clusterSize - 1) / clusterSize)); }

    // The table's chain from `first`, at most `most` long. Stops at its end, at anything that
    // isn't a cluster, and at a loop.
    QVector<quint32> chain(quint32 first, quint32 most, bool *ended = nullptr)
    {
        QVector<quint32> out;
        if (ended)
            *ended = false;
        QSet<quint32> seen;
        for (quint32 c = first; valid(c) && quint32(out.size()) < most;) {
            if (out.size() > 4096) { // only long chains are worth guarding against loops
                if (seen.contains(c))
                    break;
                seen.insert(c);
            }
            out.push_back(c);
            const quint32 n = next(c);
            if (isEnd(n)) {
                if (ended)
                    *ended = true;
                break;
            }
            c = n;
        }
        return out;
    }

    // `need` clusters from `first` on, as a deleted file's were most likely laid out: in a row,
    // stepping over the ones that are in use now (another file's, written since).
    QVector<quint32> guessRun(quint32 first, quint32 need, bool *skipped)
    {
        QVector<quint32> out;
        *skipped = false;
        if (!valid(first))
            return out;
        out.push_back(first);
        for (quint32 c = first + 1; quint32(out.size()) < need && valid(c); ++c) {
            if (inUse(c)) {
                *skipped = true;
                continue;
            }
            out.push_back(c);
        }
        return out;
    }

    QVector<Extent> extentsOf(const QVector<quint32> &clusters, quint64 size) const
    {
        QVector<Extent> out;
        quint64 left = size;
        for (quint32 c : clusters) {
            if (left == 0)
                break;
            const quint64 n = std::min(clusterSize, left);
            const quint64 at = offsetOf(c);
            if (!out.isEmpty() && out.last().end() == at)
                out.last().length += n;
            else
                out.push_back({at, n});
            left -= n;
        }
        return out;
    }

    QByteArray readClusters(const QVector<quint32> &clusters)
    {
        QByteArray out;
        for (const Extent &e : extentsOf(clusters, quint64(clusters.size()) * clusterSize))
            out += readBytes(m_source, e.start, e.length);
        return out;
    }

protected:
    Source &m_source;
    Extent m_where;
};

// --- FAT

quint8 shortNameChecksum(const uchar *name)
{
    quint8 sum = 0;
    for (int i = 0; i < 11; ++i)
        sum = quint8(((sum & 1) << 7) + (sum >> 1) + name[i]);
    return sum;
}

bool shortNameChar(uchar c)
{
    return c >= 0x20 && !std::strchr("\"*+,./:;<=>?[\\]|", c) && !(c >= 'a' && c <= 'z');
}

// FAT keeps the PC's local time, without saying which zone; this PC's is the best guess.
qint64 fatTime(quint16 date, quint16 time)
{
    if (date == 0)
        return 0;
    const QDate d(1980 + (date >> 9), (date >> 5) & 0xF, date & 0x1F);
    if (!d.isValid())
        return 0;
    const QTime t((time >> 11) & 0x1F, (time >> 5) & 0x3F, (time & 0x1F) * 2);
    return QDateTime(d, t.isValid() ? t : QTime(0, 0)).toMSecsSinceEpoch();
}

class Fat : public Clusters
{
public:
    using Clusters::Clusters;

    int bits = 0; // 12, 16 or 32
    quint64 fatStart = 0, fatBytes = 0;
    quint64 rootStart = 0; // FAT12/16: the fixed root folder
    quint32 rootEntries = 0;
    quint32 rootCluster = 0; // FAT32

    bool open()
    {
        if (parseBoot(readBytes(m_source, m_where.start, 512)))
            return true;
        // FAT32 keeps a copy of its boot sector, usually 6 sectors in.
        for (const quint64 sector : {512, 4096}) {
            if (parseBoot(readBytes(m_source, m_where.start + 6 * sector, 512)))
                return true;
        }
        return false;
    }

    QString typeName() const { return bits == 32 ? QStringLiteral("FAT32") : bits == 16 ? QStringLiteral("FAT16") : QStringLiteral("FAT12"); }

    quint32 next(quint32 c) override
    {
        const quint64 byte = bits == 32 ? quint64(c) * 4 : bits == 16 ? quint64(c) * 2 : quint64(c) + c / 2;
        if (byte + (bits == 32 ? 4 : 2) > fatBytes)
            return 0;
        const quint64 piece = byte / kFatPiece;
        auto it = m_table.find(piece);
        if (it == m_table.end()) {
            if (m_table.size() > 4096)
                m_table.clear();
            // A few bytes past the piece, so an entry across its end is read whole.
            it = m_table.insert(piece, readBytes(m_source, m_where.start + fatStart + piece * kFatPiece,
                                                 std::min(kFatPiece + 4, fatBytes - piece * kFatPiece)));
        }
        const QByteArray &data = *it;
        const quint64 inside = byte - piece * kFatPiece;
        if (inside + (bits == 32 ? 4 : 2) > quint64(data.size()))
            return 0;
        const uchar *p = bytes(data) + inside;
        if (bits == 32)
            return le32(p) & 0x0FFFFFFF;
        if (bits == 16)
            return le16(p);
        const quint16 v = le16(p);
        return (c & 1) ? v >> 4 : v & 0xFFF;
    }

    bool isEnd(quint32 value) const override
    {
        // The end of a chain, or a bad cluster: either way nothing follows.
        return bits == 32 ? value >= 0x0FFFFFF7 : bits == 16 ? value >= 0xFFF7 : value >= 0xFF7;
    }

    bool inUse(quint32 c) override { return next(c) != 0; }

    // The entries of one folder in a FAT file system. In a deleted folder everything counts as
    // deleted, marked so or not.
    QVector<Entry> parse(const QByteArray &data, bool allDeleted, QString *volumeLabel = nullptr)
    {
        QVector<Entry> out;
        struct Piece {
            bool deleted;
            quint8 order;
            quint8 checksum;
            QString chars;
        };
        QVector<Piece> pieces; // long-name pieces since the last short entry, nearest last
        for (qsizetype i = 0; i + 32 <= data.size(); i += 32) {
            const uchar *e = bytes(data) + i;
            if (e[0] == 0x00)
                break; // FAT never uses anything after this
            const uchar attr = e[11];
            const bool gone = e[0] == 0xE5;
            if ((attr & 0x3F) == 0x0F) {
                Piece p {gone, quint8(e[0] & 0x3F), e[13], QString()};
                for (const int at : {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30}) {
                    const char16_t ch = le16(e + at);
                    if (ch == 0x0000 || ch == 0xFFFF)
                        break;
                    p.chars += QChar(ch);
                }
                pieces.push_back(p);
                continue;
            }
            QVector<Piece> name = std::exchange(pieces, {});
            if (attr & 0xC0)
                continue; // not a folder entry at all
            uchar shortName[11];
            std::memcpy(shortName, e, 11);
            if (attr & 0x08) {
                if (!gone && !(attr & 0x10) && volumeLabel)
                    *volumeLabel = QString::fromLatin1(reinterpret_cast<const char *>(shortName), 11).trimmed();
                continue;
            }
            if (e[0] == '.')
                continue; // "." and ".."
            if (shortName[0] == 0x05)
                shortName[0] = 0xE5; // a name that really starts with that byte

            // The long name: the pieces just before, nearest first, that share one checksum.
            QString longName;
            while (!name.isEmpty() && name.first().checksum != name.last().checksum)
                name.removeFirst();
            if (!name.isEmpty()) {
                const quint8 sum = name.last().checksum;
                bool belongs = false;
                if (gone) {
                    // Deleting overwrote the short name's first letter. Each letter there gives a
                    // different checksum, so the long name's checksum says which it was.
                    for (int c = 0; c < 256 && !belongs; ++c) {
                        shortName[0] = uchar(c);
                        belongs = shortNameChecksum(shortName) == sum && shortNameChar(uchar(c));
                    }
                } else {
                    belongs = shortNameChecksum(shortName) == sum;
                }
                if (belongs) {
                    for (qsizetype k = name.size() - 1; k >= 0; --k)
                        longName += name[k].chars;
                } else if (gone) {
                    shortName[0] = '_';
                }
            }
            Entry entry;
            entry.deleted = gone || allDeleted;
            if (gone && longName.isEmpty()) {
                shortName[0] = '_';
                entry.nameGuessed = true;
            }
            bool junk = false;
            for (int k = 0; k < 11; ++k)
                junk = junk || !shortNameChar(shortName[k]);
            if (junk && entry.deleted)
                continue; // left over from something else
            QString base = QString::fromLatin1(reinterpret_cast<const char *>(shortName), 8).trimmed();
            QString ext = QString::fromLatin1(reinterpret_cast<const char *>(shortName) + 8, 3).trimmed();
            if (e[12] & 0x08)
                base = base.toLower();
            if (e[12] & 0x10)
                ext = ext.toLower();
            entry.name = !longName.isEmpty() ? longName : ext.isEmpty() ? base : base + QLatin1Char('.') + ext;
            entry.isDir = attr & 0x10;
            entry.first = (bits == 32 ? quint32(le16(e + 20)) << 16 : 0) | le16(e + 26);
            entry.size = entry.isDir ? 0 : le32(e + 28);
            entry.modified = fatTime(le16(e + 24), le16(e + 22));
            if (entry.deleted && ((entry.size > 0 && !valid(entry.first)) || entry.size > m_where.length))
                continue;
            out.push_back(entry);
        }
        return out;
    }

private:
    bool parseBoot(const QByteArray &boot)
    {
        if (boot.size() < 512)
            return false;
        const uchar *b = bytes(boot);
        if ((b[0] != 0xEB && b[0] != 0xE9) || le16(b + 510) != 0xAA55)
            return false;
        const quint32 sectorSize = le16(b + 11);
        const quint32 perCluster = b[13];
        const quint32 reserved = le16(b + 14);
        const quint32 fats = b[16];
        const quint32 entries = le16(b + 17);
        const quint64 total = le16(b + 19) ? le16(b + 19) : le32(b + 32);
        const quint64 fatSectors = le16(b + 22) ? le16(b + 22) : le32(b + 36);
        if ((sectorSize != 512 && sectorSize != 1024 && sectorSize != 2048 && sectorSize != 4096) || perCluster == 0
            || (perCluster & (perCluster - 1)) || reserved == 0 || fats == 0 || fats > 4 || total == 0 || fatSectors == 0)
            return false;
        const quint64 rootSectors = (quint64(entries) * 32 + sectorSize - 1) / sectorSize;
        const quint64 firstData = reserved + fats * fatSectors + rootSectors;
        if (firstData >= total)
            return false;
        quint64 clusterCount = (total - firstData) / perCluster;
        bits = clusterCount < 4085 ? 12 : clusterCount < 65525 ? 16 : 32;
        if ((bits == 32) != (entries == 0))
            return false;
        // No more clusters than the table has room for, or than the partition holds.
        clusterCount = std::min(clusterCount, fatSectors * sectorSize * 8 / quint64(bits) - 2);
        clusterSize = quint64(perCluster) * sectorSize;
        heap = firstData * sectorSize;
        if (heap >= m_where.length)
            return false;
        clusterCount = std::min(clusterCount, (m_where.length - heap) / clusterSize);
        if (clusterCount == 0 || clusterCount > 0x0FFFFFF0)
            return false;
        count = quint32(clusterCount);
        fatStart = quint64(reserved) * sectorSize;
        fatBytes = fatSectors * sectorSize;
        rootStart = (reserved + fats * fatSectors) * sectorSize;
        rootEntries = entries;
        rootCluster = bits == 32 ? le32(b + 44) : 0;
        if (bits == 32 && !valid(rootCluster))
            return false;
        label = QString::fromLatin1(reinterpret_cast<const char *>(b) + (bits == 32 ? 71 : 43), 11).trimmed();
        if (label == QLatin1String("NO NAME"))
            label.clear();
        return true;
    }

    QHash<quint64, QByteArray> m_table; // the table, read a piece at a time as it's needed
};

// --- exFAT

// exFAT times: local, with the zone they're in when the 0x80 bit of the offset says so.
qint64 exfatTime(quint32 stamp, quint8 hundredths, quint8 offset)
{
    const QDate d(1980 + int(stamp >> 25), int(stamp >> 21) & 0xF, int(stamp >> 16) & 0x1F);
    if (!d.isValid())
        return 0;
    QTime t(int(stamp >> 11) & 0x1F, int(stamp >> 5) & 0x3F, int(stamp & 0x1F) * 2);
    if (!t.isValid())
        t = QTime(0, 0);
    t = t.addMSecs(std::min(199, int(hundredths)) * 10);
    if (offset & 0x80) {
        const int quarters = (offset & 0x40) ? int(offset & 0x7F) - 128 : int(offset & 0x7F);
        return QDateTime(d, t, QTimeZone::fromSecondsAheadOfUtc(quarters * 15 * 60)).toMSecsSinceEpoch();
    }
    return QDateTime(d, t).toMSecsSinceEpoch();
}

class Exfat : public Clusters
{
public:
    using Clusters::Clusters;

    quint64 fatStart = 0, fatBytes = 0;
    quint32 rootCluster = 0;

    bool open()
    {
        if (parseBoot(readBytes(m_source, m_where.start, 512)))
            return true;
        // The backup boot region starts 12 sectors in.
        for (const quint64 sector : {512, 4096}) {
            if (parseBoot(readBytes(m_source, m_where.start + 12 * sector, 512)))
                return true;
        }
        return false;
    }

    quint32 next(quint32 c) override
    {
        const quint64 byte = quint64(c) * 4;
        if (byte + 4 > fatBytes)
            return 0;
        const quint64 piece = byte / kFatPiece;
        auto it = m_table.find(piece);
        if (it == m_table.end()) {
            if (m_table.size() > 4096)
                m_table.clear();
            it = m_table.insert(piece, readBytes(m_source, m_where.start + fatStart + piece * kFatPiece,
                                                 std::min(kFatPiece, fatBytes - piece * kFatPiece)));
        }
        const quint64 inside = byte - piece * kFatPiece;
        return inside + 4 <= quint64(it->size()) ? le32(bytes(*it) + inside) : 0;
    }

    bool isEnd(quint32 value) const override { return value >= 0xFFFFFFF7 || value < 2; }

    bool inUse(quint32 c) override
    {
        const quint64 bit = c - 2;
        if (!valid(c) || bit / 8 >= quint64(m_bitmap.size()))
            return true; // when in doubt, assume something else has it
        return bytes(m_bitmap)[bit / 8] & (1u << (bit % 8));
    }

    // Reads the allocation bitmap the root folder points to.
    void loadBitmap(quint32 first, quint64 length)
    {
        if (!valid(first) || length > quint64(count) / 8 + clusterSize)
            return;
        const quint32 need = quint32((length + clusterSize - 1) / clusterSize);
        m_bitmap = readClusters(chain(first, need)).left(qsizetype(length));
    }
    bool hasBitmap() const { return !m_bitmap.isEmpty(); }

    // The entries of one exFAT folder. A file is a set: its own entry, a stream entry (size,
    // first cluster) and its name in pieces of 15 letters.
    QVector<Entry> parse(const QByteArray &data, bool allDeleted, bool root)
    {
        QVector<Entry> out;
        for (qsizetype i = 0; i + 32 <= data.size(); i += 32) {
            const uchar *e = bytes(data) + i;
            const uchar type = e[0];
            if (type == 0x00)
                break;
            if (root && type == 0x81 && !hasBitmap())
                loadBitmap(le32(e + 20), le64(e + 24));
            if (root && type == 0x83)
                label = utf16le(e + 2, std::min<int>(e[1], 11));
            if ((type & 0x7F) != 0x05)
                continue;
            const bool gone = !(type & 0x80) || allDeleted;
            const int secondary = e[1];
            if (secondary < 2 || secondary > 18 || i + qsizetype(secondary + 1) * 32 > data.size())
                continue;
            const uchar *stream = e + 32;
            if ((stream[0] & 0x7F) != 0x40)
                continue;
            // The set's checksum covers every entry, with the in-use bits as they were.
            QByteArray set(reinterpret_cast<const char *>(e), qsizetype(secondary + 1) * 32);
            quint16 sum = 0;
            for (qsizetype k = 0; k < set.size(); ++k) {
                if (k == 2 || k == 3)
                    continue;
                uchar b = uchar(set[k]);
                if (k % 32 == 0)
                    b |= 0x80;
                sum = quint16(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + b);
            }
            if (sum != le16(e + 2) && gone)
                continue; // partly reused since: not to be trusted
            Entry entry;
            entry.deleted = gone;
            entry.isDir = le16(e + 4) & 0x10;
            entry.modified = exfatTime(le32(e + 12), e[21], e[23]);
            entry.contiguous = stream[1] & 0x02;
            const int nameLength = stream[3];
            entry.first = le32(stream + 20);
            entry.size = le64(stream + 24);
            for (int k = 2; k <= secondary && entry.name.size() < nameLength; ++k) {
                const uchar *n = e + k * 32;
                if ((n[0] & 0x7F) != 0x41)
                    break;
                entry.name += utf16le(n + 2, 15);
            }
            entry.name.truncate(nameLength);
            i += qsizetype(secondary) * 32;
            if (entry.name.isEmpty() || (entry.size > 0 && !valid(entry.first)) || entry.size > m_where.length)
                continue;
            out.push_back(entry);
        }
        return out;
    }

    // A file's or folder's clusters, from what its entry says.
    QVector<quint32> clustersOf(const Entry &entry, bool *guess, bool *skipped)
    {
        const quint32 need = quint32((entry.size + clusterSize - 1) / clusterSize);
        *guess = *skipped = false;
        if (need == 0 || !valid(entry.first))
            return {};
        if (entry.contiguous) {
            QVector<quint32> out;
            for (quint32 c = entry.first; quint32(out.size()) < need && valid(c); ++c)
                out.push_back(c);
            return out;
        }
        bool ended = false;
        const QVector<quint32> chain = this->chain(entry.first, need, &ended);
        if (!entry.deleted || quint32(chain.size()) == need)
            return chain;
        // A deleted file in pieces whose chain doesn't add up anymore: guessed like on FAT.
        *guess = true;
        return guessRun(entry.first, need, skipped);
    }

private:
    bool parseBoot(const QByteArray &boot)
    {
        if (boot.size() < 512)
            return false;
        const uchar *b = bytes(boot);
        if (std::memcmp(b + 3, "EXFAT   ", 8) != 0)
            return false;
        const int sectorShift = b[108], clusterShift = b[109];
        if (sectorShift < 9 || sectorShift > 12 || clusterShift > 25 - sectorShift)
            return false;
        const quint64 sectorSize = 1ull << sectorShift;
        clusterSize = sectorSize << clusterShift;
        fatStart = quint64(le32(b + 80)) * sectorSize;
        fatBytes = quint64(le32(b + 84)) * sectorSize;
        heap = quint64(le32(b + 88)) * sectorSize;
        quint64 clusterCount = le32(b + 92);
        rootCluster = le32(b + 96);
        // TexFAT's second table, when the flags say that's the one in use.
        if ((le16(b + 106) & 1) && b[110] == 2)
            fatStart += fatBytes;
        if (fatStart == 0 || heap <= fatStart || heap >= m_where.length || clusterCount == 0)
            return false;
        clusterCount = std::min(clusterCount, (m_where.length - heap) / clusterSize);
        clusterCount = std::min(clusterCount, fatBytes / 4 - 2);
        count = quint32(clusterCount);
        return valid(rootCluster);
    }

    QHash<quint64, QByteArray> m_table;
    QByteArray m_bitmap;
};

// --- Walking the folders

struct Folder {
    QString path;
    quint32 first = 0;
    quint64 size = 0;        // exFAT folders say how big they are
    bool contiguous = false; // exFAT
    bool deleted = false;
    int depth = 0;
};

class Walker
{
public:
    Walker(Clusters &fs, const Batch &out, const std::atomic<bool> &cancel, Volume *volume)
        : m_fs(fs)
        , m_out(out)
        , m_cancel(cancel)
        , m_volume(volume)
    {
    }

    // A file from its entry, with its pieces: exact for what the file system still uses, and
    // for exFAT files in one piece; guessed for other deleted ones.
    void addFile(const Entry &entry, const QString &folder, const QVector<quint32> &clusters, bool guessed)
    {
        Found f;
        f.name = entry.name;
        f.folder = folder;
        f.origin = Origin::FileSystem;
        f.deleted = entry.deleted;
        f.nameGuessed = entry.nameGuessed;
        f.piecesGuessed = guessed;
        f.size = entry.size;
        f.modified = entry.modified;
        f.type = filetypes::typeForName(entry.name);
        f.extents = m_fs.extentsOf(clusters, entry.size);
        quint64 covered = 0;
        for (const Extent &e : std::as_const(f.extents))
            covered += e.length;
        if (covered < entry.size)
            f.condition = Condition::MaybeDamaged; // its chain breaks off
        // Deleted, and part of its space is in use again: something else has been written there.
        if (entry.deleted) {
            for (qsizetype i = 0; i < std::min<qsizetype>(clusters.size(), 100000); ++i) {
                if (m_fs.inUse(clusters[i])) {
                    f.condition = Condition::Overwritten;
                    break;
                }
            }
        }
        m_batch.push_back(std::move(f));
        ++m_listed;
        if (m_volume) {
            ++m_volume->files;
            if (entry.deleted)
                ++m_volume->deleted;
        }
        if (m_batch.size() >= kBatch)
            flush();
    }

    void flush()
    {
        if (!m_batch.isEmpty())
            m_out(std::exchange(m_batch, {}));
    }

    bool stop() const { return m_cancel || m_listed >= kMaxListed; }

    QSet<quint32> visited;

private:
    Clusters &m_fs;
    const Batch &m_out;
    const std::atomic<bool> &m_cancel;
    Volume *m_volume;
    QVector<Found> m_batch;
    int m_listed = 0;
};

QString rootName(const QString &label, const QString &type, const Extent &where)
{
    return !label.isEmpty() ? label : tr("%1 partition at %2").arg(type, formatSize(where.start));
}

bool walkFat(Source &source, const Extent &where, const Batch &out, const std::atomic<bool> &cancel, Volume *volume)
{
    Fat fat(source, where);
    if (!fat.open())
        return false;
    Walker walker(fat, out, cancel, volume);
    // The root folder: a fixed place on FAT12/16, a chain on FAT32. Its label entry wins over
    // the one in the boot sector (that's the one Windows changes).
    QByteArray root = fat.bits == 32 ? fat.readClusters(fat.chain(fat.rootCluster, fat.clustersFor(kMaxFolderBytes)))
                                     : readBytes(source, where.start + fat.rootStart, quint64(fat.rootEntries) * 32);
    QString label = fat.label;
    const QVector<Entry> rootEntries = fat.parse(root, false, &label);
    const QString top = rootName(label, fat.typeName(), where);
    if (volume) {
        volume->type = QStringLiteral("vfat");
        volume->label = top;
        volume->where = where;
    }

    QVector<std::pair<Folder, QVector<Entry>>> todo = {{Folder{top, fat.rootCluster, 0, false, false, 0}, rootEntries}};
    while (!todo.isEmpty() && !walker.stop()) {
        auto [folder, entries] = todo.takeLast();
        for (const Entry &entry : std::as_const(entries)) {
            if (walker.stop())
                break;
            if (entry.isDir) {
                if (!fat.valid(entry.first) || walker.visited.contains(entry.first) || folder.depth >= kMaxDepth)
                    continue;
                walker.visited.insert(entry.first);
                QByteArray data;
                if (!entry.deleted) {
                    data = fat.readClusters(fat.chain(entry.first, fat.clustersFor(kMaxFolderBytes)));
                } else {
                    // A deleted folder's chain is gone. Its first cluster has to be free still and
                    // start with "." and ".." to be trusted; the clusters after it count for as
                    // long as they're free and look like folder entries.
                    if (fat.inUse(entry.first))
                        continue;
                    const QByteArray first = fat.readClusters({entry.first});
                    if (first.size() < 64 || std::memcmp(first.constData(), ".          ", 11) != 0
                        || std::memcmp(first.constData() + 32, "..         ", 11) != 0)
                        continue;
                    data = first;
                    auto ended = [](const QByteArray &d) {
                        for (qsizetype k = 0; k + 32 <= d.size(); k += 32) {
                            if (d[k] == 0)
                                return true;
                        }
                        return false;
                    };
                    for (quint32 c = entry.first + 1; !ended(data) && fat.valid(c) && c < entry.first + kMaxDeletedFolderClusters; ++c) {
                        if (fat.inUse(c))
                            break;
                        const QByteArray more = fat.readClusters({c});
                        if (more.size() < 32 || more[0] == 0 || (uchar(more[11]) & 0xC0))
                            break;
                        data += more;
                    }
                }
                todo.push_back({Folder{folder.path + QLatin1Char('/') + lost::cleanName(entry.name), entry.first, 0, false,
                                       entry.deleted || folder.deleted, folder.depth + 1},
                                fat.parse(data, entry.deleted || folder.deleted)});
                continue;
            }
            Entry file = entry;
            file.deleted = entry.deleted || folder.deleted;
            const quint32 need = quint32((file.size + fat.clusterSize - 1) / fat.clusterSize);
            if (need == 0) {
                walker.addFile(file, folder.path, {}, false);
                continue;
            }
            if (!file.deleted) {
                walker.addFile(file, folder.path, fat.chain(file.first, need), false);
                continue;
            }
            // FAT32 can lose the top half of a deleted file's first cluster number. When the
            // file doesn't start the way its name says, the other halves are tried.
            quint32 first = file.first;
            const int type = filetypes::typeForName(file.name);
            if (fat.bits == 32 && (first >> 16) == 0 && type >= 0 && fat.count > 0xFFFF) {
                auto startsRight = [&](quint32 c) {
                    if (!fat.valid(c) || fat.inUse(c))
                        return false;
                    const QByteArray head = readBytes(source, fat.offsetOf(c), 512);
                    return head.size() >= 32
                        && filetypes::candidates(bytes(head), int(head.size())).contains(filetypes::startType(type));
                };
                if (!startsRight(first)) {
                    for (quint32 high = 1; high <= std::min<quint32>(fat.count >> 16, 64); ++high) {
                        if (startsRight(high << 16 | first)) {
                            first = high << 16 | first;
                            break;
                        }
                    }
                }
            }
            file.first = first;
            bool skipped = false;
            const QVector<quint32> clusters = fat.guessRun(first, need, &skipped);
            walker.addFile(file, folder.path, clusters, need > 1);
        }
    }
    walker.flush();
    return true;
}

bool walkExfat(Source &source, const Extent &where, const Batch &out, const std::atomic<bool> &cancel, Volume *volume)
{
    Exfat fs(source, where);
    if (!fs.open())
        return false;
    Walker walker(fs, out, cancel, volume);
    const QVector<Entry> rootEntries = fs.parse(fs.readClusters(fs.chain(fs.rootCluster, fs.clustersFor(kMaxExfatFolderBytes))), false, true);
    const QString top = rootName(fs.label, QStringLiteral("exFAT"), where);
    if (volume) {
        volume->type = QStringLiteral("exfat");
        volume->label = top;
        volume->where = where;
    }
    QVector<std::pair<Folder, QVector<Entry>>> todo = {{Folder{top, fs.rootCluster, 0, false, false, 0}, rootEntries}};
    while (!todo.isEmpty() && !walker.stop()) {
        auto [folder, entries] = todo.takeLast();
        for (const Entry &entry : std::as_const(entries)) {
            if (walker.stop())
                break;
            Entry item = entry;
            item.deleted = entry.deleted || folder.deleted;
            bool guessed = false, skipped = false;
            const QVector<quint32> clusters = fs.clustersOf(item, &guessed, &skipped);
            if (item.isDir) {
                if (clusters.isEmpty() || walker.visited.contains(item.first) || folder.depth >= kMaxDepth)
                    continue;
                // A deleted folder that's been partly written over isn't to be trusted.
                if (item.deleted && fs.inUse(clusters.first()))
                    continue;
                walker.visited.insert(item.first);
                const QByteArray data = fs.readClusters(clusters).left(qsizetype(item.size));
                todo.push_back({Folder{folder.path + QLatin1Char('/') + lost::cleanName(item.name), item.first, item.size,
                                       item.contiguous, item.deleted, folder.depth + 1},
                                fs.parse(data, item.deleted, false)});
                continue;
            }
            walker.addFile(item, folder.path, clusters, guessed || (item.deleted && !item.contiguous && clusters.size() > 1));
        }
    }
    walker.flush();
    return true;
}

} // namespace

QVector<Extent> partitionsOf(Source &source)
{
    QVector<Extent> out = {{0, source.size()}};
    const QByteArray mbr = readBytes(source, 0, 512);
    if (mbr.size() < 512 || le16(bytes(mbr) + 510) != 0xAA55)
        return out;
    const uchar *m = bytes(mbr);
    // A file system's own boot sector ends the same way, and has code where a table would be.
    if (std::memcmp(m + 3, "NTFS    ", 8) == 0 || std::memcmp(m + 3, "EXFAT   ", 8) == 0 || std::memcmp(m + 82, "FAT32   ", 8) == 0
        || std::memcmp(m + 54, "FAT1", 4) == 0)
        return out;
    auto add = [&](quint64 start, quint64 length) {
        if (length > 0 && start < source.size() && start != 0)
            out.push_back({start, std::min(length, source.size() - start)});
    };
    if (m[446 + 4] == 0xEE) {
        for (const quint64 sector : {512, 4096}) {
            const QByteArray header = readBytes(source, sector, 512);
            if (header.size() < 92 || std::memcmp(header.constData(), "EFI PART", 8) != 0)
                continue;
            const uchar *h = bytes(header);
            const quint64 listAt = le64(h + 72) * sector;
            const quint32 slotCount = std::min<quint32>(le32(h + 80), 1024);
            const quint32 slotSize = le32(h + 84);
            if (slotSize < 128 || slotSize > 4096 || (slotSize & (slotSize - 1)))
                continue;
            const QByteArray list = readBytes(source, listAt, quint64(slotCount) * slotSize);
            for (quint32 i = 0; i < slotCount && qsizetype(i + 1) * slotSize <= list.size(); ++i) {
                const uchar *e = bytes(list) + qsizetype(i) * slotSize;
                if (le64(e) == 0 && le64(e + 8) == 0)
                    continue; // an empty slot
                const quint64 first = le64(e + 32), last = le64(e + 40);
                if (last >= first)
                    add(first * sector, (last - first + 1) * sector);
            }
            return out;
        }
        return out;
    }
    // MBR: four entries, and the logical partitions in a chain inside an extended one.
    quint64 extended = 0;
    for (int i = 0; i < 4; ++i) {
        const uchar *e = m + 446 + i * 16;
        const quint8 type = e[4];
        const quint64 start = quint64(le32(e + 8)) * 512, length = quint64(le32(e + 12)) * 512;
        if (type == 0)
            continue;
        if (type == 0x05 || type == 0x0F || type == 0x85)
            extended = extended ? extended : start;
        else
            add(start, length);
    }
    quint64 ebr = extended;
    for (int n = 0; ebr && n < 128; ++n) {
        const QByteArray sector = readBytes(source, ebr, 512);
        if (sector.size() < 512 || le16(bytes(sector) + 510) != 0xAA55)
            break;
        const uchar *s = bytes(sector);
        if (s[446 + 4] != 0)
            add(ebr + quint64(le32(s + 446 + 8)) * 512, quint64(le32(s + 446 + 12)) * 512);
        const quint8 nextType = s[462 + 4];
        const quint64 next = quint64(le32(s + 462 + 8)) * 512;
        ebr = (nextType == 0x05 || nextType == 0x0F || nextType == 0x85) && next ? extended + next : 0;
    }
    return out;
}

bool readFileSystem(Source &source, const Extent &where, const Batch &out, const std::atomic<bool> &cancel, Volume *volume)
{
    return walkExfat(source, where, out, cancel, volume) || readNtfs(source, where, out, cancel, volume)
        || walkFat(source, where, out, cancel, volume);
}

void checkContents(const std::shared_ptr<Source> &source, Found &file, bool full)
{
    if (!file.deleted || file.condition == Condition::Overwritten || file.size == 0)
        return;
    const int type = file.type >= 0 ? file.type : filetypes::typeForName(file.name);
    if (type < 0)
        return;
    FileReader reader(source, file);
    const QByteArray head = reader.head(4096);
    if (head.count('\0') == head.size()) {
        // Nothing but zeros: the drive has emptied that space (SSDs do, soon after a delete).
        file.condition = Condition::Overwritten;
        file.checked = true;
        return;
    }
    const int start = filetypes::startType(type);
    if (!filetypes::candidates(bytes(head), int(head.size())).contains(start)) {
        if (filetypes::alwaysRecognised(type)) {
            file.condition = Condition::Overwritten;
            file.checked = true;
        }
        return;
    }
    if (!full || file.size > kFullCheckLimit)
        return;
    const filetypes::Measured m = filetypes::measure(start, [&reader](quint64 at, char *buf, qint64 len) {
        return reader.read(at, buf, len);
    });
    file.checked = true;
    if (m.size == 0 || !m.complete)
        file.condition = Condition::MaybeDamaged;
    if (source->unreadable(0, source->size()) && file.condition == Condition::Good) {
        for (const Extent &e : std::as_const(file.extents)) {
            if (!e.zeros() && source->unreadable(e.start, e.length))
                file.condition = Condition::Unreadable;
        }
    }
}

} // namespace lost
