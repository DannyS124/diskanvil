// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fragments.h"

#include "applog.h"
#include "filetypes.h"
#include "lostraw.h"

#include <QCoreApplication>

#include <algorithm>
#include <array>
#include <cstring>

namespace lost::fragments {

namespace {

// How far from the file's start its other pieces are looked for, and how many there can be.
constexpr quint64 kBehind = 1ull << 30;
constexpr quint64 kAhead = 8ull << 30;
constexpr int kMostPieces = 64;
constexpr quint64 kSearchChunk = 8 * 1024 * 1024;

quint16 be16(const uchar *p)
{
    return quint16(p[0] << 8 | p[1]);
}

quint32 be32(const uchar *p)
{
    return quint32(p[0]) << 24 | quint32(p[1]) << 16 | quint32(p[2]) << 8 | p[3];
}

quint64 be64(const uchar *p)
{
    return quint64(be32(p)) << 32 | be32(p + 4);
}

const uchar *bytes(const QByteArray &data)
{
    return reinterpret_cast<const uchar *>(data.constData());
}

// Where a file's clusters are: logical cluster k is at map[k] (~0 while not known).
struct Pieces {
    quint64 start = 0; // where the file starts on the source (a cluster's start)
    quint64 cluster = 4096;
    QVector<quint64> map;
    int count = 1;

    // Every cluster position on the source lines up with the file's start.
    bool onGrid(quint64 at) const { return at >= start % cluster && (at - start % cluster) % cluster == 0; }

