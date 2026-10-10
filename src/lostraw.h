// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Reading the on-disk structures of file systems: little-endian numbers, UTF-16 names and
// bytes from the source, for the file system readers in lostfs.cpp and lostntfs.cpp.

#include "lostsource.h"

#include <QByteArray>
#include <QString>
#include <QtEndian>

#include <algorithm>

namespace lost::raw {

inline quint16 le16(const uchar *p)
{
    return qFromLittleEndian<quint16>(p);
}

inline quint32 le32(const uchar *p)
{
    return qFromLittleEndian<quint32>(p);
}

inline quint64 le64(const uchar *p)
{
    return qFromLittleEndian<quint64>(p);
}

inline QString utf16le(const uchar *p, int count)
{
    QString out;
    out.reserve(count);
    for (int i = 0; i < count; ++i)
        out += QChar(char16_t(le16(p + 2 * i)));
    return out;
}

inline const uchar *bytes(const QByteArray &data)
{
    return reinterpret_cast<const uchar *>(data.constData());
}

// `len` bytes at `at`, fewer at the end of the source.
inline QByteArray read(Source &source, quint64 at, quint64 len)
{
    if (at >= source.size())
        return {};
    len = std::min(len, source.size() - at);
    QByteArray data(qsizetype(len), Qt::Uninitialized);
    source.read(at, data.data(), len);
    return data;
}

} // namespace lost::raw
