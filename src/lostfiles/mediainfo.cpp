// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediainfo.h"

#include "filetypes.h"

#include <QCoreApplication>
#include <QStringDecoder>
#include <QStringList>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace mediainfo {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("mediainfo", text);
}

// A view of the bytes that never reads past their end: what isn't there reads as zero.
class Bytes
{
public:
    explicit Bytes(const QByteArray &data)
        : m_data(reinterpret_cast<const uchar *>(data.constData()))
        , m_size(quint64(data.size()))
    {
    }
    quint64 size() const { return m_size; }
    bool has(quint64 at, quint64 n) const { return at <= m_size && n <= m_size - at; }
    quint8 u8(quint64 at) const { return has(at, 1) ? m_data[at] : 0; }
    quint16 be16(quint64 at) const { return has(at, 2) ? qFromBigEndian<quint16>(m_data + at) : 0; }
    quint32 be32(quint64 at) const { return has(at, 4) ? qFromBigEndian<quint32>(m_data + at) : 0; }
    quint64 be64(quint64 at) const { return has(at, 8) ? qFromBigEndian<quint64>(m_data + at) : 0; }
    quint16 le16(quint64 at) const { return has(at, 2) ? qFromLittleEndian<quint16>(m_data + at) : 0; }
    quint32 le32(quint64 at) const { return has(at, 4) ? qFromLittleEndian<quint32>(m_data + at) : 0; }
    quint64 le64(quint64 at) const { return has(at, 8) ? qFromLittleEndian<quint64>(m_data + at) : 0; }
    bool is(quint64 at, const char *text) const
    {
        const quint64 n = strlen(text);
        return has(at, n) && std::memcmp(m_data + at, text, n) == 0;
    }
    QByteArray mid(quint64 at, quint64 n) const
    {
        if (at >= m_size)
            return {};
        n = std::min(n, m_size - at);
        return QByteArray(reinterpret_cast<const char *>(m_data + at), qsizetype(n));
    }

private:
    const uchar *m_data;
    quint64 m_size;
};

QString clean(QString text)
{
    text.remove(QChar(0));
    return text.simplified().left(200);
}

// --- MP4, MOV, 3GP, M4A: boxes inside boxes

QString videoCodec(const QByteArray &fourcc)
{
    if (fourcc == "avc1" || fourcc == "avc3")
        return QStringLiteral("H.264");
    if (fourcc == "hvc1" || fourcc == "hev1")
        return QStringLiteral("H.265");
    if (fourcc == "av01")
        return QStringLiteral("AV1");
    if (fourcc == "vp09")
        return QStringLiteral("VP9");
    if (fourcc == "mp4v")
        return QStringLiteral("MPEG-4");
    if (fourcc == "apcn" || fourcc == "apch" || fourcc == "apcs" || fourcc == "apco" || fourcc == "ap4h")
        return QStringLiteral("ProRes");
    return QString::fromLatin1(fourcc).trimmed();
}

QString audioCodec(const QByteArray &fourcc)
{
    if (fourcc == "mp4a")
        return QStringLiteral("AAC");
    if (fourcc == "ac-3")
        return QStringLiteral("AC-3");
    if (fourcc == "ec-3")
        return QStringLiteral("E-AC-3");
    if (fourcc == "Opus")
        return QStringLiteral("Opus");
    if (fourcc == "fLaC")
        return QStringLiteral("FLAC");
    if (fourcc == "alac")
        return QStringLiteral("Apple Lossless");
    if (fourcc == ".mp3")
        return QStringLiteral("MP3");
    return QString::fromLatin1(fourcc).trimmed();
}

struct Mp4Walk {
    explicit Mp4Walk(const Bytes &bytes)
        : b(bytes)
    {
    }