    QVector<Extent> extents(quint64 size) const
    {
        QVector<Extent> out;
        for (qsizetype k = 0; k < map.size(); ++k) {
            const quint64 length = std::min<quint64>(cluster, size - quint64(k) * cluster);
            if (!out.isEmpty() && out.last().end() == map[k])
                out.last().length += length;
            else
                out.push_back({map[k], length});
        }
        return out;
    }
};

// Whether the cluster at `at` is clearly not part of the file being put together: another file
// found whole has it, it's blank, it's the file system's own bookkeeping (NTFS's records and
// folder indexes), or another file starts there.
bool foreign(Context &c, quint64 at)
{
    if (c.taken && c.taken(at))
        return true;
    const QByteArray data = raw::read(c.source, at, c.clusterSize);
    if (data.size() < 512)
        return true;
    if (std::all_of(data.cbegin(), data.cend(), [](char ch) { return ch == 0; }))
        return true;
    for (const char *mark : {"FILE", "INDX", "RCRD", "RSTR"}) {
        if (memcmp(data.constData(), mark, 4) == 0)
            return true;
    }
    return !filetypes::candidates(bytes(data), 512).isEmpty();
}

// Clusters on the grid, from `from` to `to`, in big reads; `test` says whether one is the place
// (it gets the cluster's bytes). Taken and already used clusters are passed over. The first
// that passes, or ~0.
quint64 searchGrid(Context &c, const Pieces &p, quint64 from, quint64 to, const QSet<quint64> &used,
                   const std::function<bool(quint64 at, const uchar *data, qsizetype len)> &test)
{
    const quint64 cs = p.cluster;
    from = from / cs * cs + p.start % cs;
    to = std::min(to, c.source.size());
    for (quint64 at = from; at + cs <= to && !c.cancel; at += kSearchChunk / cs * cs) {
        const QByteArray chunk = raw::read(c.source, at, std::min<quint64>(kSearchChunk, to - at));
        for (qsizetype off = 0; off + qsizetype(cs) <= chunk.size(); off += qsizetype(cs)) {
            const quint64 here = at + quint64(off);
            if (used.contains(here) || (c.taken && c.taken(here)))
                continue;
            if (test(here, bytes(chunk) + off, chunk.size() - off))
                return here;
        }
    }
    return ~quint64(0);
}

// --- Films (MP4, MOV and their family)

struct Box {
    qsizetype start = -1, body = 0, end = 0;
    bool found() const { return start >= 0; }
};

// The first box called `name` among those from `from` to `to` in `data`.
Box child(const QByteArray &data, qsizetype from, qsizetype to, const char *name)
{
    const uchar *d = bytes(data);
    for (qsizetype pos = from; pos + 8 <= to;) {
        quint64 size = be32(d + pos);
        qsizetype header = 8;
        if (size == 1) {
            if (pos + 16 > to)
                return {};
            size = be64(d + pos + 8);
            header = 16;
        } else if (size == 0) {
            size = quint64(to - pos);
        }
        if (size < quint64(header) || size > quint64(to - pos))
            return {};
        if (memcmp(d + pos + 4, name, 4) == 0)
            return {pos, pos + header, pos + qsizetype(size)};
        pos += qsizetype(size);
    }
    return {};
}

QVector<Box> children(const QByteArray &data, qsizetype from, qsizetype to, const char *name)
{
    QVector<Box> out;
    for (qsizetype pos = from; pos + 8 <= to;) {
        const Box b = child(data, pos, to, name);
        if (!b.found())
            break;
        out << b;
        pos = b.end;
    }
    return out;
}

struct Track {
    enum class Codec { Avc, Hevc } codec = Codec::Avc;
    int lengthSize = 4;
};

struct Frame {
    quint64 at;   // where in the file
    quint32 size;
    int track;
};

// What each frame of a checkable track (H.264, H.265) starts with: the length of its first
// piece, then that piece's header byte(s).
bool frameStarts(const Track &t, const uchar *p, quint32 size)
{
    quint32 length = 0;
    for (int i = 0; i < t.lengthSize; ++i)
        length = length << 8 | p[i];
    if (length == 0 || quint64(length) + quint64(t.lengthSize) > size)
        return false;
    const uchar h = p[t.lengthSize];
    if (t.codec == Track::Codec::Avc) {
        const int type = h & 0x1F;
        return !(h & 0x80) && type >= 1 && type <= 23;
    }
    const uchar h2 = p[t.lengthSize + 1];
    const int type = (h >> 1) & 0x3F;
    return !(h & 0x80) && !(h & 1) && type <= 40 && (h2 & 7) != 0;
}

// The checkable frames of a film, from its index (the "moov" box, whole).
bool readIndex(const QByteArray &moov, QVector<Track> *tracks, QVector<Frame> *frames)
{
    const Box top = child(moov, 0, moov.size(), "moov");
    if (!top.found())
        return false;
    for (const Box &trak : children(moov, top.body, top.end, "trak")) {
        const Box mdia = child(moov, trak.body, trak.end, "mdia");
        const Box minf = mdia.found() ? child(moov, mdia.body, mdia.end, "minf") : Box();
        const Box stbl = minf.found() ? child(moov, minf.body, minf.end, "stbl") : Box();
        const Box stsd = stbl.found() ? child(moov, stbl.body, stbl.end, "stsd") : Box();
        if (!stsd.found() || stsd.body + 16 > stsd.end)
            continue;
        const uchar *d = bytes(moov);
        const qsizetype entry = stsd.body + 8;
        const QByteArray codec(reinterpret_cast<const char *>(d + entry + 4), 4);
        Track t;
        Box config;
        // A video sample entry's own fields take 78 bytes before its boxes.
        if (codec == "avc1" || codec == "avc3") {
            config = child(moov, entry + 86, std::min<qsizetype>(entry + be32(d + entry), stsd.end), "avcC");
            if (!config.found() || config.body + 5 > config.end)
                continue;
            t.lengthSize = (d[config.body + 4] & 3) + 1;
        } else if (codec == "hvc1" || codec == "hev1") {
            t.codec = Track::Codec::Hevc;
            config = child(moov, entry + 86, std::min<qsizetype>(entry + be32(d + entry), stsd.end), "hvcC");
            if (!config.found() || config.body + 22 > config.end)
                continue;
            t.lengthSize = (d[config.body + 21] & 3) + 1;
        } else {
            continue; // sound and other kinds: nothing in their frames to check
        }
        if (t.lengthSize == 3)
            continue;
        const Box stsz = child(moov, stbl.body, stbl.end, "stsz");
        const Box stsc = child(moov, stbl.body, stbl.end, "stsc");
        Box stco = child(moov, stbl.body, stbl.end, "stco");
        const bool wide = !stco.found();
        if (wide)
            stco = child(moov, stbl.body, stbl.end, "co64");
        if (!stsz.found() || !stsc.found() || !stco.found() || stsz.body + 12 > stsz.end || stsc.body + 8 > stsc.end || stco.body + 8 > stco.end)
            continue;
        const quint32 fixed = be32(d + stsz.body + 4), count = be32(d + stsz.body + 8);
        const quint32 runs = be32(d + stsc.body + 4), chunks = be32(d + stco.body + 4);
        if ((!fixed && stsz.body + 12 + qsizetype(count) * 4 > stsz.end) || stsc.body + 8 + qsizetype(runs) * 12 > stsc.end
            || stco.body + 8 + qsizetype(chunks) * (wide ? 8 : 4) > stco.end || count > 20000000)
            continue;
        const int number = int(tracks->size());
        *tracks << t;
        quint32 sample = 0;
        for (quint32 k = 0; k < chunks && sample < count; ++k) {
            quint32 perChunk = 0;
            for (quint32 r = 0; r < runs; ++r) {
                if (be32(d + stsc.body + 8 + r * 12) <= k + 1)
                    perChunk = be32(d + stsc.body + 8 + r * 12 + 4);
            }
            quint64 at = wide ? be64(d + stco.body + 8 + qsizetype(k) * 8) : be32(d + stco.body + 8 + qsizetype(k) * 4);
            for (quint32 i = 0; i < perChunk && sample < count; ++i, ++sample) {
                const quint32 size = fixed ? fixed : be32(d + stsz.body + 12 + qsizetype(sample) * 4);
                *frames << Frame{at, size, number};
                at += size;
            }
        }
    }
    std::sort(frames->begin(), frames->end(), [](const Frame &a, const Frame &b) { return a.at < b.at; });
    return !frames->isEmpty();
}

enum class Fits { Yes, No, CantTell };

// Whether the frames that start in logical cluster k start right when it's at `data`.
Fits clusterFits(const QVector<Track> &tracks, const QVector<Frame> &frames, quint64 k, quint64 cs, const uchar *data, qsizetype len)
{
    const quint64 from = k * cs;
    auto it = std::lower_bound(frames.cbegin(), frames.cend(), from, [](const Frame &f, quint64 at) { return f.at < at; });
    int checked = 0;
    for (; it != frames.cend() && it->at < from + cs; ++it) {
        const qsizetype off = qsizetype(it->at - from);
        const Track &t = tracks[it->track];
        if (off + t.lengthSize + 2 > len || off + t.lengthSize + 2 > qsizetype(cs))
            continue;
        if (!frameStarts(t, data + off, it->size))
            return Fits::No;
        ++checked;
    }
    return checked ? Fits::Yes : Fits::CantTell;
}

} // namespace

void noteIndexes(const uchar *data, qsizetype len, quint64 at, QVector<quint64> *out)
{
    // "moov", with its size before it and its first box ("mvhd") right after.
    for (qsizetype i = 4; i + 12 <= len; ++i) {
        const void *hit = memchr(data + i, 'm', size_t(len - 12 - i + 1));
        if (!hit)
            break;
        i = reinterpret_cast<const uchar *>(hit) - data;
        if (i >= 4 && i + 12 <= len && memcmp(data + i, "moov", 4) == 0 && memcmp(data + i + 8, "mvhd", 4) == 0 && be32(data + i - 4) >= 16
            && out->size() < 100000)
            *out << at + quint64(i - 4);
    }
}

bool mp4(Context &c, Found &file)
{
    if (file.extents.size() != 1)
        return false;
    const quint64 cs = c.clusterSize;
    Pieces p;
    p.start = file.extents.first().start;
    p.cluster = cs;
    // The boxes at the top, as far as they run on from the start.
    quint64 pos = 0, mdatFrom = 0, mdatTo = 0, moovAt = ~quint64(0), moovSize = 0;
    for (int boxes = 0; boxes < 64; ++boxes) {
        const QByteArray h = raw::read(c.source, p.start + pos, 16);
        if (h.size() < 16)
            break;
        quint64 size = be32(bytes(h));
        const bool long64 = size == 1;
        if (long64)
            size = be64(bytes(h) + 8);
        const QByteArray name = h.mid(4, 4);
        if (size < 8 || !std::all_of(name.cbegin(), name.cend(), [](char ch) { return ch >= 0x20 && ch < 0x7F; }))
            break;
        if (name == "mdat") {
            mdatFrom = pos;
            mdatTo = pos + size;
        } else if (name == "moov") {
            moovAt = p.start + pos;
            moovSize = size;
        } else if (boxes == 0 && name != "ftyp") {
            return false;
        }
        pos += size;
        if (name == "mdat" && moovAt == ~quint64(0))
            break; // the index comes after the frames, wherever it is
    }
    if (mdatTo == 0)
        return false;
    // The index: in the first piece, or where a "moov" lines up with where it should start.
    quint64 indexLogical = moovAt != ~quint64(0) ? moovAt - p.start : mdatTo;
    QByteArray moov;
    QVector<Track> tracks;
    QVector<Frame> frames;
    const auto tryIndex = [&](quint64 at) {
        const QByteArray h = raw::read(c.source, at, 8);
        if (h.size() < 8)
            return false;
        const quint64 size = be32(bytes(h));
        if (size < 16 || size > 512ull * 1024 * 1024)
            return false;
        QByteArray box = raw::read(c.source, at, size);
        QVector<Track> t;
        QVector<Frame> f;
        if (quint64(box.size()) != size || !readIndex(box, &t, &f))
            return false;
        // Its frames have to be inside this film's frame box, and the first ones where the film
        // starts (so it's this film's index, not another's laid out the same way).
        if (f.first().at < mdatFrom || f.last().at + f.last().size > mdatTo)
            return false;
        // ...and reach (nearly) to its end: a shorter film of the same kind starts the same way.
        if (f.last().at + f.last().size < mdatTo - std::min<quint64>((mdatTo - mdatFrom) / 10, 4 * 1024 * 1024))
            return false;
        const quint64 k0 = f.first().at / cs;
        const QByteArray first = raw::read(c.source, p.start + k0 * cs, cs);
        if (first.size() != qsizetype(cs) || clusterFits(t, f, k0, cs, bytes(first), first.size()) != Fits::Yes)
            return false;
        moov = std::move(box);
        tracks = std::move(t);
        frames = std::move(f);
        moovSize = size;
        return true;
    };
    bool indexElsewhere = false;
    if (moovAt != ~quint64(0)) {
        if (!tryIndex(moovAt))
            return false;
    } else {
        // Where it should be first, then anywhere it lines up with the clusters the same way.
        QVector<quint64> places = {p.start + indexLogical};
        for (quint64 at : std::as_const(c.indexes)) {
            if (at % cs == (p.start + indexLogical) % cs && at != p.start + indexLogical)
                places << at;
        }
        for (quint64 at : std::as_const(places)) {
            if (c.cancel || !tryIndex(at))
                continue;
            moovAt = at;
            indexElsewhere = at != p.start + indexLogical;
            break;
        }
        if (moov.isEmpty())
            return false;
    }
    const quint64 size = std::max(mdatTo, indexLogical + moovSize);
    const quint64 clusters = (size + cs - 1) / cs;
    p.map.fill(~quint64(0), qsizetype(clusters));
    // The index's own clusters, when it's in a piece of its own (it's found whole there).
    if (indexElsewhere) {
        const quint64 first = indexLogical / cs, physical = moovAt - indexLogical % cs;
        for (quint64 k = first; k < clusters; ++k)
            p.map[qsizetype(k)] = physical + (k - first) * cs;
        ++p.count;
    }

    // The frames, cluster by cluster: where they stop being right, the piece that carries on.
    QSet<quint64> used;
    qint64 base = qint64(p.start); // where logical cluster 0 would be, for the piece being followed
    qint64 lastGood = -1;
    for (quint64 k = 0; k < clusters && !c.cancel; ++k) {
        if (p.map[qsizetype(k)] != ~quint64(0)) {
            used.insert(p.map[qsizetype(k)]);
            continue;
        }
        const qint64 at = base + qint64(k * cs);
        const QByteArray data = at >= 0 ? raw::read(c.source, quint64(at), cs) : QByteArray();
        const Fits fits = data.size() == qsizetype(cs) ? clusterFits(tracks, frames, k, cs, bytes(data), data.size()) : Fits::No;
        if (fits != Fits::No) {
            p.map[qsizetype(k)] = quint64(at);
            used.insert(quint64(at));
            if (fits == Fits::Yes)
                lastGood = qint64(k);
            continue;
        }
        if (++p.count > kMostPieces)
            return false;
        // Where frame k's cluster really is: its frames start right there, and in the next
        // clusters that have frames.
        const auto there = [&](quint64 place, const uchar *d, qsizetype len) {
            if (clusterFits(tracks, frames, k, cs, d, len) != Fits::Yes)
                return false;
            int more = 0;
            for (quint64 n = 1; n < 64 && k + n < clusters && more < 2; ++n) {
                const QByteArray next = raw::read(c.source, place + n * cs, cs);
                const Fits f = next.size() == qsizetype(cs) ? clusterFits(tracks, frames, k + n, cs, bytes(next), next.size()) : Fits::No;
                if (f == Fits::No)
                    return false;
                more += f == Fits::Yes;
            }
            return true;
        };
        const quint64 near = quint64(std::max<qint64>(at, 0));
        quint64 found = searchGrid(c, p, near, near + 512ull * 1024 * 1024, used, there);
        if (found == ~quint64(0))
            found = searchGrid(c, p, p.start > kBehind ? p.start - kBehind : 0, p.start + kAhead, used, there);
        if (found == ~quint64(0))
            return false;
        // The new piece starts somewhere after the last cluster known right: as far back as the
        // old piece's clusters are clearly something else's.
        quint64 from = k;
        while (qint64(from) - 1 > lastGood) {
            const qint64 old = base + qint64((from - 1) * cs);
            if (old < 0 || !foreign(c, quint64(old)))
                break;
            --from;
        }
        for (quint64 n = from; n < k; ++n) {
            used.remove(p.map[qsizetype(n)]);
            p.map[qsizetype(n)] = found - (k - n) * cs;
            used.insert(p.map[qsizetype(n)]);
        }
        base = qint64(found) - qint64(k * cs);
        p.map[qsizetype(k)] = found;
        used.insert(found);
        lastGood = qint64(k);
    }
    if (c.cancel || std::any_of(p.map.cbegin(), p.map.cend(), [](quint64 m) { return m == ~quint64(0); }))
        return false;
    if (p.count == 1 && file.size == size && file.condition == Condition::Good)
        return false; // whole already, in one piece
    file.extents = p.extents(size);
    file.size = size;
    file.condition = Condition::Good;
    file.checked = true;
    if (p.count > 1)
        file.note = QCoreApplication::translate("lost::fragments", "Put back together from %n piece(s) on the drive.", nullptr, p.count);
    qCInfo(lcOps).noquote() << "Find Lost Files: a film at" << p.start << "put together from" << p.count << "pieces";
    return true;
}

// --- JPEG photos

namespace {

struct Huffman {
    std::array<int, 18> maxCode{};
    std::array<int, 17> valPtr{};
    std::array<int, 17> minCode{};
    QByteArray values;
    bool present = false;
};

struct JpegHeader {
    struct Component {
        int id = 0, h = 1, v = 1, dc = 0, ac = 0;
    };
    QVector<Component> components; // in the scan's order
    std::array<Huffman, 4> dc, ac;
    int mcusX = 0, mcusY = 0, restart = 0;
    quint64 entropy = 0; // where the picture data starts
};

bool buildTable(const uchar *counts, const uchar *values, int total, Huffman *t)
{
    t->values = QByteArray(reinterpret_cast<const char *>(values), total);
    int code = 0, k = 0;
    for (int length = 1; length <= 16; ++length) {
        t->valPtr[length] = k;
        t->minCode[length] = code;
        code += counts[length - 1];
        k += counts[length - 1];
        t->maxCode[length] = counts[length - 1] ? code - 1 : -1;
        code <<= 1;
    }
    t->maxCode[17] = 0x7FFFFFFF;
    t->present = true;
    return k == total;
}

// The headers of a baseline JPEG, up to its picture data. Progressive ones aren't taken.
bool readHeader(const QByteArray &d, JpegHeader *h)
{
    const uchar *p = bytes(d);
    qsizetype pos = 2;
    QVector<JpegHeader::Component> frame;
    int width = 0, height = 0;
    while (pos + 4 <= d.size()) {
        if (p[pos] != 0xFF)
            return false;
        const uchar marker = p[pos + 1];
        if (marker == 0xFF) {
            ++pos;
            continue;
        }
        const int length = be16(p + pos + 2);
        if (length < 2 || pos + 2 + length > d.size())
            return false;
        const uchar *s = p + pos + 4;
        if (marker == 0xC0 || marker == 0xC1) {
            if (length < 8 || s[0] != 8)
                return false;
            height = be16(s + 1);
            width = be16(s + 3);
            const int n = s[5];
            if (n < 1 || n > 4 || length < 8 + 3 * n)
                return false;
            for (int i = 0; i < n; ++i)
                frame << JpegHeader::Component{s[6 + 3 * i], s[7 + 3 * i] >> 4, s[7 + 3 * i] & 15, 0, 0};
        } else if (marker >= 0xC2 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            return false; // progressive, lossless or arithmetic: not read here
        } else if (marker == 0xC4) {
            qsizetype at = 0;
            while (at + 17 <= length - 2) {
                const int cls = s[at] >> 4, id = s[at] & 15;
                if (cls > 1 || id > 3)
                    return false;
                int total = 0;
                for (int i = 0; i < 16; ++i)
                    total += s[at + 1 + i];
                if (at + 17 + total > length - 2 || total > 256)
                    return false;
                if (!buildTable(s + at + 1, s + at + 17, total, cls ? &h->ac[size_t(id)] : &h->dc[size_t(id)]))
                    return false;
                at += 17 + total;
            }
        } else if (marker == 0xDD && length >= 4) {
            h->restart = be16(s);
        } else if (marker == 0xDA) {
            const int n = s[0];
            if (frame.isEmpty() || n < 1 || n > 4 || length < 6 + 2 * n)
                return false;
            int hMax = 1, vMax = 1;
            for (const auto &f : std::as_const(frame)) {
                hMax = std::max(hMax, f.h);
                vMax = std::max(vMax, f.v);
            }
            for (int i = 0; i < n; ++i) {
                auto it = std::find_if(frame.cbegin(), frame.cend(), [&](const JpegHeader::Component &f) { return f.id == s[1 + 2 * i]; });
                if (it == frame.cend())
                    return false;
                JpegHeader::Component comp = *it;
                comp.dc = s[2 + 2 * i] >> 4;
                comp.ac = s[2 + 2 * i] & 15;
                if (comp.dc > 3 || comp.ac > 3 || !h->dc[size_t(comp.dc)].present || !h->ac[size_t(comp.ac)].present)
                    return false;
                if (n == 1) // one component on its own isn't interleaved: one block per unit
                    comp.h = comp.v = 1;
                h->components << comp;
            }
            if (n == 1) {
                const auto &f = *std::find_if(frame.cbegin(), frame.cend(), [&](const JpegHeader::Component &x) { return x.id == s[1]; });
                h->mcusX = (width * f.h / hMax + 7) / 8;
                h->mcusY = (height * f.v / vMax + 7) / 8;
            } else {
                h->mcusX = (width + 8 * hMax - 1) / (8 * hMax);
                h->mcusY = (height + 8 * vMax - 1) / (8 * vMax);
            }
            h->entropy = quint64(pos + 2 + length);
            return h->mcusX > 0 && h->mcusY > 0 && h->mcusX * qint64(h->mcusY) < 50000000;
        }
        pos += 2 + length;
    }
    return false;
}

// Decodes a JPEG's picture data (no picture made: only that it's all valid), through a reader
// that maps the file's clusters to where they are. Its state can be kept at any point and
// carried on from, to try where the next piece is.
class Decoder
{
public:
    struct State {
        quint64 pos = 0;  // the file's next byte
        quint32 bits = 0; // bits not used yet, and how many
        int count = 0;
        std::array<int, 4> dcPred{};
        qint64 mcu = 0;
        int restartsLeft = 0, nextRestart = 0;
    };

