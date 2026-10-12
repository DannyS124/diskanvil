// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pictures.h"

#include "filetypes.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QImageReader>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace pictures {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("pictures", text);
}

QString idOf(int type)
{
    return type >= 0 && type < filetypes::types().size() ? filetypes::types()[type].id : QString();
}

QImage fitted(QImage image, int pixels)
{
    if (!image.isNull() && pixels > 0 && (image.width() > pixels || image.height() > pixels))
        image = image.scaled(pixels, pixels, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

// With Qt's reader for that format only (or the one the bytes say, when there's no format).
QImage readWith(const QByteArray &data, const QByteArray &format, int pixels, QString *why)
{
    QByteArray copy = data; // shared, not copied
    QBuffer buffer(&copy);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, format);
    reader.setDecideFormatFromContent(format.isEmpty());
    reader.setAutoDetectImageFormat(format.isEmpty());
    reader.setAutoTransform(true); // the way the camera was held
    const QSize full = reader.size();
    if (full.isValid() && pixels > 0 && (full.width() > pixels || full.height() > pixels))
        reader.setScaledSize(full.scaled(QSize(pixels, pixels), Qt::KeepAspectRatio));
    const QImage image = reader.read();
    if (image.isNull() && why)
        *why = reader.errorString();
    return fitted(image, pixels);
}

// A view of bytes that never reads past their end.
struct Bytes {
    const uchar *p;
    quint64 n;
    explicit Bytes(const QByteArray &d)
        : p(reinterpret_cast<const uchar *>(d.constData()))
        , n(quint64(d.size()))
    {
    }
    bool has(quint64 at, quint64 len) const { return at <= n && len <= n - at; }
    quint32 be16(quint64 at) const { return has(at, 2) ? qFromBigEndian<quint16>(p + at) : 0; }
    quint32 be32(quint64 at) const { return has(at, 4) ? qFromBigEndian<quint32>(p + at) : 0; }
    quint64 be64(quint64 at) const { return has(at, 8) ? qFromBigEndian<quint64>(p + at) : 0; }
};

// PackBits, as Photoshop packs each row: into `out` (its size is the row's), false when the
// row's bytes don't add up.
bool unpackRow(const uchar *in, quint64 inSize, uchar *out, quint64 outSize)
{
    quint64 i = 0, o = 0;
    while (o < outSize && i < inSize) {
        const int c = qint8(in[i++]);
        if (c >= 0) {
            const quint64 run = quint64(c) + 1;
            if (i + run > inSize || o + run > outSize)
                return false;
            std::memcpy(out + o, in + i, run);
            i += run;
            o += run;
        } else if (c != -128) {
            const quint64 run = quint64(1 - c);
            if (i >= inSize || o + run > outSize)
                return false;
            std::memset(out + o, in[i++], run);
            o += run;
        }
    }
    return o == outSize;
}

// The small JPEG Photoshop keeps of its picture (image resource 1036, or 1033 in old files).
QImage psdThumbnail(const Bytes &b, quint64 at, quint64 end, int pixels)
{
    while (at + 12 <= end && b.has(at, 12) && std::memcmp(b.p + at, "8BIM", 4) == 0) {
        const quint32 id = b.be16(at + 4);
        quint64 name = 1 + (b.has(at + 6, 1) ? b.p[at + 6] : 0);
        name += name & 1;
        const quint64 sizeAt = at + 6 + name;
        const quint64 size = b.be32(sizeAt), data = sizeAt + 4;
        if (!b.has(data, size))
            break;
        if ((id == 1036 || id == 1033) && size > 28) {
            const QImage image = readWith(QByteArray(reinterpret_cast<const char *>(b.p + data + 28), qsizetype(size - 28)), "jpeg", pixels, nullptr);
            if (!image.isNull()) // 1033 keeps its colours the other way round
                return id == 1033 ? image.rgbSwapped() : image;
        }
        at = data + size + (size & 1);
    }
    return {};
}

} // namespace

QByteArray readerFormat(const QString &id)
{
    if (id == QLatin1String("jpg"))
        return "jpeg";
    if (id == QLatin1String("heic"))
        return "heif";
    if (id == QLatin1String("tif"))
        return "tiff";
    if (id == QLatin1String("png") || id == QLatin1String("gif") || id == QLatin1String("bmp") || id == QLatin1String("webp")
        || id == QLatin1String("avif"))
        return id.toLatin1();
    return {};
}

