// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "zstdseek.h"

#include <QtEndian>

namespace zstdseek {

namespace {

constexpr quint32 kSkippableMagic = 0x184D2A5E;
constexpr quint32 kSeekableMagic = 0x8F92EAB1;

void put32(QByteArray &out, quint32 v)
{
    char b[4];
    qToLittleEndian<quint32>(v, b);
    out.append(b, 4);
}

} // namespace

QByteArray table(const QVector<QPair<quint32, quint32>> &frames)
{
    // A skippable frame: its magic and size, then an entry for each frame, then the footer
    // (how many, a descriptor saying there are no checksums here, the seekable magic).
    QByteArray entries;
    for (const auto &[compressed, size] : frames) {
        put32(entries, compressed);
        put32(entries, size);
    }
    QByteArray out;
    put32(out, kSkippableMagic);
    put32(out, quint32(entries.size() + 9));
    out += entries;
    put32(out, quint32(frames.size()));
    out.append(char(0));
    put32(out, kSeekableMagic);
    return out;
}

std::optional<QVector<Frame>> readTable(quint64 size, const std::function<bool(quint64, char *, quint64)> &read)
{
    char footer[9];
    if (size < 17 || !read(size - 9, footer, 9) || qFromLittleEndian<quint32>(footer + 5) != kSeekableMagic)
        return std::nullopt;
    const quint32 count = qFromLittleEndian<quint32>(footer);
    const bool checksums = uchar(footer[4]) & 0x80;
    const quint64 entry = checksums ? 12 : 8;
    const quint64 entries = quint64(count) * entry;
    if (count == 0 || entries + 17 > size || count > 100000000)
        return std::nullopt;
    QByteArray buf(qsizetype(entries + 8), '\0');
    if (!read(size - 9 - entries - 8, buf.data(), entries + 8) || qFromLittleEndian<quint32>(buf.constData()) != kSkippableMagic
        || qFromLittleEndian<quint32>(buf.constData() + 4) != quint32(entries + 9))
        return std::nullopt;
    QVector<Frame> out;
    out.reserve(count);
    quint64 compressed = 0, offset = 0;
    for (quint32 i = 0; i < count; ++i) {
        const char *e = buf.constData() + 8 + i * entry;
        Frame f;
        f.compressedOffset = compressed;
        f.compressedSize = qFromLittleEndian<quint32>(e);
        f.offset = offset;
        f.size = qFromLittleEndian<quint32>(e + 4);
        compressed += f.compressedSize;
        offset += f.size;
        out.push_back(f);
    }
    // The frames have to end where the table starts.
    if (compressed != size - 9 - entries - 8)
        return std::nullopt;
    return out;
}

} // namespace zstdseek