    Decoder(const JpegHeader &h, std::function<int(quint64 at)> byte)
        : m_h(h)
        , m_byte(std::move(byte))
    {
        m_state.pos = h.entropy;
        m_state.restartsLeft = h.restart;
    }

    State state() const { return m_state; }
    void setState(const State &s)
    {
        m_state = s;
        m_failed = false;
    }
    qint64 total() const { return qint64(m_h.mcusX) * m_h.mcusY; }
    bool failed() const { return m_failed; }

    // One unit of the picture (an MCU). False on anything a JPEG can't have.
    bool next()
    {
        if (m_h.restart && m_state.restartsLeft == 0 && m_state.mcu > 0) {
            // A restart marker, the next in turn, on a byte boundary.
            m_state.bits = 0;
            m_state.count = 0;
            int b = m_byte(m_state.pos), m = m_byte(m_state.pos + 1);
            while (b == 0xFF && m == 0xFF) { // fill bytes
                ++m_state.pos;
                b = m;
                m = m_byte(m_state.pos + 1);
            }
            if (b != 0xFF || m != 0xD0 + m_state.nextRestart)
                return fail();
            m_state.pos += 2;
            m_state.nextRestart = (m_state.nextRestart + 1) & 7;
            m_state.restartsLeft = m_h.restart;
            m_state.dcPred.fill(0);
        }
        for (int c = 0; c < m_h.components.size(); ++c) {
            const auto &comp = m_h.components[c];
            for (int b = 0; b < comp.h * comp.v; ++b) {
                if (!block(c, comp))
                    return fail();
            }
        }
        ++m_state.mcu;
        if (m_h.restart)
            --m_state.restartsLeft;
        return true;
    }

