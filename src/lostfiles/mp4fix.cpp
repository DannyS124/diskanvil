// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mp4fix.h"

#include <QCoreApplication>
#include <QHash>
#include <QVector>
#include <QtEndian>

#include <cstring>
#include <optional>

namespace mp4fix {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("mp4fix", text);
}

constexpr quint64 kMostNal = 64 * 1024 * 1024;

struct Bytes {
    const uchar *p;
    quint64 n;
    bool has(quint64 at, quint64 len) const { return at <= n && len <= n - at; }
    quint32 u8(quint64 at) const { return has(at, 1) ? p[at] : 0; }
    quint32 be32(quint64 at) const { return has(at, 4) ? qFromBigEndian<quint32>(p + at) : 0; }
    quint64 be64(quint64 at) const { return has(at, 8) ? qFromBigEndian<quint64>(p + at) : 0; }
    bool is(quint64 at, const char *text, int len) const { return has(at, quint64(len)) && std::memcmp(p + at, text, size_t(len)) == 0; }
    QByteArray mid(quint64 at, quint64 len) const
    {
        return has(at, len) ? QByteArray(reinterpret_cast<const char *>(p + at), qsizetype(len)) : QByteArray();
    }
};

struct Box {
    QByteArray type;
    quint64 at = 0, header = 0, size = 0;
    quint64 body() const { return at + header; }
    quint64 end() const { return at + size; }
};

QVector<Box> children(const Bytes &b, quint64 from, quint64 to)
{
    QVector<Box> out;
    for (quint64 at = from; at + 8 <= to && out.size() < 10000;) {
        quint64 size = b.be32(at), header = 8;
        if (size == 1) {
            size = b.be64(at + 8);
            header = 16;
        } else if (size == 0) {
            size = to - at;
        }
        if (size < header || size > to - at)
            break;
        out.push_back({b.mid(at + 4, 4), at, header, size});
        at += size;
    }
    return out;
}

// A copy, not a pointer: the list it's from is often a temporary.
std::optional<Box> child(const QVector<Box> &list, const char *type)
{
    for (const Box &box : list) {
        if (box.type == type)
            return box;
    }
    return std::nullopt;
}

QByteArray be32(quint32 v)
{
    QByteArray b(4, '\0');
    qToBigEndian(v, b.data());
    return b;
}

QByteArray be64(quint64 v)
{
    QByteArray b(8, '\0');
    qToBigEndian(v, b.data());
    return b;
}

QByteArray box(const QByteArray &type, const QByteArray &payload)
{
    return be32(quint32(8 + payload.size())) + type + payload;
}

QByteArray fullBox(const char *type, const QByteArray &payload)
{
    return box(type, QByteArray(4, '\0') + payload); // version 0, no flags
}

struct Track {
    Box trak;
    QByteArray handler, format;
    quint32 timescale = 0;
    quint32 delta = 0;      // how long each sample lasts (the commonest in its table)
    quint32 sampleSize = 0; // when every sample is the same size (PCM sound)
    int nalLength = 0;      // H.264, H.265: how many bytes give each NAL unit's length
    quint64 biggestChunk = 0; // the most bytes one chunk of it takes
    bool hevc = false, mjpeg = false, pcm = false, reordered = false;
    QByteArray sps; // H.264: its sequence parameters (from avcC), for the frames' display order
};