    const Bytes &b;
    Info info;
    int boxes = 0;
    // The track being read: its handler says whether it's the picture or the sound.
    QByteArray handler;
    int trackWidth = 0, trackHeight = 0;
    QByteArray trackCodec;
    int trackRate = 0, trackChannels = 0;
    bool haveVideo = false, haveAudio = false;

    void endTrack()
    {
        if (handler == "vide" && !haveVideo) {
            haveVideo = true;
            info.width = trackWidth;
            info.height = trackHeight;
            if (!trackCodec.isEmpty())
                info.codec = videoCodec(trackCodec) + (info.codec.isEmpty() ? QString() : QStringLiteral(", ") + info.codec);
        } else if (handler == "soun" && !haveAudio) {
            haveAudio = true;
            info.sampleRate = trackRate;
            info.channels = trackChannels;
            if (!trackCodec.isEmpty())
                info.codec = info.codec.isEmpty() ? audioCodec(trackCodec) : info.codec + QStringLiteral(", ") + audioCodec(trackCodec);
        }
        handler.clear();
        trackWidth = trackHeight = trackRate = trackChannels = 0;
        trackCodec.clear();
    }

    // The value in an iTunes-style tag: a 'data' box with a type, a locale, then the value.
    QByteArray tagValue(quint64 at, quint64 end) const
    {
        const quint32 size = b.be32(at);
        if (size < 16 || at + size > end || !b.is(at + 4, "data"))
            return {};
        return b.mid(at + 16, size - 16);
    }

    void walk(quint64 at, quint64 end, int depth)
    {
        while (at + 8 <= end && depth < 12 && ++boxes < 20000) {
            quint64 size = b.be32(at);
            const QByteArray type = b.mid(at + 4, 4);
            quint64 header = 8;
            if (size == 1) {
                size = b.be64(at + 8);
                header = 16;
            } else if (size == 0) {
                size = end - at;
            }
            if (size < header || size > end - at)
                return;
            const quint64 body = at + header, next = at + size;
            if (type == "moov" || type == "mdia" || type == "minf" || type == "stbl" || type == "udta" || type == "ilst"
                || type == "edts" || type == "dinf") {
                walk(body, next, depth + 1);
            } else if (type == "trak") {
                walk(body, next, depth + 1);
                endTrack();
            } else if (type == "meta") {
                walk(body + 4, next, depth + 1); // a full box: version and flags first
            } else if (type == "mvhd") {
                const bool v1 = b.u8(body) == 1;
                const quint32 scale = b.be32(body + (v1 ? 20 : 12));
                const quint64 duration = v1 ? b.be64(body + 24) : b.be32(body + 16);
                if (scale > 0 && duration > 0 && duration != 0xFFFFFFFFu && duration / scale < 1000ull * 3600)
                    info.durationMs = qint64(duration * 1000 / scale);
            } else if (type == "tkhd") {
                const bool v1 = b.u8(body) == 1;
                trackWidth = int(b.be32(body + (v1 ? 88 : 76)) >> 16);
                trackHeight = int(b.be32(body + (v1 ? 92 : 80)) >> 16);
            } else if (type == "hdlr") {
                handler = b.mid(body + 8, 4);
            } else if (type == "stsd") {
                // The first sample description: its format is the codec; sound says its rate.
                const quint64 entry = body + 8;
                if (b.be32(body + 4) > 0 && b.has(entry, 16)) {
                    trackCodec = b.mid(entry + 4, 4);
                    if (handler == "soun") {
                        trackChannels = b.be16(entry + 24);
                        trackRate = int(b.be32(entry + 32) >> 16);
                    }
                }
            } else if (type == "\xa9nam" || type == "\xa9" "ART" || type == "\xa9" "alb" || type == "\xa9" "day") {
                const QString value = clean(QString::fromUtf8(tagValue(body, next)));
                if (type == "\xa9nam")
                    info.title = value;
                else if (type == "\xa9" "ART")
                    info.artist = value;
                else if (type == "\xa9" "alb")
                    info.album = value;
                else
                    info.year = value.left(4);
            } else if (type == "covr" && info.cover.isEmpty()) {
                info.cover = tagValue(body, next);
            }
            at = next;
        }
    }
};