bool isRaw(const QString &id)
{
    static const QStringList raw = {QStringLiteral("cr2"), QStringLiteral("cr3"), QStringLiteral("nef"), QStringLiteral("arw"),
                                    QStringLiteral("dng"), QStringLiteral("orf"), QStringLiteral("rw2"), QStringLiteral("pef"),
                                    QStringLiteral("raf")};
    return raw.contains(id);
}

QByteArray embeddedJpeg(const QByteArray &data)
{
    struct Candidate {
        qsizetype at;
        qint64 area;
    };
    QVector<Candidate> found;
    for (qsizetype at = data.indexOf("\xFF\xD8\xFF"); at >= 0 && found.size() < 64; at = data.indexOf("\xFF\xD8\xFF", at + 3)) {
        const QByteArray rest = QByteArray::fromRawData(data.constData() + at, data.size() - at);
        QByteArray copy = rest;
        QBuffer buffer(&copy);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "jpeg");
        reader.setDecideFormatFromContent(false);
        const QSize size = reader.size();
        if (size.isValid() && size.width() >= 32 && size.height() >= 32)
            found << Candidate{at, qint64(size.width()) * size.height()};
    }
    std::stable_sort(found.begin(), found.end(), [](const Candidate &a, const Candidate &c) { return a.area > c.area; });
    for (const Candidate &c : std::as_const(found)) {
        const QByteArray rest = QByteArray::fromRawData(data.constData() + c.at, data.size() - c.at);
        if (!readWith(rest, "jpeg", 64, nullptr).isNull()) // decodes (scaled small: quick)
            return data.mid(c.at);
    }
    return {};
}

QImage psd(const QByteArray &data, int pixels)
{
    const Bytes b(data);
    if (!b.has(0, 26) || std::memcmp(b.p, "8BPS", 4) != 0)
        return {};
    const quint32 version = b.be16(4), channels = b.be16(12), height = b.be32(14), width = b.be32(18), depth = b.be16(22),
                  mode = b.be16(24);
    if ((version != 1 && version != 2) || channels == 0 || channels > 56 || width == 0 || height == 0 || width > 300000 || height > 300000)
        return {};
    quint64 at = 26;
    const quint64 colours = at + 4, colourSize = b.be32(at);
    at = colours + colourSize;
    const quint64 resources = at + 4, resourceSize = b.be32(at);
    at = resources + resourceSize;
    at += version == 1 ? 4 + quint64(b.be32(at)) : 8 + b.be64(at);
    const quint32 compression = b.be16(at);
    at += 2;
    // The colour channels it has: 1 for grey, bitmaps and indexed, 3 for RGB, 4 for CMYK.
    const quint32 needed = mode == 3 ? 3 : mode == 4 ? 4 : 1;
    const bool known = mode == 0 || mode == 1 || mode == 3 || mode == 4 || (mode == 2 && colourSize >= 768 && b.has(colours, 768));
    const bool readable = known && channels >= needed && (depth == 1 ? mode == 0 : (depth == 8 || depth == 16) && mode != 0)
                       && (compression == 0 || compression == 1);
    const quint64 rows = quint64(channels) * height;
    const int countSize = version == 1 ? 2 : 4;
    if (!readable || !b.has(at, compression == 1 ? rows * countSize : 0))
        return psdThumbnail(b, resources, resources + resourceSize, pixels);
    // Every `step`th row and column, when it's to be shown smaller: rows can be jumped to.
    const quint32 longest = std::max(width, height);
    const quint32 step = pixels > 0 && longest > quint32(pixels) ? std::max<quint32>(1, longest / quint32(pixels)) : 1;
    const quint32 outW = std::max<quint32>(1, width / step), outH = std::max<quint32>(1, height / step);
    if (quint64(outW) * outH > 120'000'000ull) // too big to hold, even shown smaller
        return psdThumbnail(b, resources, resources + resourceSize, pixels);
    const quint64 rowBytes = depth == 1 ? (quint64(width) + 7) / 8 : quint64(width) * (depth / 8);
    // Where each row of the channels used starts (packed rows: after every row's byte count).
    const quint64 used = quint64(needed) * height;
    QVector<quint64> rowAt(qsizetype(used), 0), rowSize(qsizetype(used), 0);
    if (compression == 0) {
        for (quint64 r = 0; r < used; ++r)
            rowAt[qsizetype(r)] = at + r * rowBytes;
        std::fill(rowSize.begin(), rowSize.end(), rowBytes);
    } else {
        quint64 pos = at + rows * countSize;
        for (quint64 r = 0; r < used; ++r) {
            rowAt[qsizetype(r)] = pos;
            rowSize[qsizetype(r)] = countSize == 2 ? b.be16(at + r * 2) : b.be32(at + r * 4);
            pos += rowSize[qsizetype(r)];
        }
    }
    QImage out(int(outW), int(outH), QImage::Format_RGB32);
    QByteArray row(qsizetype(rowBytes), Qt::Uninitialized);
    QVector<QByteArray> planes(int(needed), QByteArray(int(outW), '\0'));
    const uchar *table = b.p + colours;
    for (quint32 y = 0; y < outH; ++y) {
        for (quint32 c = 0; c < needed; ++c) {
            const quint64 r = quint64(c) * height + quint64(y) * step;
            const quint64 from = rowAt[qsizetype(r)], size = rowSize[qsizetype(r)];
            uchar *dst = reinterpret_cast<uchar *>(row.data());
            bool ok = b.has(from, size);
            if (ok && compression == 0)
                std::memcpy(dst, b.p + from, rowBytes);
            else if (ok)
                ok = unpackRow(b.p + from, size, dst, rowBytes);
            if (!ok) { // cut short or damaged here: what was read is kept, the rest is left grey
                if (y == 0)
                    return psdThumbnail(b, resources, resources + resourceSize, pixels);
                out = out.copy(0, 0, int(outW), int(y));
                return fitted(out, pixels);
            }
            uchar *plane = reinterpret_cast<uchar *>(planes[int(c)].data());
            for (quint32 x = 0; x < outW; ++x) {
                const quint64 sx = quint64(x) * step;
                plane[x] = depth == 1 ? ((dst[sx / 8] >> (7 - sx % 8)) & 1 ? 0 : 255) : dst[sx * (depth / 8)]; // 16-bit: the high byte
            }
        }
        QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(int(y)));
        const auto *p0 = reinterpret_cast<const uchar *>(planes[0].constData());
        for (quint32 x = 0; x < outW; ++x) {
            switch (mode) {
            case 2: // indexed: reds, greens, blues, 256 of each
                line[x] = qRgb(table[p0[x]], table[256 + p0[x]], table[512 + p0[x]]);
                break;
            case 3:
                line[x] = qRgb(p0[x], uchar(planes[1][x]), uchar(planes[2][x]));
                break;
            case 4: { // CMYK, kept the other way round (255 is no ink)
                const int k = uchar(planes[3][x]);
                line[x] = qRgb(p0[x] * k / 255, uchar(planes[1][x]) * k / 255, uchar(planes[2][x]) * k / 255);
                break;
            }
            default:
                line[x] = qRgb(p0[x], p0[x], p0[x]);
                break;
            }
        }
    }
    return fitted(out, pixels);
}

