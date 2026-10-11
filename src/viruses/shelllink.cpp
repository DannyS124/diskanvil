// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shelllink.h"

#include <QtEndian>

#include <cstring>

namespace shelllink {

namespace {

constexpr quint32 kHeaderSize = 0x4C;
constexpr quint32 kHasIdList = 0x1, kHasLinkInfo = 0x2, kHasName = 0x4, kHasRelativePath = 0x8, kHasWorkingDir = 0x10,
                  kHasArguments = 0x20, kHasIconLocation = 0x40, kIsUnicode = 0x80;
constexpr quint32 kEnvironmentBlock = 0xA0000001;

// The class every shortcut names: 00021401-0000-0000-C000-000000000046.
const char kClassId[16] = {0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, char(0xC0), 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};

struct Reader {
    const QByteArray &bytes;
    qsizetype at = 0;
    bool ok = true;

    bool has(qsizetype n) const { return ok && n >= 0 && at + n <= bytes.size(); }
    quint16 u16(qsizetype pos) const { return qFromLittleEndian<quint16>(bytes.constData() + pos); }
    quint32 u32(qsizetype pos) const { return qFromLittleEndian<quint32>(bytes.constData() + pos); }

    // A NUL-ended string inside [from, end), ANSI or UTF-16.
    QString cString(qsizetype from, qsizetype end, bool wide) const
    {
        QString out;
        if (from < 0 || end > bytes.size())
            return out;
        if (wide) {
            for (qsizetype i = from; i + 1 < end; i += 2) {
                const quint16 c = u16(i);
                if (c == 0)
                    break;
                out += QChar(char16_t(c));
            }
        } else {
            for (qsizetype i = from; i < end && bytes[i] != '\0'; ++i)
                out += QChar::fromLatin1(bytes[i]); // the ANSI code page isn't known; Latin-1 is close
        }
        return out;
    }

    // StringData: a count of characters, then the characters.
    QString counted(bool wide)
    {
        if (!has(2)) {
            ok = false;
            return {};
        }
        const quint16 count = u16(at);
        at += 2;
        const qsizetype size = wide ? 2 * qsizetype(count) : count;
        if (!has(size)) {
            ok = false;
            return {};
        }
        QString out;
        for (quint16 i = 0; i < count; ++i)
            out += wide ? QChar(char16_t(u16(at + 2 * i))) : QChar::fromLatin1(bytes[at + i]);
        at += size;
        return out;
    }
};

} // namespace

bool parse(const QByteArray &bytes, Link *link)
{
    *link = {};
    Reader r{bytes};
    if (!r.has(kHeaderSize) || r.u32(0) != kHeaderSize || std::memcmp(bytes.constData() + 4, kClassId, 16) != 0)
        return false;
    const quint32 flags = r.u32(0x14);
    const bool wide = flags & kIsUnicode;
    r.at = kHeaderSize;
    if (flags & kHasIdList) {
        if (!r.has(2))
            return false;
        r.at += 2 + r.u16(r.at); // the shell's own item list: the LinkInfo below says the same plainer
    }
    if (flags & kHasLinkInfo) {
        if (!r.has(28))
            return false;
        const qsizetype info = r.at;
        const quint32 size = r.u32(info), headerSize = r.u32(info + 4), infoFlags = r.u32(info + 8);
        if (size < 28 || !r.has(size))
            return false;
        const qsizetype end = info + size;
        if (infoFlags & 0x1) { // VolumeIDAndLocalBasePath
            QString base, suffix;
            if (headerSize >= 0x24 && r.has(36)) {
                base = r.cString(info + r.u32(info + 28), end, true);
                suffix = r.cString(info + r.u32(info + 32), end, true);
            }
            if (base.isEmpty()) {
                base = r.cString(info + r.u32(info + 16), end, false);
                suffix = r.cString(info + r.u32(info + 24), end, false);
            }
            link->target = base + suffix;
        }
        r.at = end;
    }
    QString name;
    if (flags & kHasName)
        name = r.counted(wide);
    if (flags & kHasRelativePath)
        link->relativePath = r.counted(wide);
    if (flags & kHasWorkingDir)
        link->workingDir = r.counted(wide);
    if (flags & kHasArguments)
        link->arguments = r.counted(wide);
    if (flags & kHasIconLocation)
        r.counted(wide);
    // Extra blocks: the target with its environment variables still in it ("%windir%\...")
    // is what the shortcut was made with, and what's there on another PC too.
    while (r.ok && r.has(8)) {
        const quint32 size = r.u32(r.at), signature = r.u32(r.at + 4);
        if (size < 8 || !r.has(size))
            break;
        if (signature == kEnvironmentBlock && size >= 8 + 260 + 520) {
            QString target = r.cString(r.at + 8 + 260, r.at + 8 + 260 + 520, true);
            if (target.isEmpty())
                target = r.cString(r.at + 8, r.at + 8 + 260, false);
            if (!target.isEmpty())
                link->target = target;
        }
        r.at += size;
    }
    return !link->target.isEmpty() || !link->relativePath.isEmpty();
}

} // namespace shelllink