// --- MP3: an ID3 tag (title, artist, cover), then frames

QString id3Text(const QByteArray &frame)
{
    if (frame.isEmpty())
        return {};
    const char encoding = frame[0];
    const QByteArray text = frame.mid(1);
    switch (encoding) {
    case 0:
        return clean(QString::fromLatin1(text));
    case 1: // UTF-16 with a byte order mark
        return clean(QStringDecoder(QStringDecoder::Utf16)(text));
    case 2:
        return clean(QStringDecoder(QStringDecoder::Utf16BE)(text));
    default:
        return clean(QString::fromUtf8(text));
    }
}

// The picture in an APIC frame: encoding, MIME type, picture type, description, then the image.
QByteArray id3Picture(const QByteArray &frame)
{
    if (frame.size() < 4)
        return {};
    const char encoding = frame[0];
    qsizetype at = frame.indexOf('\0', 1);
    if (at < 0)
        return {};
    at += 2; // past the MIME type's end and the picture type
    // The description ends with one zero byte, or two for UTF-16.
    if (encoding == 1 || encoding == 2) {
        for (; at + 1 < frame.size(); at += 2) {
            if (frame[at] == 0 && frame[at + 1] == 0) {
                at += 2;
                break;
            }
        }
    } else {
        const qsizetype end = frame.indexOf('\0', at);
        if (end < 0)
            return {};
        at = end + 1;
    }
    return at < frame.size() ? frame.mid(at) : QByteArray();
}