QImage decode(const QByteArray &data, int type, int pixels, QString *why)
{
    const QString id = idOf(type);
    QString error;
    QImage image;
    if (id == QLatin1String("psd")) {
        image = psd(data, pixels);
        if (image.isNull())
            error = tr("its picture is in a form that can't be read here, or it's damaged");
    } else if (isRaw(id)) {
        // The JPEG the camera made of the photo: every maker puts one in, mostly full size.
        const QByteArray jpeg = embeddedJpeg(data);
        image = jpeg.isEmpty() ? QImage() : readWith(jpeg, "jpeg", pixels, &error);
        if (image.isNull() && id != QLatin1String("cr3") && id != QLatin1String("raf"))
            image = readWith(data, "tiff", pixels, &error); // some DNGs keep only a TIFF picture
        if (image.isNull())
            error = tr("the photo the camera put inside it couldn't be found, or it's damaged");
    } else {
        image = readWith(data, readerFormat(id), pixels, &error);
    }
    if (image.isNull() && why)
        *why = error;
    return image;
}

QSize size(const QByteArray &data, int type)
{
    const QString id = idOf(type);
    const Bytes b(data);
    if (id == QLatin1String("psd"))
        return b.has(0, 26) ? QSize(int(std::min<quint32>(b.be32(18), INT_MAX)), int(std::min<quint32>(b.be32(14), INT_MAX))) : QSize();
    if (isRaw(id))
        return {}; // the camera's own numbers (EXIF) say it better than its preview would
    QByteArray copy = data;
    QBuffer buffer(&copy);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, readerFormat(id));
    reader.setDecideFormatFromContent(readerFormat(id).isEmpty());
    return reader.size();
}

} // namespace pictures