    // After the last unit: where the end marker is, or ~0.
    quint64 end()
    {
        quint64 at = m_state.pos;
        // Padding bits are ones; a partly used byte was already taken.
        for (int k = 0; k < 4; ++k, ++at) {
            if (m_byte(at) == 0xFF && m_byte(at + 1) == 0xD9)
                return at + 2;
        }
        return ~quint64(0);
    }

private:
    bool fail()
    {
        m_failed = true;
        return false;
    }

    int bit()
    {
        if (m_state.count == 0) {
            const int b = m_byte(m_state.pos);
            if (b < 0)
                return -1;
            if (b == 0xFF) {
                const int stuffed = m_byte(m_state.pos + 1);
                if (stuffed != 0)
                    return -1; // a marker in the middle of the picture data
                m_state.pos += 2;
            } else {
                m_state.pos += 1;
            }
            m_state.bits = quint32(b);
            m_state.count = 8;
        }
        --m_state.count;
        return (m_state.bits >> m_state.count) & 1;
    }

    int bits(int n)
    {
        int v = 0;
        for (int i = 0; i < n; ++i) {
            const int b = bit();
            if (b < 0)
                return -1;
            v = v << 1 | b;
        }
        return v;
    }

    int decode(const Huffman &t)
    {
        int code = 0;
        for (int length = 1; length <= 16; ++length) {
            const int b = bit();
            if (b < 0)
                return -1;
            code = code << 1 | b;
            if (t.maxCode[size_t(length)] >= 0 && code <= t.maxCode[size_t(length)]) {
                const int index = t.valPtr[size_t(length)] + code - t.minCode[size_t(length)];
                return index < t.values.size() ? uchar(t.values[index]) : -1;
            }
        }
        return -1;
    }

