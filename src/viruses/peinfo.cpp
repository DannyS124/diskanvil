// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "peinfo.h"

#include <QtEndian>

#include <cstring>

namespace peinfo {

namespace {

quint16 u16(const QByteArray &d, qint64 at)
{
    return at >= 0 && at + 2 <= d.size() ? qFromLittleEndian<quint16>(d.constData() + at) : 0;
}

quint32 u32(const QByteArray &d, qint64 at)
{
    return at >= 0 && at + 4 <= d.size() ? qFromLittleEndian<quint32>(d.constData() + at) : 0;
}

struct Section {
    quint32 address = 0, size = 0, offset = 0;
};

// A UTF-16 string at `at`, up to its NUL or `most` characters; `used` is how many bytes it took
// with the NUL.
QString utf16(const QByteArray &d, qint64 at, qint64 most, qint64 *used = nullptr)
{
    QString out;
    qint64 i = 0;
    for (; i < most && at + 2 * i + 2 <= d.size(); ++i) {
        const quint16 c = u16(d, at + 2 * i);
        if (c == 0) {
            ++i;
            break;
        }
        out += QChar(c);
    }
    if (used)
        *used = 2 * i;
    return out;
}

qint64 align4(qint64 v)
{
    return (v + 3) & ~qint64(3);
}

// One block of the version resource (VS_VERSIONINFO, StringFileInfo, a string table, a
// string): its length, its value's, its type (1 text), its key, then its value and children.
void walk(const QByteArray &d, qint64 at, qint64 end, int depth, bool strings, QHash<QString, QString> &out)
{
    while (at + 6 <= end && depth < 6) {
        const qint64 length = u16(d, at), valueLength = u16(d, at + 2), type = u16(d, at + 4);
        if (length < 6 || at + length > end)
            return;
        qint64 keyBytes = 0;
        const QString key = utf16(d, at + 6, (length - 6) / 2, &keyBytes);
        const qint64 value = align4(at + 6 + keyBytes);
        const qint64 valueBytes = type == 1 ? valueLength * 2 : valueLength;
        if (depth == 3 && strings && type == 1)
            out.insert(key, utf16(d, value, valueLength).trimmed());
        const qint64 children = align4(value + valueBytes);
        if (depth < 3 && children < at + length)
            walk(d, children, at + length, depth + 1, strings || key == QLatin1String("StringFileInfo"), out);
        at = align4(at + length);
    }
}

} // namespace

QHash<QString, QString> versionStrings(const QByteArray &d)
{
    QHash<QString, QString> out;
    if (d.size() < 0x40 || !d.startsWith("MZ"))
        return out;
    const qint64 pe = u32(d, 0x3C);
    if (pe < 0x40 || pe + 24 > d.size() || std::memcmp(d.constData() + pe, "PE\0\0", 4) != 0)
        return out;
    const int sections = u16(d, pe + 6);
    const qint64 optional = pe + 24, optionalSize = u16(d, pe + 20);
    const quint16 magic = u16(d, optional);
    // The resource directory: data directory 2.
    const qint64 directories = optional + (magic == 0x20B ? 112 : 96);
    if (magic != 0x10B && magic != 0x20B)
        return out;
    const quint32 resourceRva = u32(d, directories + 2 * 8);
    QVector<Section> table;
    for (int i = 0; i < sections && i < 96; ++i) {
        const qint64 at = optional + optionalSize + i * 40;
        table.push_back({u32(d, at + 12), std::max(u32(d, at + 8), u32(d, at + 16)), u32(d, at + 20)});
    }
    const auto offsetOf = [&table](quint32 rva) -> qint64 {
        for (const Section &s : table) {
            if (rva >= s.address && rva < s.address + s.size)
                return qint64(rva - s.address) + s.offset;
        }
        return -1;
    };
    const qint64 root = offsetOf(resourceRva);
    if (root < 0)
        return out;
    // Down the tree: the type (16, RT_VERSION), then the first name, then the first language.
    qint64 dir = root;
    for (int level = 0; level < 3; ++level) {
        const int count = u16(d, dir + 12) + u16(d, dir + 14);
        qint64 next = -1;
        for (int i = 0; i < count && i < 4096; ++i) {
            const qint64 entry = dir + 16 + i * 8;
            const quint32 id = u32(d, entry), target = u32(d, entry + 4);
            if (level == 0 && id != 16)
                continue;
            next = root + (target & 0x7FFFFFFF);
            if (level < 2 && !(target & 0x80000000))
                return out; // a type or name with data straight under it: not what a version resource looks like
            break;
        }
        if (next < 0 || next >= d.size())
            return out;
        dir = next;
    }
    // The data entry: where the resource is, and how big.
    const qint64 data = offsetOf(u32(d, dir));
    const qint64 size = u32(d, dir + 4);
    if (data < 0 || size < 6 || data + size > d.size())
        return out;
    walk(d, data, data + size, 0, false, out);
    return out;
}

} // namespace peinfo
