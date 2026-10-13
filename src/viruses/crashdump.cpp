// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "crashdump.h"

#include <QFileInfo>
#include <QSet>
#include <QTimeZone>

#include <cstring>

namespace crashdump {

namespace {

constexpr quint32 kPage = 0x45474150; // "PAGE": what Windows fills unused fields with

template<typename T>
bool get(const QByteArray &f, qint64 at, T *out)
{
    if (at < 0 || at + qint64(sizeof(T)) > f.size())
        return false;
    std::memcpy(out, f.constData() + at, sizeof(T));
    return true;
}

template<typename T>
T get(const QByteArray &f, qint64 at)
{
    T v {};
    get(f, at, &v);
    return v;
}

QDateTime fileTime(quint64 t)
{
    if (t == 0 || t == 0x4547415045474150ULL)
        return {};
    return QDateTime::fromMSecsSinceEpoch(qint64(t / 10000) - 11644473600000LL, QTimeZone::UTC);
}

// A DUMP_STRING: u32 length in UTF-16 units, then the characters.
QString dumpString(const QByteArray &f, qint64 at)
{
    const quint32 n = get<quint32>(f, at);
    if (n == 0 || n > 1024 || at + 4 + 2 * qint64(n) > f.size())
        return {};
    QString s(int(n), Qt::Uninitialized);
    for (quint32 i = 0; i < n; ++i)
        s[int(i)] = QChar(get<quint16>(f, at + 4 + 2 * i));
    return s;
}

QString baseName(const QString &path)
{
    return path.section(QLatin1Char('\\'), -1);
}

} // namespace

Dump read(const QByteArray &f)
{
    Dump d;
    if (f.size() < 0x1000 || !f.startsWith("PAGE")) {
        d.problem = QStringLiteral("not a Windows crash dump");
        return d;
    }
    const QByteArray valid = f.mid(4, 4);
    if (valid == "DU64") {
        if (f.size() < 0x2000) {
            d.problem = QStringLiteral("cut short");
            return d;
        }
        d.build = get<quint32>(f, 0x0c);
        const quint32 machine = get<quint32>(f, 0x30);
        d.machine = machine == 0x8664 ? QStringLiteral("x64") : machine == 0xaa64 ? QStringLiteral("ARM64") : QStringLiteral("0x%1").arg(machine, 0, 16);
        d.code = get<quint32>(f, 0x38);
        for (int i = 0; i < 4; ++i)
            d.parameters[size_t(i)] = get<quint64>(f, 0x40 + 8 * i);
        d.type = int(get<quint32>(f, 0xf98));
        d.time = fileTime(get<quint64>(f, 0xfa8));
        if (machine == 0x8664) {
            const quint64 rip = get<quint64>(f, 0x348 + 0xf8);
            if (rip != 0x4547415045474150ULL)
                d.instruction = rip;
        }
    } else if (valid == "DUMP") {
        // The 32-bit one.
        d.build = get<quint32>(f, 0x0c);
        d.machine = QStringLiteral("x86");
        d.code = get<quint32>(f, 0x28);
        for (int i = 0; i < 4; ++i)
            d.parameters[size_t(i)] = get<quint32>(f, 0x2c + 4 * i);
        d.type = int(get<quint32>(f, 0xf88));
        d.time = fileTime(get<quint64>(f, 0xfc0));
    } else {
        d.problem = QStringLiteral("not a Windows crash dump");
        return d;
    }
    d.ok = true;
    if (valid != "DU64" || d.type != 4)
        return d;

    // A small dump: TRIAGE_DUMP64 at 0x2000, its offsets from the start of the file.
    constexpr qint64 t = 0x2000;
    const quint32 validOffset = get<quint32>(f, t + 0x08);
    d.complete = validOffset > 0 && get<quint32>(f, validOffset) == 0x44475254; // "TRGD"
    const quint32 driverList = get<quint32>(f, t + 0x30), driverCount = get<quint32>(f, t + 0x34);
    for (quint32 i = 0; i < std::min<quint32>(driverCount, 4096); ++i) {
        const qint64 entry = qint64(driverList) + 0x90 * qint64(i);
        if (entry + 0x90 > f.size())
            break;
        Driver drv;
        drv.path = dumpString(f, get<quint32>(f, entry));
        drv.name = baseName(drv.path);
        drv.base = get<quint64>(f, entry + 0x38);
        drv.size = get<quint32>(f, entry + 0x48);
        if (!drv.name.isEmpty() && drv.size > 0)
            d.drivers << drv;
    }
    const quint32 unloadedAt = get<quint32>(f, t + 0x18);
    if (unloadedAt > 0) {
        const quint32 count = get<quint32>(f, unloadedAt);
        for (quint32 i = 0; i < std::min<quint32>(count, 256); ++i) {
            const qint64 e = qint64(unloadedAt) + 8 + 0x38 * qint64(i);
            if (e + 0x38 > f.size())
                break;
            const int bytes = std::min<int>(get<quint16>(f, e), 24);
            QString name;
            for (int c = 0; c < bytes / 2; ++c) {
                const quint16 ch = get<quint16>(f, e + 0x10 + 2 * c);
                if (ch == 0)
                    break;
                name += QChar(ch);
            }
            Driver u;
            u.name = name;
            u.base = get<quint64>(f, e + 0x28);
            const quint64 end = get<quint64>(f, e + 0x30);
            u.size = end > u.base ? end - u.base : 0;
            if (!name.isEmpty() && u.size > 0)
                d.unloaded << u;
        }
    }
    // The crashing thread's stack: the drivers its words point into, nearest the top first.
    // Without the drivers' unwind data this can't tell a real return address from an old one,
    // so it's said as "seen on the stack", never as the cause.
    const quint32 stack = get<quint32>(f, t + 0x28), stackSize = get<quint32>(f, t + 0x2c);
    const quint64 top = get<quint64>(f, t + 0x48);
    const quint64 rsp = valid == "DU64" ? get<quint64>(f, 0x348 + 0x98) : 0;
    if (stack > 0 && stackSize > 0 && qint64(stack) + stackSize <= f.size()) {
        qint64 from = 0;
        if (rsp >= top && rsp < top + stackSize)
            from = qint64(rsp - top);
        QSet<QString> listed;
        for (qint64 at = from & ~7; at + 8 <= stackSize && d.onStack.size() < 8; at += 8) {
            const quint64 word = get<quint64>(f, stack + at);
            for (const Driver &drv : d.drivers) {
                if (word >= drv.base && word < drv.base + drv.size) {
                    if (!listed.contains(drv.name)) {
                        listed.insert(drv.name);
                        d.onStack << drv.name;
                    }
                    break;
                }
            }
        }
    }
    return d;
}

QString where(const Dump &dump, quint64 address, bool *unloaded)
{
    if (unloaded)
        *unloaded = false;
    if (address == 0)
        return {};
    for (const Driver &drv : dump.drivers)
        if (address >= drv.base && address < drv.base + drv.size)
            return QStringLiteral("%1+0x%2").arg(drv.name).arg(address - drv.base, 0, 16);
    for (const Driver &drv : dump.unloaded) {
        if (address >= drv.base && address < drv.base + drv.size) {
            if (unloaded)
                *unloaded = true;
            return QStringLiteral("%1+0x%2").arg(drv.name).arg(address - drv.base, 0, 16);
        }
    }
    return {};
}

} // namespace crashdump