Track readTrack(const Bytes &b, const Box &trak)
{
    Track t;
    t.trak = trak;
    const QVector<Box> kids = children(b, trak.body(), trak.end());
    const std::optional<Box> mdia = child(kids, "mdia");
    if (!mdia)
        return t;
    const QVector<Box> media = children(b, mdia->body(), mdia->end());
    if (const std::optional<Box> mdhd = child(media, "mdhd"))
        t.timescale = b.be32(mdhd->body() + (b.u8(mdhd->body()) == 1 ? 20 : 12));
    if (const std::optional<Box> hdlr = child(media, "hdlr"))
        t.handler = b.mid(hdlr->body() + 8, 4);
    const std::optional<Box> minf = child(media, "minf");
    const std::optional<Box> stbl = minf ? child(children(b, minf->body(), minf->end()), "stbl") : std::nullopt;
    if (!stbl)
        return t;
    const QVector<Box> table = children(b, stbl->body(), stbl->end());
    if (const std::optional<Box> stsd = child(table, "stsd")) {
        const quint64 entry = stsd->body() + 8;
        const quint64 entrySize = b.be32(entry);
        t.format = b.mid(entry + 4, 4);
        t.hevc = t.format == "hvc1" || t.format == "hev1";
        t.mjpeg = t.format == "jpeg" || t.format == "mjpa" || t.format == "AVDJ" || t.format == "dmb1";
        static const QList<QByteArray> pcm = {"sowt", "twos", "lpcm", "ipcm", "fpcm", "in24", "in32", "fl32", "fl64", "raw "};
        t.pcm = pcm.contains(t.format);
        if (t.handler == "vide" && entrySize > 86) {
            for (const Box &config : children(b, entry + 86, std::min(entry + entrySize, stsd->end()))) {
                if (config.type == "avcC") {
                    t.nalLength = int(b.u8(config.body() + 4) & 3) + 1;
                    if ((b.u8(config.body() + 5) & 0x1F) > 0)
                        t.sps = b.mid(config.body() + 8, (b.u8(config.body() + 6) << 8) | b.u8(config.body() + 7));
                }
                else if (config.type == "hvcC")
                    t.nalLength = int(b.u8(config.body() + 21) & 3) + 1;
            }
        }
    }
    if (const std::optional<Box> stts = child(table, "stts")) {
        const quint32 entries = std::min<quint32>(b.be32(stts->body() + 4), 100000);
        quint32 most = 0;
        for (quint32 i = 0; i < entries; ++i) {
            const quint32 count = b.be32(stts->body() + 8 + 8ull * i);
            if (count > most) {
                most = count;
                t.delta = b.be32(stts->body() + 12 + 8ull * i);
            }
        }
    }
    const std::optional<Box> stsz = child(table, "stsz"), stsc = child(table, "stsc");
    const std::optional<Box> offsets = child(table, "stco") ? child(table, "stco") : child(table, "co64");
    if (stsz)
        t.sampleSize = b.be32(stsz->body() + 4);
    if (stsz && stsc && offsets) {
        // How big its chunks are: so many samples each (stsc), each so big (stsz).
        const quint32 chunks = std::min<quint32>(b.be32(offsets->body() + 4), 1000000), runs = std::min<quint32>(b.be32(stsc->body() + 4), 100000);
        const quint32 samples = b.be32(stsz->body() + 8);
        quint32 run = 0, sample = 0;
        for (quint32 c = 1; c <= chunks && sample <= samples; ++c) {
            while (run + 1 < runs && b.be32(stsc->body() + 8 + 12ull * (run + 1)) <= c)
                ++run;
            const quint32 count = std::min<quint32>(b.be32(stsc->body() + 12 + 12ull * run), 1000000);
            quint64 bytes = 0;
            for (quint32 k = 0; k < count && sample < samples; ++k, ++sample)
                bytes += t.sampleSize ? t.sampleSize : b.be32(stsz->body() + 12 + 4ull * sample);
            t.biggestChunk = std::max(t.biggestChunk, bytes);
        }
    }
    t.reordered = child(table, "ctts").has_value();
    return t;
}

// --- H.264 and H.265 NAL units, each after its length

struct Nal {
    quint64 length = 0;
    int type = -1;
};

bool nal(const Track &v, const Bytes &b, quint64 at, quint64 end, Nal *out)
{
    const int n = v.nalLength;
    if (at + quint64(n) + 3 > end)
        return false;
    quint64 length = 0;
    for (int i = 0; i < n; ++i)
        length = length << 8 | b.p[at + quint64(i)];
    if (length < 2 || length > kMostNal || length > end - at - quint64(n))
        return false;
    const quint32 h0 = b.p[at + quint64(n)];
    if (h0 & 0x80)
        return false;
    int type;
    if (!v.hevc) {
        type = int(h0 & 0x1F);
        const int importance = int(h0 >> 5);
        if (type < 1 || type > 12 || ((type == 5 || type == 7 || type == 8) && importance == 0) || (type >= 9 && importance != 0))
            return false;
    } else {
        const quint32 h1 = b.p[at + quint64(n) + 1];
        type = int((h0 >> 1) & 0x3F);
        if ((((h0 & 1) << 5) | (h1 >> 3)) != 0 || (h1 & 7) == 0 || !(type <= 9 || (type >= 16 && type <= 21) || (type >= 32 && type <= 40)))
            return false;
    }
    out->length = length;
    out->type = type;
    return true;
}