void readMp3(const Bytes &b, quint64 fileSize, Info &info)
{
    quint64 audio = 0;
    if (b.is(0, "ID3")) {
        const int version = b.u8(3);
        const quint32 tagSize = (b.u8(6) & 0x7F) << 21 | (b.u8(7) & 0x7F) << 14 | (b.u8(8) & 0x7F) << 7 | (b.u8(9) & 0x7F);
        audio = 10 + quint64(tagSize);
        quint64 at = 10;
        if (b.u8(5) & 0x40) // an extended header first
            at += version == 4 ? ((b.u8(10) & 0x7F) << 21 | (b.u8(11) & 0x7F) << 14 | (b.u8(12) & 0x7F) << 7 | (b.u8(13) & 0x7F)) : b.be32(10) + 4;
        for (int frames = 0; frames < 500 && at + 10 <= std::min<quint64>(audio, b.size()); ++frames) {
            const QByteArray id = b.mid(at, 4);
            if (id.at(0) == 0)
                break;
            const quint32 size = version == 4 ? ((b.u8(at + 4) & 0x7F) << 21 | (b.u8(at + 5) & 0x7F) << 14 | (b.u8(at + 6) & 0x7F) << 7 | (b.u8(at + 7) & 0x7F))
                                              : b.be32(at + 4);
            if (size == 0 || at + 10 + size > audio)
                break;
            const QByteArray body = b.mid(at + 10, size);
            if (id == "TIT2")
                info.title = id3Text(body);
            else if (id == "TPE1")
                info.artist = id3Text(body);
            else if (id == "TALB")
                info.album = id3Text(body);
            else if ((id == "TYER" || id == "TDRC") && info.year.isEmpty())
                info.year = id3Text(body).left(4);
            else if (id == "APIC" && info.cover.isEmpty())
                info.cover = id3Picture(body);
            at += 10 + size;
        }
    } else if (fileSize >= 128 && b.size() == fileSize && b.is(b.size() - 128, "TAG")) {
        // The old kind of tag, in the last 128 bytes.
        const quint64 t = b.size() - 128;
        info.title = clean(QString::fromLatin1(b.mid(t + 3, 30)));
        info.artist = clean(QString::fromLatin1(b.mid(t + 33, 30)));
        info.album = clean(QString::fromLatin1(b.mid(t + 63, 30)));
        info.year = clean(QString::fromLatin1(b.mid(t + 93, 4)));
    }
    // The first frame: its bit rate and sample rate. A Xing or Info header in it says how many
    // frames there are; without one, the bit rate is taken to be the same all through.
    for (quint64 at = audio; at + 4 <= std::min<quint64>(b.size(), audio + 65536); ++at) {
        if (b.u8(at) != 0xFF || (b.u8(at + 1) & 0xE0) != 0xE0)
            continue;
        const int version = (b.u8(at + 1) >> 3) & 3; // 3: MPEG-1, 2: MPEG-2, 0: MPEG-2.5
        const int layer = (b.u8(at + 1) >> 1) & 3;   // 1: layer III
        const int rateIndex = b.u8(at + 2) >> 4, freqIndex = (b.u8(at + 2) >> 2) & 3;
        if (layer != 1 || version == 1 || rateIndex == 0 || rateIndex == 15 || freqIndex == 3)
            continue;
        static const int v1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
        static const int v2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
        static const int freq[] = {44100, 48000, 32000};
        const int kbps = version == 3 ? v1[rateIndex] : v2[rateIndex];
        const int rate = freq[freqIndex] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
        const bool mono = (b.u8(at + 3) >> 6) == 3;
        info.codec = QStringLiteral("MP3, %1 kbit/s").arg(kbps);
        info.sampleRate = rate;
        info.channels = mono ? 1 : 2;
        const quint64 side = version == 3 ? (mono ? 17 : 32) : (mono ? 9 : 17);
        const quint64 xing = at + 4 + side;
        if (b.is(xing, "Xing") || b.is(xing, "Info")) {
            const quint32 flags = b.be32(xing + 4);
            if (flags & 1) {
                const quint64 frames = b.be32(xing + 8);
                info.durationMs = qint64(frames * (version == 3 ? 1152 : 576) * 1000 / quint64(rate));
                info.codec = QStringLiteral("MP3");
            }
        }
        if (info.durationMs < 0 && kbps > 0 && fileSize > audio)
            info.durationMs = qint64((fileSize - audio) * 8 / quint64(kbps));
        break;
    }
}

// --- Ogg: pages carrying packets; the first ones say what's inside

void readComments(const QByteArray &packet, quint64 start, Info &info)
{
    const Bytes b(packet);
    quint64 at = start;
    at += 4 + quint64(b.le32(at)); // the vendor string
    const quint32 count = std::min<quint32>(b.le32(at), 1000);
    at += 4;
    for (quint32 i = 0; i < count && b.has(at, 4); ++i) {
        const quint32 length = b.le32(at);
        at += 4;
        if (!b.has(at, length))
            break;
        const QString comment = QString::fromUtf8(b.mid(at, length));
        at += length;
        const QString key = comment.section(QLatin1Char('='), 0, 0).toUpper(), value = clean(comment.section(QLatin1Char('='), 1));
        if (key == QLatin1String("TITLE"))
            info.title = value;
        else if (key == QLatin1String("ARTIST"))
            info.artist = value;
        else if (key == QLatin1String("ALBUM"))
            info.album = value;
        else if (key == QLatin1String("DATE"))
            info.year = value.left(4);
    }
}

