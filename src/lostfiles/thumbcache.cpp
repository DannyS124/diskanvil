// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thumbcache.h"

#include "ole.h"

#include <QByteArrayView>
#include <QHash>
#include <QRegularExpression>
#include <QtEndian>

#include <algorithm>

namespace thumbcache {

namespace {

quint32 le32(const QByteArray &d, qsizetype at)
{
    return at >= 0 && at + 4 <= d.size() ? qFromLittleEndian<quint32>(d.constData() + at) : 0;
}

// Where a picture starts in [from, to), and what kind it is.
qsizetype pictureStart(const QByteArray &d, qsizetype from, qsizetype to, QString *kind)
{
    const QByteArrayView range(d.constData() + from, to - from);
    const qsizetype jpeg = range.indexOf(QByteArrayView("\xFF\xD8\xFF", 3));
    const qsizetype png = range.indexOf(QByteArrayView("\x89PNG\r\n\x1a\n", 8));
    const qsizetype bmp = range.indexOf(QByteArrayView("BM", 2));
    qsizetype best = -1;
    for (const auto &[at, name] : {std::pair{jpeg, "jpg"}, std::pair{png, "png"}}) {
        if (at >= 0 && (best < 0 || at < best)) {
            best = at;
            *kind = QString::fromLatin1(name);
        }
    }
    // A BMP's header says how big it is: only one that adds up counts.
    if (bmp >= 0 && (best < 0 || bmp < best) && le32(d, from + bmp + 2) > 54 && from + bmp + le32(d, from + bmp + 2) <= to
        && le32(d, from + bmp + 14) >= 12 && le32(d, from + bmp + 14) <= 124) {
        best = bmp;
        *kind = QStringLiteral("bmp");
    }
    return best < 0 ? -1 : from + best;
}

// The picture's own end: a JPEG's EOI or a PNG's IEND (the entry can have padding after it).
qsizetype pictureEnd(const QByteArray &d, qsizetype start, qsizetype to, const QString &kind)
{
    if (kind == QLatin1String("png")) {
        const qsizetype end = QByteArrayView(d.constData() + start, to - start).indexOf(QByteArrayView("IEND\xAE\x42\x60\x82", 8));
        return end >= 0 ? start + end + 8 : to;
    }
    if (kind == QLatin1String("bmp"))
        return std::min<qsizetype>(to, start + qsizetype(le32(d, start + 2)));
    const qsizetype end = QByteArrayView(d.constData() + start, to - start).lastIndexOf(QByteArrayView("\xFF\xD9", 2));
    return end >= 0 ? start + end + 2 : to;
}

QVector<Thumb> thumbcacheFile(const QByteArray &d, int most)
{
    QVector<Thumb> out;
    // Entries one after another, each "CMMM" and its size; where a size doesn't add up, the next "CMMM".
    qsizetype at = d.indexOf("CMMM", 4);
    while (at >= 0 && at + 8 <= d.size() && out.size() < most) {
        const quint32 size = le32(d, at + 4);
        qsizetype end = size >= 24 && at + qsizetype(size) <= d.size() ? at + qsizetype(size) : -1;
        if (end < 0) {
            const qsizetype next = d.indexOf("CMMM", at + 4);
            end = next < 0 ? d.size() : next;
        }
        QString kind;
        const qsizetype start = pictureStart(d, at + 24, end, &kind);
        if (start >= 0) {
            const qsizetype stop = pictureEnd(d, start, end, kind);
            if (stop - start > 32)
                out.push_back({QString(), kind, quint64(start), QByteArray(), quint64(stop - start)});
        }
        const qsizetype next = d.indexOf("CMMM", std::max(end, at + 4));
        at = next;
    }
    return out;
}

QVector<Thumb> thumbsDb(const QByteArray &d, int most)
{
    QVector<Thumb> out;
    const ole::File file(d);
    if (!file.valid())
        return out;
    // The Catalog: a number and a name for each photo.
    QHash<quint32, QString> names;
    const QByteArray catalog = file.stream(QStringLiteral("Catalog"), 16 * 1024 * 1024);
    for (qsizetype at = le32(catalog, 0) & 0xFFFF; at + 16 <= catalog.size() && names.size() < most;) {
        const quint32 size = le32(catalog, at), id = le32(catalog, at + 4);
        if (size < 16 || at + qsizetype(size) > catalog.size())
            break;
        const QByteArray name = catalog.mid(at + 16, qsizetype(size) - 16);
        names.insert(id, QString::fromUtf16(reinterpret_cast<const char16_t *>(name.constData()), name.size() / 2).section(QChar(0), 0, 0));
        at += qsizetype(size);
    }
    static const QRegularExpression number(QStringLiteral("^\\d+$"));
    for (const ole::File::Entry &e : file.entries()) {
        if (e.storage || !number.match(e.path).hasMatch() || out.size() >= most)
            continue;
        const QByteArray stream = file.stream(e.path, 16 * 1024 * 1024);
        QString kind;
        const qsizetype start = pictureStart(stream, 0, stream.size(), &kind);
        if (start < 0)
            continue;
        const qsizetype stop = pictureEnd(stream, start, stream.size(), kind);
        Thumb t;
        // The stream is named by the photo's number written backwards ("01" is 10).
        QString digits = e.path;
        std::reverse(digits.begin(), digits.end());
        t.name = names.value(digits.toUInt());
        t.kind = kind;
        t.data = stream.mid(start, stop - start);
        t.size = quint64(t.data.size());
        out.push_back(t);
    }
    return out;
}

} // namespace

bool isCache(const QString &name)
{
    static const QRegularExpression cache(QStringLiteral("^(thumbcache_\\w+\\.db|thumbs\\.db|ehthumbs(_vista)?\\.db)$"), QRegularExpression::CaseInsensitiveOption);
    return cache.match(name).hasMatch();
}

QVector<Thumb> read(const QByteArray &file, int most)
{
    if (file.startsWith("CMMM"))
        return thumbcacheFile(file, most);
    if (file.startsWith("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1"))
        return thumbsDb(file, most);
    return {};
}

} // namespace thumbcache