bool slice(const Track &v, int type)
{
    return v.hevc ? type <= 9 || (type >= 16 && type <= 21) : type >= 1 && type <= 5;
}

bool keyframe(const Track &v, int type)
{
    return v.hevc ? type >= 16 && type <= 21 : type == 5;
}

// The NAL units that come before a picture's first slice.
bool startsPicture(const Track &v, int type)
{
    return v.hevc ? type == 35 || (type >= 32 && type <= 34) || type == 39 : type >= 6 && type <= 9;
}

bool firstSlice(const Track &v, const Bytes &b, quint64 at)
{
    return b.u8(at + quint64(v.nalLength) + (v.hevc ? 2 : 1)) & 0x80; // first_mb_in_slice is 0 / first_slice_segment_in_pic_flag
}

// Reads the bits of a slice header (no emulation-prevention bytes this early in it).
class Bits
{
public:
    Bits(const Bytes &b, quint64 at)
        : m_b(b)
        , m_at(at)
    {
    }
    int bit()
    {
        const int v = (m_b.u8(m_at + m_pos / 8) >> (7 - m_pos % 8)) & 1;
        ++m_pos;
        return v;
    }
    // Exp-Golomb: so many zeros, a one, then that many bits. -1 when it's longer than any sane field.
    qint64 ue()
    {
        int zeros = 0;
        while (bit() == 0)
            if (++zeros > 20)
                return -1;
        qint64 v = 0;
        for (int i = 0; i < zeros; ++i)
            v = v << 1 | bit();
        return (qint64(1) << zeros) - 1 + v;
    }

private:
    const Bytes &m_b;
    quint64 m_at;
    quint64 m_pos = 0;
};

// The first slice of a picture, with a header that makes sense: which part of the picture (the
// first), what kind of slice, which picture parameters.
bool firstSliceHeader(const Track &v, const Bytes &b, quint64 at, int type)
{
    Bits bits(b, at + quint64(v.nalLength) + (v.hevc ? 2 : 1));
    if (!v.hevc) {
        const qint64 firstMb = bits.ue(), sliceType = bits.ue(), parameters = bits.ue();
        return firstMb == 0 && sliceType >= 0 && sliceType <= 9 && parameters >= 0 && parameters <= 255;
    }
    if (!bits.bit())
        return false; // first_slice_segment_in_pic_flag
    if (type >= 16 && type <= 23)
        bits.bit(); // no_output_of_prior_pics_flag
    const qint64 parameters = bits.ue();
    return parameters >= 0 && parameters <= 63;
}

bool picture(const Track &v, const Bytes &b, quint64 at, const Nal &unit)
{
    return startsPicture(v, unit.type) || (slice(v, unit.type) && firstSliceHeader(v, b, at, unit.type));
}

// Frames start (again) here: NAL units that start a picture, followed by more of them, by the
// end, or (a picture of one NAL unit, then sound) by another picture's start within the most
// the clip's sound takes between two.
bool framesStart(const Track &v, const Bytes &b, quint64 at, quint64 end, quint64 gap)
{
    Nal unit;
    if (!nal(v, b, at, end, &unit) || !picture(v, b, at, unit))
        return false;
    quint64 e = at + quint64(v.nalLength) + unit.length;
    int count = 1;
    while (e < end && count < 256 && nal(v, b, e, end, &unit)) {
        e += quint64(v.nalLength) + unit.length;
        ++count;
    }
    if (count >= 2 || e >= end)
        return true;
    for (quint64 q = e; q < end && q - e <= gap; ++q) {
        if (nal(v, b, q, end, &unit) && picture(v, b, q, unit))
            return true;
    }
    return end - e <= gap; // the last picture, with only sound after it
}

struct Sample {
    quint64 at, size;
    bool key;
    qint64 order = -1; // H.264: pic_order_cnt_lsb of its first slice, -1 when not known
    bool reference = true; // other frames are predicted from it (nal_ref_idc isn't 0)
};