void readOgg(const Bytes &b, quint64 fileSize, Info &info)
{
    // The first three packets of the first stream, put together from the pages' segments.
    QList<QByteArray> packets;
    QByteArray current;
    quint32 serial = 0;
    quint64 at = 0;
    for (int pages = 0; pages < 64 && packets.size() < 3 && b.is(at, "OggS"); ++pages) {
        const int segments = b.u8(at + 26);
        if (pages == 0)
            serial = b.le32(at + 14);
        quint64 data = at + 27 + quint64(segments);
        const bool mine = b.le32(at + 14) == serial;
        for (int s = 0; s < segments; ++s) {
            const int n = b.u8(at + 27 + quint64(s));
            if (mine) {
                current += b.mid(data, quint64(n));
                if (n < 255) {
                    packets << current;
                    current.clear();
                }
            }
            data += quint64(n);
        }
        at = data;
    }
    int rate = 0, preSkip = 0;
    for (const QByteArray &p : std::as_const(packets)) {
        const Bytes pb(p);
        if (pb.is(0, "\x01vorbis")) {
            info.codec = QStringLiteral("Vorbis");
            info.channels = pb.u8(11);
            rate = int(pb.le32(12));
        } else if (pb.is(0, "OpusHead")) {
            info.codec = QStringLiteral("Opus");
            info.channels = pb.u8(9);
            preSkip = pb.le16(10);
            rate = 48000; // Opus always counts in 48 kHz
            info.sampleRate = int(pb.le32(12));
        } else if (pb.is(0, "\x80theora")) {
            info.codec = QStringLiteral("Theora");
            info.width = int(pb.be16(10)) * 16;
            info.height = int(pb.be16(12)) * 16;
        } else if (pb.is(0, "\x03vorbis")) {
            readComments(p, 7, info);
        } else if (pb.is(0, "OpusTags")) {
            readComments(p, 8, info);
        }
    }
    if (info.sampleRate == 0)
        info.sampleRate = rate;
    // How long: the position in samples of the last page there is.
    if (rate > 0 && b.size() == fileSize) {
        const QByteArray tail = b.mid(b.size() > 65536 ? b.size() - 65536 : 0, 65536);
        const qsizetype last = tail.lastIndexOf("OggS");
        if (last >= 0) {
            const Bytes tb(tail);
            const quint64 granule = tb.le64(quint64(last) + 6);
            if (granule != ~quint64(0) && granule > quint64(preSkip))
                info.durationMs = qint64((granule - quint64(preSkip)) * 1000 / quint64(rate));
        }
    }
}

// --- RIFF: WAV and AVI

void readRiff(const Bytes &b, Info &info)
{
    const bool avi = b.is(8, "AVI ");
    quint64 at = 12;
    quint32 byteRate = 0;
    for (int chunks = 0; chunks < 1000 && b.has(at, 8); ++chunks) {
        const QByteArray id = b.mid(at, 4);
        const quint64 size = b.le32(at + 4), body = at + 8;
        if (id == "fmt " && !avi) {
            info.codec = b.le16(body) == 1 ? QStringLiteral("PCM") : QStringLiteral("WAV");
            info.channels = b.le16(body + 2);
            info.sampleRate = int(b.le32(body + 4));
            byteRate = b.le32(body + 8);
            if (b.le16(body) == 1)
                info.codec = QStringLiteral("PCM, %1-bit").arg(b.le16(body + 14));
        } else if (id == "data" && !avi && byteRate > 0) {
            info.durationMs = qint64(size * 1000 / byteRate);
        } else if (id == "LIST") {
            const QByteArray kind = b.mid(body, 4);
            if (kind == "hdrl" || kind == "strl" || kind == "INFO") {
                // Inside: avih, strh, INAM... read with the same loop, one level down.
                quint64 in = body + 4;
                const quint64 end = std::min(body + size, b.size());
                for (int k = 0; k < 200 && in + 8 <= end; ++k) {
                    const QByteArray sub = b.mid(in, 4);
                    const quint64 subSize = b.le32(in + 4), subBody = in + 8;
                    if (sub == "avih") {
                        const quint64 usPerFrame = b.le32(subBody), frames = b.le32(subBody + 16);
                        info.durationMs = qint64(usPerFrame * frames / 1000);
                        info.width = int(b.le32(subBody + 32));
                        info.height = int(b.le32(subBody + 36));
                    } else if (sub == "strh" && b.is(subBody, "vids") && info.codec.isEmpty()) {
                        const QByteArray fourcc = b.mid(subBody + 4, 4).toUpper();
                        info.codec = fourcc == "H264" || fourcc == "AVC1" ? QStringLiteral("H.264")
                                   : fourcc == "XVID" || fourcc == "DIVX" || fourcc == "DX50" ? QStringLiteral("MPEG-4 (%1)").arg(QString::fromLatin1(fourcc))
                                   : fourcc == "MJPG" ? QStringLiteral("Motion JPEG")
                                                       : QString::fromLatin1(fourcc).trimmed();
                    } else if (sub == "LIST") {
                        in = subBody + 4; // strl inside hdrl: step into it
                        continue;
                    } else if (sub == "INAM") {
                        info.title = clean(QString::fromLatin1(b.mid(subBody, subSize)));
                    } else if (sub == "IART") {
                        info.artist = clean(QString::fromLatin1(b.mid(subBody, subSize)));
                    }
                    in = subBody + subSize + (subSize & 1);
                }
            }
        }
        at = body + size + (size & 1);
    }
}