    bool block(int c, const JpegHeader::Component &comp)
    {
        const int s = decode(m_h.dc[size_t(comp.dc)]);
        if (s < 0 || s > 11)
            return false;
        if (s && bits(s) < 0)
            return false;
        for (int k = 1; k < 64;) {
            const int rs = decode(m_h.ac[size_t(comp.ac)]);
            if (rs < 0)
                return false;
            const int r = rs >> 4, size = rs & 15;
            if (size == 0) {
                if (r != 15)
                    break; // the rest of the block is zero
                k += 16;
                continue;
            }
            if (size > 10)
                return false;
            k += r;
            if (k > 63 || bits(size) < 0)
                return false;
            ++k;
        }
        Q_UNUSED(c)
        return true;
    }

    const JpegHeader &m_h;
    std::function<int(quint64 at)> m_byte;
    State m_state;
    bool m_failed = false;
};

} // namespace

bool jpeg(Context &c, Found &file)
{
    if (file.extents.size() != 1)
        return false;
    const quint64 cs = c.clusterSize;
    Pieces p;
    p.start = file.extents.first().start;
    p.cluster = cs;
    JpegHeader h;
    if (!readHeader(raw::read(c.source, p.start, 256 * 1024), &h) || h.entropy >= 4 * 1024 * 1024)
        return false;
    // The file's bytes through its pieces: clusters before `piece` from the start on, the rest
    // where the piece being tried puts them.
    QVector<quint64> map; // logical cluster -> where, for the pieces settled so far
    quint64 trying = ~quint64(0), tryingFrom = 0;
    QHash<quint64, QByteArray> cache;
    const auto clusterAt = [&](quint64 k) -> quint64 {
        if (trying != ~quint64(0) && k >= tryingFrom)
            return trying + (k - tryingFrom) * cs;
        if (k < quint64(map.size()))
            return map[qsizetype(k)];
        return map.isEmpty() ? p.start + k * cs : map.last() + (k - quint64(map.size()) + 1) * cs;
    };
    const auto byte = [&](quint64 at) -> int {
        const quint64 k = at / cs, where = clusterAt(k);
        auto it = cache.find(where);
        if (it == cache.end()) {
            if (cache.size() > 256)
                cache.clear();
            it = cache.insert(where, raw::read(c.source, where, cs));
        }
        const quint64 off = at % cs;
        return off < quint64(it->size()) ? uchar((*it)[qsizetype(off)]) : -1;
    };
    Decoder d(h, byte);
    QVector<Decoder::State> atCluster; // the state where each cluster's bytes start being used
    QSet<quint64> used;
    for (;;) {
        // Decode on until the end or a unit that can't be. Each cluster keeps the state from
        // before the unit that read into it first, so trying another piece for it re-reads all
        // of it through that piece.
        while (d.state().mcu < d.total()) {
            const Decoder::State s = d.state();
            const bool ok = d.next();
            while (quint64(atCluster.size()) <= d.state().pos / cs)
                atCluster << s;
            if (!ok)
                break;
        }
        if (!d.failed()) {
            const quint64 end = d.end();
            if (end == ~quint64(0))
                return false;
            const quint64 clusters = (end + cs - 1) / cs;
            while (quint64(map.size()) < clusters)
                map << clusterAt(quint64(map.size()));
            if (p.count == 1 && file.size == end && file.condition == Condition::Good)
                return false;
            p.map = map.mid(0, qsizetype(clusters));
            file.extents = p.extents(end);
            file.size = end;
            file.condition = Condition::Good;
            file.checked = true;
            if (p.count > 1)
                file.note = QCoreApplication::translate("lost::fragments", "Put back together from %n piece(s) on the drive.", nullptr, p.count);
            qCInfo(lcOps).noquote() << "Find Lost Files: a photo at" << p.start << "put together from" << p.count << "pieces";
            return true;
        }
        if (++p.count > kMostPieces || c.cancel)
            return false;
        // It broke in this cluster or a few before (bad data can decode a little way): the next
        // piece is where decoding carries on from one of those to the end, or furthest.
        const quint64 broke = d.state().pos / cs;
        const quint64 settled = quint64(map.size());
        quint64 bestAt = ~quint64(0), bestFrom = 0;
        qint64 bestMcu = -1;
        for (quint64 from = broke; from + 4 > broke && from >= std::max<quint64>(settled, 1) && from < quint64(atCluster.size()); --from) {
            const quint64 previous = clusterAt(from - 1);
            const auto test = [&](quint64 place, const uchar *, qsizetype) {
                trying = place;
                tryingFrom = from;
                d.setState(atCluster[qsizetype(from)]);
                const qint64 start = d.state().mcu;
                while (d.state().mcu < d.total() && d.next()) {
                }
                const qint64 reached = d.state().mcu;
                const bool whole = reached >= d.total() && d.end() != ~quint64(0);
                trying = ~quint64(0);
                // Decoding on a little from wrong data proves nothing: at least a cluster's worth.
                if (whole || (reached - start > 0 && d.state().pos >= (from + 1) * cs && reached > bestMcu)) {
                    if (reached > bestMcu) {
                        bestMcu = reached;
                        bestAt = place;
                        bestFrom = from;
                    }
                }
                return whole;
            };
            quint64 near = previous + cs;
            quint64 found = searchGrid(c, p, near, near + 64ull * 1024 * 1024, used, test);
            if (found == ~quint64(0))
                found = searchGrid(c, p, p.start > kBehind ? p.start - kBehind : 0, p.start + kAhead / 8, used, test);
            if (found != ~quint64(0))
                break;
        }
        if (bestAt == ~quint64(0))
            return false;
        // Settled: the clusters before the new piece, and the new piece's start.
        while (quint64(map.size()) < bestFrom) {
            map << clusterAt(quint64(map.size()));
            used.insert(map.last());
        }
        map << bestAt;
        used.insert(bestAt);
        d.setState(atCluster[qsizetype(bestFrom)]);
        atCluster.resize(qsizetype(bestFrom) + 1);
    }
}

} // namespace lost::fragments