// --- H.264 display order: frames can be stored in another order than they're shown (B-frames);
// each one's slice header says where it goes (its picture order count), by the rules in the
// sequence parameters.

// A NAL unit's payload without the emulation-prevention bytes (00 00 03 → 00 00).
QByteArray rbsp(const QByteArray &nal)
{
    QByteArray out;
    out.reserve(nal.size());
    int zeros = 0;
    for (const char c : nal) {
        if (zeros >= 2 && c == 3) {
            zeros = 0;
            continue;
        }
        zeros = c == 0 ? zeros + 1 : 0;
        out += c;
    }
    return out;
}

class BitReader
{
public:
    explicit BitReader(QByteArray data)
        : m_data(std::move(data))
    {
    }
    bool ok() const { return m_pos <= quint64(m_data.size()) * 8; }
    quint32 bits(int n)
    {
        quint32 v = 0;
        for (int i = 0; i < n; ++i, ++m_pos)
            v = v << 1 | (m_pos / 8 < quint64(m_data.size()) ? (quint8(m_data[qsizetype(m_pos / 8)]) >> (7 - m_pos % 8)) & 1 : 0);
        return v;
    }
    qint64 ue()
    {
        int zeros = 0;
        while (bits(1) == 0 && ok())
            if (++zeros > 31)
                return -1;
        return (qint64(1) << zeros) - 1 + bits(zeros);
    }
    qint64 se()
    {
        const qint64 v = ue();
        return v & 1 ? (v + 1) / 2 : -(v / 2);
    }

private:
    QByteArray m_data;
    quint64 m_pos = 0;
};

struct OrderRules {
    bool known = false; // only picture order count type 0 is followed
    int frameNumBits = 0, orderBits = 0;
    bool frameMbsOnly = true, separateColourPlane = false;
};

OrderRules orderRules(const QByteArray &sps)
{
    OrderRules r;
    if (sps.size() < 4)
        return r;
    BitReader in(rbsp(sps.mid(1)));
    const quint32 profile = in.bits(8);
    in.bits(16); // constraints, level
    in.ue();     // seq_parameter_set_id
    static const QList<quint32> high = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};
    if (high.contains(profile)) {
        const qint64 chroma = in.ue();
        if (chroma == 3)
            r.separateColourPlane = in.bits(1);
        in.ue();
        in.ue();
        in.bits(1);
        if (in.bits(1)) { // scaling matrices
            for (int i = 0; i < (chroma == 3 ? 12 : 8); ++i) {
                if (!in.bits(1))
                    continue;
                int last = 8, next = 8;
                for (int j = 0; j < (i < 6 ? 16 : 64); ++j) {
                    if (next != 0)
                        next = int((last + in.se() + 256) % 256);
                    last = next == 0 ? last : next;
                }
            }
        }
    }
    r.frameNumBits = int(in.ue()) + 4;
    if (in.ue() != 0) // pic_order_cnt_type: 2 is the stored order itself, 1 isn't followed here
        return r;
    r.orderBits = int(in.ue()) + 4;
    in.ue();    // max_num_ref_frames
    in.bits(1); // gaps_in_frame_num_value_allowed_flag
    in.ue();    // width
    in.ue();    // height
    r.frameMbsOnly = in.bits(1);
    r.known = in.ok() && r.frameNumBits <= 16 && r.orderBits <= 16;
    return r;
}

// The slice's pic_order_cnt_lsb, from its header.
qint64 orderOf(const OrderRules &r, const Bytes &b, quint64 nalAt, quint64 nalLength, int nalBytes)
{
    const QByteArray head = b.mid(nalAt + quint64(nalBytes), std::min<quint64>(nalLength, 48));
    if (head.isEmpty())
        return -1;
    const bool idr = (quint8(head[0]) & 0x1F) == 5;
    BitReader in(rbsp(head.mid(1)));
    in.ue(); // first_mb_in_slice
    in.ue(); // slice_type
    in.ue(); // pic_parameter_set_id
    if (r.separateColourPlane)
        in.bits(2);
    in.bits(r.frameNumBits);
    if (!r.frameMbsOnly && in.bits(1)) // field_pic_flag: fields aren't followed
        return -1;
    if (idr)
        in.ue(); // idr_pic_id
    const qint64 lsb = in.bits(r.orderBits);
    return in.ok() ? lsb : -1;
}