QString duration(qint64 ms)
{
    const qint64 s = (ms + 500) / 1000;
    if (s >= 3600)
        return tr("%1 h %2 min").arg(s / 3600).arg((s % 3600) / 60);
    if (s >= 60)
        return tr("%1 min %2 s").arg(s / 60).arg(s % 60);
    return tr("%1 s").arg(s);
}

} // namespace

Info read(int type, const QByteArray &data, quint64 fileSize)
{
    Info info;
    if (type < 0 || type >= filetypes::types().size())
        return info;
    const QString id = filetypes::types()[type].id;
    const Bytes b(data);
    if (id == QLatin1String("mp4") || id == QLatin1String("mov") || id == QLatin1String("3gp") || id == QLatin1String("m4a")) {
        Mp4Walk walk(b);
        walk.walk(0, b.size(), 0);
        info = walk.info;
    } else if (id == QLatin1String("mp3")) {
        readMp3(b, fileSize, info);
    } else if (id == QLatin1String("ogg") || id == QLatin1String("opus") || id == QLatin1String("ogv")) {
        readOgg(b, fileSize, info);
    } else if (id == QLatin1String("wav") || id == QLatin1String("avi")) {
        readRiff(b, info);
    }
    return info;
}

QString describe(const Info &info)
{
    QStringList first;
    if (info.durationMs >= 0)
        first << duration(info.durationMs);
    if (info.width > 0 && info.height > 0)
        first << QStringLiteral("%1 × %2").arg(info.width).arg(info.height);
    if (!info.codec.isEmpty())
        first << info.codec;
    if (info.sampleRate > 0 && info.width == 0) {
        const QString khz = QString::number(info.sampleRate / 1000.0, 'g', 3);
        first << (info.channels == 1 ? tr("%1 kHz mono").arg(khz) : info.channels == 2 ? tr("%1 kHz stereo").arg(khz)
                                                                                         : tr("%1 kHz").arg(khz));
    }
    QString second;
    if (!info.artist.isEmpty() && !info.title.isEmpty())
        second = tr("%1: %2").arg(info.artist, info.title);
    else
        second = !info.title.isEmpty() ? info.title : info.artist;
    QStringList extra;
    if (!info.album.isEmpty())
        extra << info.album;
    if (!info.year.isEmpty())
        extra << info.year;
    if (!extra.isEmpty())
        second += (second.isEmpty() ? QString() : QStringLiteral(" ")) + QStringLiteral("(%1)").arg(extra.join(QStringLiteral(", ")));
    QStringList lines;
    if (!first.isEmpty())
        lines << first.join(QStringLiteral(" · "));
    if (!second.isEmpty())
        lines << second;
    return lines.join(QLatin1Char('\n'));
}

} // namespace mediainfo
