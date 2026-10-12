// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mediainfo.h"

#include "filetypes.h"
#include "places.h"

#include <QCoreApplication>
#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <QTimeZone>
#include <QStringDecoder>
#include <QStringList>
#include <QtEndian>

#include <algorithm>
#include <cmath>
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

    QVector<QByteArray> keys; // QuickTime's metadata keys: ilst's items are numbered after them
    bool inIlst = false;

    void place(const QString &iso6709)
    {
        double lat = 0, lon = 0, alt = 0;
        bool hasAlt = false;
        if (!info.hasLocation && parseIso6709(iso6709, &lat, &lon, &alt, &hasAlt)) {
            info.hasLocation = true;
            info.latitude = lat;
            info.longitude = lon;
            info.hasAltitude = hasAlt;
            info.altitude = alt;
        }
    }

    // One of Apple's QuickTime keys and its value.
    void keyed(const QByteArray &key, const QString &value)
    {
        if (key == "com.apple.quicktime.location.ISO6709")
            place(value);
        else if (key == "com.apple.quicktime.make")
            info.make = value;
        else if (key == "com.apple.quicktime.model")
            info.model = value;
        else if (key == "com.apple.quicktime.software")
            info.software = value;
        else if (key == "com.apple.quicktime.creationdate" && !info.made.isValid()) {
            // "2024-07-14T13:22:05+0200"
            QString t = value.left(24);
            if (t.size() == 24 && (t[19] == QLatin1Char('+') || t[19] == QLatin1Char('-')))
                t.insert(22, QLatin1Char(':'));
            const QDateTime when = QDateTime::fromString(t, Qt::ISODate);
            if (when.isValid() && when.date().year() >= 1990)
                info.made = when;
        }
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
            const quint32 item = b.be32(at + 4);
            if (inIlst && item >= 1 && item <= quint32(keys.size())) {
                // QuickTime metadata: the item is the key's number, its value in a 'data' box.
                keyed(keys[item - 1], clean(QString::fromUtf8(tagValue(body, next))));
            } else if (type == "ilst") {
                const bool was = inIlst;
                inIlst = true;
                walk(body, next, depth + 1);
                inIlst = was;
            } else if (type == "keys") {
                // A full box: version and flags, how many, then each: its size, "mdta", the key.
                keys.clear();
                const quint32 count = b.be32(body + 4);
                quint64 k = body + 8;
                for (quint32 i = 0; i < count && i < 500 && k + 8 <= next; ++i) {
                    const quint32 keySize = b.be32(k);
                    if (keySize < 8 || keySize > next - k)
                        break;
                    keys << b.mid(k + 8, keySize - 8);
                    k += keySize;
                }
            } else if (type == "\xa9xyz") {
                // Where: a 16-bit length, a language, then ISO 6709 text.
                const quint32 length = b.be16(body);
                if (length > 0 && body + 4 + length <= next)
                    place(QString::fromLatin1(b.mid(body + 4, length)));
            } else if (type == "loci") {
                // 3GPP's: version and flags, a language, a name, a role, then 16.16 fixed longitude,
                // latitude and altitude.
                quint64 p = body + 6;
                while (p < next && b.u8(p) != 0)
                    ++p;
                p += 2;
                if (p + 12 <= next && !info.hasLocation) {
                    const double lon = qint32(b.be32(p)) / 65536.0, lat = qint32(b.be32(p + 4)) / 65536.0;
                    if (std::abs(lat) <= 90 && std::abs(lon) <= 180 && (lat != 0 || lon != 0)) {
                        info.hasLocation = true;
                        info.latitude = lat;
                        info.longitude = lon;
                        info.hasAltitude = true;
                        info.altitude = qint32(b.be32(p + 8)) / 65536.0;
                    }
                }
            } else if (type == "moov" || type == "mdia" || type == "minf" || type == "stbl" || type == "udta"
                || type == "edts" || type == "dinf") {
                walk(body, next, depth + 1);
            } else if (type == "trak") {
                walk(body, next, depth + 1);
                endTrack();
            } else if (type == "meta") {
                // ISO's is a full box (version and flags first); QuickTime's isn't.
                const bool quickTime = b.is(body + 4, "hdlr") || b.is(body + 4, "keys");
                walk(quickTime ? body : body + 4, next, depth + 1);
            } else if (type == "mvhd") {
                const bool v1 = b.u8(body) == 1;
                // Seconds since 1904, in UTC. Cameras without a clock write 0, or 1904 itself.
                const quint64 made = v1 ? b.be64(body + 4) : b.be32(body + 4);
                const qint64 since1970 = qint64(made) - 2082844800LL;
                if (made > 0 && since1970 > 315532800LL && since1970 < 4102444800LL)
                    info.recorded = QDateTime::fromSecsSinceEpoch(since1970, QTimeZone::UTC);
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

// --- FLAC: its information blocks (the stream's facts, Vorbis-style tags, the cover)

void readFlac(const Bytes &b, Info &info)
{
    if (!b.is(0, "fLaC"))
        return;
    quint64 at = 4;
    for (int blocks = 0; blocks < 1000 && b.has(at, 4); ++blocks) {
        const quint32 head = b.be32(at), type = (head >> 24) & 0x7F, length = head & 0xFFFFFF;
        const quint64 body = at + 4;
        if (type == 0 && length >= 18) { // STREAMINFO
            const quint32 rate = b.be32(body + 10) >> 12;
            const quint64 samples = (quint64(b.u8(body + 13) & 0x0F) << 32) | b.be32(body + 14);
            info.codec = QStringLiteral("FLAC");
            info.sampleRate = int(rate);
            info.channels = ((b.u8(body + 12) >> 1) & 7) + 1;
            if (rate && samples)
                info.durationMs = qint64(samples * 1000 / rate);
        } else if (type == 4) {
            readComments(b.mid(body, length), 0, info);
        } else if (type == 6 && info.cover.isEmpty()) { // PICTURE: kind, MIME type, words, size, then it
            quint64 p = body + 4;
            p += 4 + quint64(b.be32(p));
            p += 4 + quint64(b.be32(p));
            p += 16;
            const quint32 size = b.be32(p);
            if (b.has(p + 4, size) && p + 4 + size <= body + length)
                info.cover = b.mid(p + 4, size);
        }
        at = body + length;
        if (head & 0x80000000) // the last one
            break;
    }
}

// --- Matroska and WebM: elements inside elements, each an ID and a size

struct Element {
    quint32 id = 0;
    quint64 body = 0, size = 0;
    bool open = false; // its size isn't said (a live recording): it runs to the end
};

int vintLength(quint32 first)
{
    int length = 1;
    while (length <= 8 && !(first & (0x80u >> (length - 1))))
        ++length;
    return length;
}

bool element(const Bytes &b, quint64 at, quint64 end, Element &e)
{
    const quint32 first = b.u8(at);
    const int idLength = vintLength(first);
    if (first == 0 || idLength > 4 || !b.has(at, quint64(idLength) + 1))
        return false;
    e.id = 0;
    for (int i = 0; i < idLength; ++i)
        e.id = e.id << 8 | b.u8(at + quint64(i));
    quint64 p = at + quint64(idLength);
    const quint32 sizeFirst = b.u8(p);
    const int sizeLength = vintLength(sizeFirst);
    if (sizeFirst == 0 || sizeLength > 8)
        return false;
    quint64 size = sizeFirst & (0xFFu >> sizeLength);
    bool ones = size == (0xFFu >> sizeLength);
    for (int i = 1; i < sizeLength; ++i) {
        size = size << 8 | b.u8(p + quint64(i));
        ones = ones && b.u8(p + quint64(i)) == 0xFF;
    }
    e.body = p + quint64(sizeLength);
    e.open = ones;
    e.size = ones || e.body > end ? (end > e.body ? end - e.body : 0) : std::min(size, end - e.body);
    return true;
}

quint64 ebmlUnsigned(const Bytes &b, const Element &e)
{
    quint64 v = 0;
    for (quint64 i = 0; i < std::min<quint64>(e.size, 8); ++i)
        v = v << 8 | b.u8(e.body + i);
    return v;
}

double ebmlFloat(const Bytes &b, const Element &e)
{
    if (e.size == 4) {
        const quint32 bits = b.be32(e.body);
        float f;
        std::memcpy(&f, &bits, sizeof f);
        return f;
    }
    if (e.size == 8) {
        const quint64 bits = b.be64(e.body);
        double d;
        std::memcpy(&d, &bits, sizeof d);
        return d;
    }
    return 0;
}

QString matroskaCodec(const QByteArray &id)
{
    static const QHash<QByteArray, QString> names = {
        {"V_MPEG4/ISO/AVC", QStringLiteral("H.264")}, {"V_MPEGH/ISO/HEVC", QStringLiteral("H.265")}, {"V_AV1", QStringLiteral("AV1")},
        {"V_VP8", QStringLiteral("VP8")},             {"V_VP9", QStringLiteral("VP9")},              {"V_THEORA", QStringLiteral("Theora")},
        {"V_MPEG2", QStringLiteral("MPEG-2")},         {"V_MS/VFW/FOURCC", QStringLiteral("VfW")},    {"A_AAC", QStringLiteral("AAC")},
        {"A_OPUS", QStringLiteral("Opus")},           {"A_VORBIS", QStringLiteral("Vorbis")},        {"A_FLAC", QStringLiteral("FLAC")},
        {"A_AC3", QStringLiteral("AC-3")},            {"A_EAC3", QStringLiteral("E-AC-3")},          {"A_DTS", QStringLiteral("DTS")},
        {"A_MPEG/L3", QStringLiteral("MP3")},         {"A_PCM/INT/LIT", QStringLiteral("PCM")},      {"A_TRUEHD", QStringLiteral("TrueHD")}};
    if (names.contains(id))
        return names.value(id);
    if (id.startsWith("A_AAC"))
        return QStringLiteral("AAC");
    return QString::fromLatin1(id.mid(2)).left(20);
}

struct MatroskaWalk {
    MatroskaWalk(const Bytes &bytes, Info &into)
        : b(bytes)
        , info(into)
    {
    }
    const Bytes &b;
    Info &info;
    quint64 timecodeScale = 1000000;
    double duration = -1;
    QString videoCodec, audioCodec;
    int steps = 0;

    void walk(quint64 from, quint64 to, int depth)
    {
        Element e;
        for (quint64 at = from; at < to && depth < 12 && ++steps < 200000 && element(b, at, to, e);) {
            switch (e.id) {
            case 0x18538067: // Segment
            case 0x1549A966: // Info
            case 0x1654AE6B: // Tracks
            case 0x1941A469: // Attachments
            case 0x1254C367: // Tags
            case 0x7373:     // Tag
                walk(e.body, e.body + e.size, depth + 1);
                break;
            case 0xAE: // TrackEntry
                track(e);
                break;
            case 0x61A7: // AttachedFile: a cover, when it's a picture
                attachment(e);
                break;
            case 0x67C8: // SimpleTag
                tag(e);
                break;
            case 0x2AD7B1:
                timecodeScale = std::max<quint64>(1, ebmlUnsigned(b, e));
                break;
            case 0x4489:
                duration = ebmlFloat(b, e);
                break;
            case 0x7BA9:
                if (info.title.isEmpty())
                    info.title = clean(QString::fromUtf8(b.mid(e.body, e.size)));
                break;
            case 0x4461: { // DateUTC: nanoseconds from the start of 2001
                const qint64 ns = qint64(ebmlUnsigned(b, e));
                const QDateTime t = QDateTime(QDate(2001, 1, 1), QTime(0, 0), QTimeZone::UTC).addMSecs(ns / 1000000);
                if (t.date().year() > 1990 && t.date().year() < 2200)
                    info.recorded = t;
                break;
            }
            case 0x1F43B675: // Cluster: the film itself, stepped over (the tags can be after it)
            default:
                break;
            }
            if (e.open && e.id != 0x18538067)
                return;
            at = e.body + e.size;
        }
    }

    void track(const Element &entry)
    {
        quint64 type = 0;
        QByteArray codec;
        Element e;
        for (quint64 at = entry.body, end = entry.body + entry.size; at < end && element(b, at, end, e); at = e.body + e.size) {
            if (e.id == 0x83)
                type = ebmlUnsigned(b, e);
            else if (e.id == 0x86)
                codec = b.mid(e.body, std::min<quint64>(e.size, 64));
            else if (e.id == 0xE0 || e.id == 0xE1) { // Video, Audio
                Element v;
                for (quint64 p = e.body, vend = e.body + e.size; p < vend && element(b, p, vend, v); p = v.body + v.size) {
                    if (v.id == 0xB0 && info.width == 0)
                        info.width = int(std::min<quint64>(ebmlUnsigned(b, v), 100000));
                    else if (v.id == 0xBA && info.height == 0)
                        info.height = int(std::min<quint64>(ebmlUnsigned(b, v), 100000));
                    else if (v.id == 0xB5 && info.sampleRate == 0)
                        info.sampleRate = int(std::clamp(ebmlFloat(b, v), 0.0, 1e7));
                    else if (v.id == 0x9F && info.channels == 0)
                        info.channels = int(std::min<quint64>(ebmlUnsigned(b, v), 64));
                }
            }
        }
        if (type == 1 && videoCodec.isEmpty())
            videoCodec = matroskaCodec(codec);
        else if (type == 2 && audioCodec.isEmpty())
            audioCodec = matroskaCodec(codec);
    }

    void attachment(const Element &file)
    {
        QByteArray mime;
        Element data, e;
        for (quint64 at = file.body, end = file.body + file.size; at < end && element(b, at, end, e); at = e.body + e.size) {
            if (e.id == 0x4660)
                mime = b.mid(e.body, std::min<quint64>(e.size, 64));
            else if (e.id == 0x465C)
                data = e;
        }
        if (info.cover.isEmpty() && mime.startsWith("image/") && data.size > 0 && data.size < 16 * 1024 * 1024)
            info.cover = b.mid(data.body, data.size);
    }

    void tag(const Element &simple)
    {
        QString name, value;
        Element e;
        for (quint64 at = simple.body, end = simple.body + simple.size; at < end && element(b, at, end, e); at = e.body + e.size) {
            if (e.id == 0x45A3)
                name = QString::fromUtf8(b.mid(e.body, std::min<quint64>(e.size, 64))).toUpper();
            else if (e.id == 0x4487)
                value = clean(QString::fromUtf8(b.mid(e.body, std::min<quint64>(e.size, 1024))));
        }
        if (name == QLatin1String("TITLE") && info.title.isEmpty())
            info.title = value;
        else if ((name == QLatin1String("ARTIST") || name == QLatin1String("DIRECTOR")) && info.artist.isEmpty())
            info.artist = value;
        else if (name == QLatin1String("ALBUM") && info.album.isEmpty())
            info.album = value;
        else if ((name == QLatin1String("DATE_RELEASED") || name == QLatin1String("DATE_RECORDED")) && info.year.isEmpty())
            info.year = value.left(4);
    }
};

void readMatroska(const Bytes &b, Info &info)
{
    MatroskaWalk walk(b, info);
    Element header;
    if (!element(b, 0, b.size(), header) || header.id != 0x1A45DFA3)
        return;
    walk.walk(header.body + header.size, b.size(), 0);
    if (walk.duration > 0)
        info.durationMs = qint64(std::min(walk.duration * double(walk.timecodeScale) / 1e6, 1e12));
    info.codec = !walk.videoCodec.isEmpty() ? walk.videoCodec : walk.audioCodec;
}

// --- Windows Media (ASF): objects named by GUIDs in the header

void readAsf(const Bytes &b, Info &info)
{
    static const QByteArray header("\x30\x26\xB2\x75\x8E\x66\xCF\x11\xA6\xD9\x00\xAA\x00\x62\xCE\x6C", 16);
    static const QByteArray fileProperties("\xA1\xDC\xAB\x8C\x47\xA9\xCF\x11\x8E\xE4\x00\xC0\x0C\x20\x53\x65", 16);
    static const QByteArray streamProperties("\x91\x07\xDC\xB7\xB7\xA9\xCF\x11\x8E\xE6\x00\xC0\x0C\x20\x53\x65", 16);
    static const QByteArray contentDescription("\x33\x26\xB2\x75\x8E\x66\xCF\x11\xA6\xD9\x00\xAA\x00\x62\xCE\x6C", 16);
    static const QByteArray extendedDescription("\x40\xA4\xD0\xD2\x07\xE3\xD2\x11\x97\xF0\x00\xA0\xC9\x5E\xA8\x50", 16);
    static const QByteArray video("\xC0\xEF\x19\xBC\x4D\x5B\xCF\x11\xA8\xFD\x00\x80\x5F\x5C\x44\x2B", 16);
    static const QByteArray audio("\x40\x9E\x69\xF8\x4D\x5B\xCF\x11\xA8\xFD\x00\x80\x5F\x5C\x44\x2B", 16);
    if (b.mid(0, 16) != header)
        return;
    const quint64 headerSize = std::min<quint64>(b.le64(16), b.size());
    const quint32 objects = std::min<quint32>(b.le32(24), 1000);
    const auto utf16 = [&b](quint64 at, quint64 bytes) {
        const QByteArray raw = b.mid(at, std::min<quint64>(bytes, 4096));
        return clean(QString::fromUtf16(reinterpret_cast<const char16_t *>(raw.constData()), raw.size() / 2));
    };
    QString videoCodec, audioCodec;
    quint64 at = 30;
    for (quint32 i = 0; i < objects && at + 24 <= headerSize; ++i) {
        const QByteArray guid = b.mid(at, 16);
        const quint64 size = b.le64(at + 16);
        if (size < 24)
            break;
        if (guid == fileProperties) {
            const quint64 play = b.le64(at + 64), preroll = b.le64(at + 80);
            if (play / 10000 > preroll)
                info.durationMs = qint64(play / 10000 - preroll);
            const quint64 created = b.le64(at + 48);
            const QDateTime t = QDateTime(QDate(1601, 1, 1), QTime(0, 0), QTimeZone::UTC).addMSecs(qint64(created / 10000));
            if (created && t.date().year() > 1990 && t.date().year() < 2200)
                info.recorded = t;
        } else if (guid == streamProperties) {
            const QByteArray kind = b.mid(at + 24, 16);
            const quint64 data = at + 78;
            if (kind == video && videoCodec.isEmpty()) {
                info.width = int(std::min<quint32>(b.le32(data), 100000));
                info.height = int(std::min<quint32>(b.le32(data + 4), 100000));
                const QByteArray fourcc = b.mid(data + 11 + 16, 4);
                videoCodec = fourcc == "WMV3" ? QStringLiteral("WMV 9") : fourcc == "WVC1" ? QStringLiteral("VC-1") : QString::fromLatin1(fourcc).trimmed();
            } else if (kind == audio && audioCodec.isEmpty()) {
                const quint32 tag = b.le16(data);
                info.channels = int(std::min<quint32>(b.le16(data + 2), 64));
                info.sampleRate = int(std::min<quint32>(b.le32(data + 4), 10000000));
                audioCodec = tag == 0x0161 ? QStringLiteral("WMA") : tag == 0x0162 ? QStringLiteral("WMA Pro") : tag == 0x0163 ? QStringLiteral("WMA Lossless")
                           : tag == 0x000A ? QStringLiteral("WMA Voice") : tag == 0x0055 ? QStringLiteral("MP3") : tag == 0x0001 ? QStringLiteral("PCM")
                                                                                                                                  : QString();
            }
        } else if (guid == contentDescription) {
            const quint64 title = b.le16(at + 24), author = b.le16(at + 26);
            info.title = utf16(at + 34, title);
            info.artist = utf16(at + 34 + title, author);
        } else if (guid == extendedDescription) {
            quint64 p = at + 26;
            const quint32 count = std::min<quint32>(b.le16(at + 24), 500);
            for (quint32 k = 0; k < count && p + 6 <= at + size; ++k) {
                const quint64 nameLength = b.le16(p);
                const QString name = utf16(p + 2, nameLength);
                const quint32 type = b.le16(p + 2 + nameLength);
                const quint64 valueLength = b.le16(p + 4 + nameLength), value = p + 6 + nameLength;
                if (type == 0 && name == QLatin1String("WM/AlbumTitle"))
                    info.album = utf16(value, valueLength);
                else if (type == 0 && (name == QLatin1String("WM/Year") || name == QLatin1String("date"))) // "date": ffmpeg's
                    info.year = utf16(value, valueLength).left(4);
                else if (type == 0 && name == QLatin1String("WM/AlbumArtist") && info.artist.isEmpty())
                    info.artist = utf16(value, valueLength);
                else if (type == 1 && name == QLatin1String("WM/Picture") && info.cover.isEmpty()) {
                    // A kind byte, its size, its MIME type and words (each ending in a 0), then it.
                    quint64 q = value + 5;
                    for (int strings = 0; strings < 2; ++strings) {
                        while (q + 2 <= value + valueLength && b.le16(q) != 0)
                            q += 2;
                        q += 2;
                    }
                    const quint32 pictureSize = b.le32(value + 1);
                    if (q + pictureSize <= value + valueLength)
                        info.cover = b.mid(q, pictureSize);
                }
                p = value + valueLength;
            }
        }
        at += size;
    }
    info.codec = !videoCodec.isEmpty() ? videoCodec : audioCodec;
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
    } else if (id == QLatin1String("flac")) {
        readFlac(b, info);
    } else if (id == QLatin1String("mkv") || id == QLatin1String("webm")) {
        readMatroska(b, info);
    } else if (id == QLatin1String("wmv") || id == QLatin1String("wma")) {
        readAsf(b, info);
    }
    return info;
}

bool parseIso6709(const QString &text, double *latitude, double *longitude, double *altitude, bool *hasAltitude)
{
    static const QRegularExpression form(QStringLiteral("^\\s*([+-]\\d{1,2}(?:\\.\\d+)?)([+-]\\d{1,3}(?:\\.\\d+)?)([+-]\\d+(?:\\.\\d+)?)?"));
    const QRegularExpressionMatch m = form.match(text);
    if (!m.hasMatch())
        return false;
    const double lat = m.captured(1).toDouble(), lon = m.captured(2).toDouble();
    if (std::abs(lat) > 90 || std::abs(lon) > 180 || (lat == 0 && lon == 0))
        return false;
    *latitude = lat;
    *longitude = lon;
    *hasAltitude = m.hasCaptured(3);
    *altitude = *hasAltitude ? m.captured(3).toDouble() : 0;
    return true;
}

QStringList facts(const Info &info)
{
    QStringList out;
    const auto add = [&out](const QString &label, const QString &value) {
        if (!value.trimmed().isEmpty())
            out << label + QLatin1Char('\t') + value.trimmed();
    };
    if (info.made.isValid())
        add(tr("Recorded"), QLocale().toString(info.made, QLocale::LongFormat));
    else if (info.recorded.isValid())
        add(tr("Recorded"), QLocale().toString(info.recorded.toLocalTime(), QLocale::LongFormat));
    const QString device = info.model.startsWith(info.make, Qt::CaseInsensitive) || info.make.isEmpty() ? info.model
                         : info.model.isEmpty() ? info.make : info.make + QLatin1Char(' ') + info.model;
    add(tr("Phone or camera"), device);
    add(tr("Made with"), info.software);
    if (info.hasLocation)
        out += places::facts(info.latitude, info.longitude, info.hasAltitude, info.altitude);
    return out;
}

QByteArray mp4Moov(const Reader &read, quint64 size, quint64 *where)
{
    // The top-level boxes, one after another: just their headers are read.
    quint64 at = 0;
    for (int n = 0; n < 1000 && at + 8 <= size; ++n) {
        char header[16];
        if (read(at, header, 16) < 8)
            return {};
        const auto be32 = [&header](int i) { return quint32(quint8(header[i])) << 24 | quint32(quint8(header[i + 1])) << 16 | quint32(quint8(header[i + 2])) << 8 | quint8(header[i + 3]); };
        quint64 boxSize = be32(0);
        if (boxSize == 1)
            boxSize = quint64(be32(8)) << 32 | be32(12);
        else if (boxSize == 0)
            boxSize = size - at;
        if (boxSize < 8 || boxSize > size - at)
            return {};
        if (QByteArrayView(header + 4, 4) == QByteArrayView("moov")) {
            if (boxSize > 64ull * 1024 * 1024)
                return {};
            QByteArray moov(qsizetype(boxSize), Qt::Uninitialized);
            if (where)
                *where = at;
            return read(at, moov.data(), moov.size()) == moov.size() ? moov : QByteArray();
        }
        at += boxSize;
    }
    return {};
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