// Composition offsets (ctts) from the frames' orders: within each run from one keyframe to the
// next, frames are shown in order of their picture order count. Empty when it's the stored order.
QVector<quint32> showOffsets(const QVector<Sample> &frames, const OrderRules &r, quint32 delta)
{
    QVector<qint64> order(frames.size());
    const qint64 maxLsb = qint64(1) << r.orderBits;
    qint64 prevMsb = 0, prevLsb = 0;
    for (qsizetype i = 0; i < frames.size(); ++i) {
        const Sample &f = frames[i];
        if (f.order < 0)
            return {};
        if (f.key) {
            prevMsb = 0;
            prevLsb = 0;
        }
        qint64 msb = prevMsb;
        if (f.order < prevLsb && prevLsb - f.order >= maxLsb / 2)
            msb = prevMsb + maxLsb;
        else if (f.order > prevLsb && f.order - prevLsb > maxLsb / 2)
            msb = prevMsb - maxLsb;
        order[i] = msb + f.order;
        if (f.reference) {
            prevMsb = msb;
            prevLsb = f.order;
        }
    }
    QVector<qint64> shown(frames.size());
    for (qsizetype start = 0; start < frames.size();) {
        qsizetype stop = start + 1;
        while (stop < frames.size() && !frames[stop].key)
            ++stop;
        QVector<qsizetype> run;
        for (qsizetype i = start; i < stop; ++i)
            run << i;
        std::stable_sort(run.begin(), run.end(), [&order](qsizetype a, qsizetype c) { return order[a] < order[c]; });
        for (qsizetype k = 0; k < run.size(); ++k)
            shown[run[k]] = start + k;
        start = stop;
    }
    qint64 lead = 0;
    bool reordered = false;
    for (qsizetype i = 0; i < frames.size(); ++i) {
        lead = std::max(lead, i - shown[i]);
        reordered = reordered || shown[i] != i;
    }
    if (!reordered)
        return {};
    QVector<quint32> out(frames.size());
    for (qsizetype i = 0; i < frames.size(); ++i)
        out[i] = quint32((shown[i] + lead - i) * delta);
    return out;
}

struct Chunk {
    quint64 at;
    quint32 samples;
};

QString duration(double seconds)
{
    const qint64 s = qint64(seconds + 0.5);
    return s >= 60 ? tr("%1 min %2 s").arg(s / 60).arg(s % 60) : tr("%1 s").arg(s);
}

// A copy of a container box with some of its children replaced, some left out, and the ones
// inside changed too.
QByteArray rebuilt(const Bytes &b, const Box &container, const QHash<QByteArray, QByteArray> &replace, const QList<QByteArray> &leaveOut)
{
    static const QList<QByteArray> containers = {"trak", "mdia", "minf"};
    QByteArray payload;
    for (const Box &kid : children(b, container.body(), container.end())) {
        if (leaveOut.contains(kid.type))
            continue;
        if (replace.contains(kid.type))
            payload += replace.value(kid.type);
        else if (containers.contains(kid.type))
            payload += rebuilt(b, kid, replace, leaveOut);
        else
            payload += b.mid(kid.at, kid.size);
    }
    return box(container.type, payload);
}

// A box copied with a duration in it changed (tkhd, mdhd, mvhd): where it is depends on the version.
QByteArray withDuration(const Bytes &b, const Box &box, quint64 offsetV0, quint64 offsetV1, quint64 value)
{
    QByteArray copy = b.mid(box.at, box.size);
    const bool v1 = b.u8(box.body()) == 1;
    const quint64 at = box.header + (v1 ? offsetV1 : offsetV0);
    if (v1 && at + 8 <= quint64(copy.size()))
        qToBigEndian(value, copy.data() + at);
    else if (!v1 && at + 4 <= quint64(copy.size()))
        qToBigEndian(quint32(std::min<quint64>(value, 0xFFFFFFFFu)), copy.data() + at);
    return copy;
}

} // namespace

Fixed rebuild(const uchar *brokenData, quint64 brokenSize, const uchar *referenceData, quint64 referenceSize, QIODevice &out,
              const std::function<bool(int)> &progress)
{
    Fixed f;
    const Bytes b{brokenData, brokenSize}, ref{referenceData, referenceSize};
    // What the working clip says.
    const QVector<Box> top = children(ref, 0, ref.n);
    const std::optional<Box> ftyp = child(top, "ftyp"), moov = child(top, "moov");
    if (!moov) {
        f.why = tr("The clip picked has no index itself: pick one that plays");
        return f;
    }
    const QVector<Box> movie = children(ref, moov->body(), moov->end());
    const std::optional<Box> mvhd = child(movie, "mvhd");
    Track video, sound;
    for (const Box &kid : movie) {
        if (kid.type != "trak")
            continue;
        const Track t = readTrack(ref, kid);
        if (t.handler == "vide" && video.format.isEmpty())
            video = t;
        else if (t.handler == "soun" && sound.format.isEmpty())
            sound = t;
    }
    if (video.format.isEmpty() || !mvhd || video.timescale == 0 || video.delta == 0) {
        f.why = tr("The clip picked has no picture track this can follow");
        return f;
    }
    if (!video.mjpeg && video.nalLength == 0) {
        f.why = tr("Its frames are stored as \"%1\", which this can't take apart (H.264, H.265 and Motion JPEG it can)").arg(QString::fromLatin1(video.format));
        return f;
    }
    const bool withSound = sound.pcm && sound.sampleSize > 0 && sound.timescale > 0 && sound.delta > 0;
    // Between two frames, at most this much sound (with room to spare).
    const quint64 gapMost = sound.format.isEmpty() ? 4096 : sound.biggestChunk * 2 + 4096;
    const quint32 movieScale = ref.be32(mvhd->body() + (ref.u8(mvhd->body()) == 1 ? 20 : 12));

    // Where the broken one's frames are: its mdat, to its end or to where the file stops.
    quint64 start = 0, end = b.n;
    for (const Box &kid : children(b, 0, b.n)) {
        if (kid.type == "mdat") {
            start = kid.body();
            end = std::min(kid.end(), b.n);
        }
    }
    if (start == 0) {
        // The box itself may be cut short or say it's bigger than what's there.
        for (quint64 at = 0; at + 8 <= std::min<quint64>(b.n, 1 << 20); ++at) {
            if (b.is(at + 4, "mdat", 4)) {
                start = at + (b.be32(at) == 1 ? 16 : 8);
                break;
            }
        }
    }

    QVector<Sample> frames;
    QVector<Chunk> chunks;
    quint64 skipped = 0, soundSamples = 0;
    const auto gap = [&](quint64 at, quint64 size) {
        if (size == 0)
            return;
        if (withSound && size >= sound.sampleSize) {
            const quint64 count = size / sound.sampleSize;
            chunks.push_back({at, quint32(count)});
            soundSamples += count;
            skipped += size - count * sound.sampleSize;
        } else {
            skipped += size;
        }
    };
    int lastPercent = -1;
    const auto tell = [&](quint64 pos) {
        const int percent = int((pos - start) * 95 / std::max<quint64>(1, end - start));
        if (percent != lastPercent) {
            lastPercent = percent;
            return !progress || progress(percent);
        }
        return true;
    };
    if (video.mjpeg) {
        for (quint64 pos = start; pos < end;) {
            if (!tell(pos)) {
                f.why = tr("Stopped");
                return f;
            }
            if (b.is(pos, "\xFF\xD8\xFF", 3)) {
                quint64 q = pos + 2;
                while (q + 1 < end && !(b.p[q] == 0xFF && b.p[q + 1] == 0xD9))
                    ++q;
                if (q + 1 >= end)
                    break; // the last one, cut short
                frames.push_back({pos, q + 2 - pos, true});
                pos = q + 2;
                continue;
            }
            quint64 q = pos + 1;
            while (q < end && !b.is(q, "\xFF\xD8\xFF", 3))
                ++q;
            gap(pos, q - pos);
            pos = q;
        }
    } else {
        qint64 sampleStart = -1, order = -1;
        bool hasSlice = false, key = false, reference = true;
        const OrderRules rules = video.hevc ? OrderRules() : orderRules(video.sps);
        const auto close = [&](quint64 at) {
            if (sampleStart >= 0 && hasSlice)
                frames.push_back({quint64(sampleStart), at - quint64(sampleStart), key, order, reference});
            else if (sampleStart >= 0)
                skipped += at - quint64(sampleStart);
            sampleStart = -1;
            order = -1;
            hasSlice = key = false;
            reference = true;
        };
        for (quint64 pos = start; pos < end;) {
            if (!tell(pos)) {
                f.why = tr("Stopped");
                return f;
            }
            Nal unit;
            if (nal(video, b, pos, end, &unit) && (sampleStart >= 0 || framesStart(video, b, pos, end, gapMost))) {
                if (slice(video, unit.type)) {
                    if (hasSlice && firstSlice(video, b, pos))
                        close(pos);
                    if (sampleStart < 0)
                        sampleStart = qint64(pos);
                    if (!hasSlice && rules.known) {
                        order = orderOf(rules, b, pos, unit.length, video.nalLength);
                        reference = (b.u8(pos + quint64(video.nalLength)) >> 5) != 0;
                    }
                    hasSlice = true;
                    key = key || keyframe(video, unit.type);
                } else {
                    if (hasSlice && startsPicture(video, unit.type))
                        close(pos);
                    if (sampleStart < 0)
                        sampleStart = qint64(pos);
                }
                pos += quint64(video.nalLength) + unit.length;
                continue;
            }
            close(pos);
            // Not a frame: sound, or damage, up to where frames start again.
            quint64 q = pos + 1;
            while (q < end && !framesStart(video, b, q, end, gapMost))
                ++q;
            gap(pos, q - pos);
            pos = q;
        }
        close(end);
    }
    if (frames.isEmpty()) {
        f.why = tr("No frames like the clip's were found in it: it may be from another camera, or recorded with other settings");
        return f;
    }

    // The new index: the working clip's, with this film's frames (and sound) in its tables.
    const QVector<quint32> offsets = video.hevc || video.mjpeg ? QVector<quint32>() : showOffsets(frames, orderRules(video.sps), video.delta);
    const quint64 frameTime = quint64(frames.size()) * video.delta;
    const quint64 soundTime = withSound ? soundSamples * sound.delta : 0;
    const double seconds = double(frameTime) / video.timescale;
    const quint64 movieTime = quint64(seconds * movieScale);
    const auto trackFor = [&](const Track &t, bool isVideo, quint64 dataStart) {
        QHash<QByteArray, QByteArray> replace;
        const QVector<Box> kids = children(ref, t.trak.body(), t.trak.end());
        const quint64 mediaTime = isVideo ? frameTime : soundTime;
        if (const std::optional<Box> tkhd = child(kids, "tkhd"))
            replace["tkhd"] = withDuration(ref, *tkhd, 20, 28, quint64(double(mediaTime) / t.timescale * movieScale));
        const std::optional<Box> mdia = child(kids, "mdia");
        if (const std::optional<Box> mdhd = mdia ? child(children(ref, mdia->body(), mdia->end()), "mdhd") : std::nullopt)
            replace["mdhd"] = withDuration(ref, *mdhd, 16, 24, mediaTime);
        // The sample table, made anew around the clip's description of the codec.
        const std::optional<Box> minf = mdia ? child(children(ref, mdia->body(), mdia->end()), "minf") : std::nullopt;
        const std::optional<Box> stbl = minf ? child(children(ref, minf->body(), minf->end()), "stbl") : std::nullopt;
        const std::optional<Box> stsd = stbl ? child(children(ref, stbl->body(), stbl->end()), "stsd") : std::nullopt;
        QByteArray table = stsd ? ref.mid(stsd->at, stsd->size) : QByteArray();
        if (isVideo) {
            table += fullBox("stts", be32(1) + be32(quint32(frames.size())) + be32(t.delta));
            QByteArray keys;
            quint32 keyCount = 0;
            for (int i = 0; i < frames.size(); ++i) {
                if (frames[i].key) {
                    keys += be32(quint32(i + 1));
                    ++keyCount;
                }
            }
            if (keyCount > 0 && keyCount < quint32(frames.size()))
                table += fullBox("stss", be32(keyCount) + keys);
            if (!offsets.isEmpty()) { // when each is shown: runs of the same offset
                QByteArray runs;
                quint32 runCount = 0;
                for (qsizetype i = 0; i < offsets.size();) {
                    qsizetype j = i + 1;
                    while (j < offsets.size() && offsets[j] == offsets[i])
                        ++j;
                    runs += be32(quint32(j - i)) + be32(offsets[i]);
                    ++runCount;
                    i = j;
                }
                table += fullBox("ctts", be32(runCount) + runs);
            }
            table += fullBox("stsc", be32(1) + be32(1) + be32(1) + be32(1));
            QByteArray sizes, offsets;
            for (const Sample &s : std::as_const(frames)) {
                sizes += be32(quint32(s.size));
                offsets += be64(dataStart + (s.at - start));
            }
            table += fullBox("stsz", be32(0) + be32(quint32(frames.size())) + sizes);
            table += fullBox("co64", be32(quint32(frames.size())) + offsets);
        } else {
            table += fullBox("stts", be32(1) + be32(quint32(soundSamples)) + be32(t.delta));
            QByteArray runs, offsets;
            quint32 runCount = 0, last = 0;
            for (int i = 0; i < chunks.size(); ++i) {
                if (chunks[i].samples != last) {
                    runs += be32(quint32(i + 1)) + be32(chunks[i].samples) + be32(1);
                    ++runCount;
                    last = chunks[i].samples;
                }
                offsets += be64(dataStart + (chunks[i].at - start));
            }
            table += fullBox("stsc", be32(runCount) + runs);
            table += fullBox("stsz", be32(t.sampleSize) + be32(quint32(soundSamples)));
            table += fullBox("co64", be32(quint32(chunks.size())) + offsets);
        }
        replace["stbl"] = box("stbl", table);
        return rebuilt(ref, t.trak, replace, {"edts", "tref"});
    };
    const auto index = [&](quint64 dataStart) {
        QByteArray payload = withDuration(ref, *mvhd, 16, 24, movieTime) + trackFor(video, true, dataStart);
        if (withSound && !chunks.isEmpty())
            payload += trackFor(sound, false, dataStart);
        return box("moov", payload);
    };
    const QByteArray head = ftyp ? ref.mid(ftyp->at, ftyp->size) : box("ftyp", QByteArray("isom\0\0\x02\0isomiso2mp41", 20));
    const quint64 dataSize = end - start;
    const quint64 mdatHeader = dataSize + 8 > 0xFFFFFFFFull ? 16 : 8;
    const quint64 moovSize = quint64(index(0).size());
    const QByteArray newIndex = index(quint64(head.size()) + moovSize + mdatHeader);
    QByteArray mdat = mdatHeader == 16 ? be32(1) + QByteArray("mdat") + be64(dataSize + 16) : be32(quint32(dataSize + 8)) + QByteArray("mdat");
    if (out.write(head) != head.size() || out.write(newIndex) != newIndex.size() || out.write(mdat) != mdat.size()) {
        f.why = tr("The repaired copy couldn't be written");
        return f;
    }
    for (quint64 at = start; at < end;) {
        const qint64 piece = qint64(std::min<quint64>(end - at, 16 * 1024 * 1024));
        if (out.write(reinterpret_cast<const char *>(b.p + at), piece) != piece) {
            f.why = tr("The repaired copy couldn't be written");
            return f;
        }
        at += quint64(piece);
    }
    if (progress)
        progress(100);
    f.ok = true;
    f.done << QCoreApplication::translate("mp4fix", "Rebuilt the index of its %n frame(s)", nullptr, int(frames.size())) + QStringLiteral(", ")
                  + duration(seconds);
    if (withSound && !chunks.isEmpty())
        f.done << tr("and of its sound");
    else if (!sound.format.isEmpty())
        f.done << tr("Its sound is packed (%1), which can't be cut up without the index: it plays without sound").arg(QString::fromLatin1(sound.format).trimmed());
    if (video.reordered && offsets.isEmpty())
        f.done << tr("Its frames play in the order they're stored: this camera reorders them, so it may judder in places");
    if (skipped > dataSize / 100)
        f.done << tr("Left out %1 bytes that weren't frames (damaged, or something else)").arg(skipped);
    return f;
}

} // namespace mp4fix
