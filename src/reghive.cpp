// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "reghive.h"

#include "blockio.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimeZone>
#include <QtEndian>

#include <cstring>

// The layout, as documented by Maxim Suhanov's "Windows registry file format specification"
// and libyal's notes on it:
//
// - A 4096-byte base block: "regf", two sequence numbers (equal when Windows closed the hive
//   properly), the root key's cell, the size of the hive bins, a checksum over its first 508
//   bytes.
// - Hive bins ("hbin", 4096 bytes or more each) full of cells. A cell starts with its size,
//   negative while it's in use; cells point at each other by their offset from the first bin.
// - Cells: "nk" a key, "vk" a value, "lf"/"lh"/"li"/"ri" lists of subkeys, a plain list of
//   values, "db" a big value's list of pieces, "sk" security (not needed here).
// - A log (.LOG1/.LOG2, Windows 8.1 and later): a 512-byte copy of the base block, then
//   "HvLE" entries, each a numbered set of whole 4096-byte pages of the hive bins, checked
//   with Marvin32 hashes.

namespace reg {

namespace {

constexpr qsizetype kBase = 4096;          // the base block; the hive bins follow
constexpr quint32 kNoCell = 0xFFFFFFFF;
constexpr quint32 kSegment = 16344;        // a big value's pieces
constexpr quint32 kMostData = 64u << 20;   // more than any real value; a damaged size isn't believed
constexpr int kMostKeys = 4 << 20;
constexpr quint16 kKeyCompressedName = 0x0020;
constexpr quint16 kValueCompressedName = 0x0001;

quint16 u16(const char *p)
{
    return qFromLittleEndian<quint16>(p);
}

quint32 u32(const char *p)
{
    return qFromLittleEndian<quint32>(p);
}

quint64 u64(const char *p)
{
    return qFromLittleEndian<quint64>(p);
}

void put32(char *p, quint32 v)
{
    qToLittleEndian<quint32>(v, p);
}

quint32 rotl(quint32 x, int n)
{
    return (x << n) | (x >> (32 - n));
}

// XOR of the first 127 numbers, never 0 or -1.
quint32 checksum(const char *base)
{
    quint32 sum = 0;
    for (int i = 0; i < 127; ++i)
        sum ^= u32(base + 4 * i);
    if (sum == 0xFFFFFFFF)
        return 0xFFFFFFFE;
    return sum == 0 ? 1 : sum;
}

bool validBase(const QByteArray &d)
{
    return d.size() >= 512 && d.startsWith("regf") && checksum(d.constData()) == u32(d.constData() + 508);
}

QString utf16(const char *p, qsizetype bytes)
{
    QString out;
    out.reserve(bytes / 2);
    for (qsizetype i = 0; i + 1 < bytes; i += 2)
        out += QChar(char16_t(u16(p + i)));
    return out;
}

QString recordName(const char *p, qsizetype bytes, bool compressed)
{
    return compressed ? QString::fromLatin1(p, bytes) : utf16(p, bytes);
}

QDateTime fromFiletime(quint64 t)
{
    if (t == 0)
        return {};
    return QDateTime::fromMSecsSinceEpoch(qint64(t / 10000) - 11644473600000LL, QTimeZone::UTC);
}

quint64 filetimeNow()
{
    return quint64(QDateTime::currentMSecsSinceEpoch() + 11644473600000LL) * 10000;
}

struct LogEntry {
    quint32 sequence = 0;
    quint32 binsSize = 0;
    QVector<QPair<quint32, quint32>> pages; // offset in the hive bins, size
    const char *data = nullptr;              // the pages, one after the other
};

struct Log {
    quint32 sequence = 0;
    QByteArray base;
    QVector<LogEntry> entries;
};

// The entries of a log in Windows 8.1's format, up to the first that's damaged or out of
// order. An invalid log gives nothing.
std::optional<Log> readLog(const QByteArray &bytes)
{
    if (!validBase(bytes) || u32(bytes.constData() + 28) != 6)
        return std::nullopt;
    Log log;
    log.sequence = u32(bytes.constData() + 4);
    log.base = bytes.left(512);
    for (qsizetype at = 512; at + 40 <= bytes.size();) {
        const char *p = bytes.constData() + at;
        if (std::memcmp(p, "HvLE", 4) != 0)
            break;
        const quint32 size = u32(p + 4);
        if (size < 512 || size % 512 != 0 || at + qsizetype(size) > bytes.size())
            break;
        if (marvin32(p, 32) != u64(p + 32) || marvin32(p + 40, size - 40) != u64(p + 24))
            break;
        LogEntry e;
        e.sequence = u32(p + 12);
        e.binsSize = u32(p + 16);
        const quint32 count = u32(p + 20);
        if (count > (size - 40) / 8)
            break;
        quint64 total = 40 + quint64(count) * 8;
        for (quint32 i = 0; i < count; ++i) {
            const quint32 offset = u32(p + 40 + 8 * i), pageSize = u32(p + 44 + 8 * i);
            e.pages.push_back({offset, pageSize});
            total += pageSize;
        }
        if (total > size)
            break;
        e.data = p + 40 + 8 * qsizetype(count);
        if (!log.entries.isEmpty() && e.sequence != log.entries.last().sequence + 1)
            break;
        log.entries.push_back(e);
        at += size;
    }
    return log;
}

} // namespace

quint64 marvin32(const char *data, qsizetype size, quint64 seed)
{
    quint32 p0 = quint32(seed), p1 = quint32(seed >> 32);
    auto block = [&p0, &p1] {
        p1 ^= p0;
        p0 = rotl(p0, 20);
        p0 += p1;
        p1 = rotl(p1, 9);
        p1 ^= p0;
        p0 = rotl(p0, 27);
        p0 += p1;
        p1 = rotl(p1, 19);
    };
    qsizetype i = 0;
    for (; size - i >= 4; i += 4) {
        p0 += u32(data + i);
        block();
    }
    quint32 last = 0x80;
    switch (size - i) {
    case 3:
        last = 0x80000000u | (quint32(uchar(data[i + 2])) << 16) | u16(data + i);
        break;
    case 2:
        last = 0x800000u | u16(data + i);
        break;
    case 1:
        last = 0x8000u | uchar(data[i]);
        break;
    default:
        break;
    }
    p0 += last;
    block();
    block();
    return (quint64(p1) << 32) | p0;
}

QString Value::text() const
{
    switch (type) {
    case Sz:
    case ExpandSz:
    case Link:
        return strings().value(0);
    case MultiSz:
        return strings().join(QStringLiteral("; "));
    case Dword:
    case DwordBigEndian:
    case Qword: {
        bool ok = false;
        const quint64 n = number(&ok);
        return ok ? QString::number(n) : QString();
    }
    default:
        return QString::fromLatin1(data.left(64).toHex(' ')) + (data.size() > 64 ? QStringLiteral(" …") : QString());
    }
}

QStringList Value::strings() const
{
    const QString all = utf16(data.constData(), data.size());
    if (type != MultiSz)
        return {all.left(all.indexOf(QChar(0)) < 0 ? all.size() : all.indexOf(QChar(0)))};
    QStringList out = all.split(QChar(0));
    while (!out.isEmpty() && out.last().isEmpty())
        out.removeLast();
    return out;
}

quint64 Value::number(bool *ok) const
{
    if (ok)
        *ok = true;
    if ((type == Dword || type == Binary) && data.size() == 4)
        return u32(data.constData());
    if (type == DwordBigEndian && data.size() == 4)
        return qFromBigEndian<quint32>(data.constData());
    if ((type == Qword || type == Binary) && data.size() == 8)
        return u64(data.constData());
    if (ok)
        *ok = false;
    return 0;
}

bool Hive::open(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = f.errorString();
        return false;
    }
    QByteArray hive = f.readAll();
    // Its logs: <name>.LOG1 and .LOG2, whatever case the drive keeps them in.
    QVector<QByteArray> logs;
    const QFileInfo info(path);
    const QString base = info.fileName().toLower();
    for (const QFileInfo &other : QDir(info.absolutePath()).entryInfoList(QDir::Files | QDir::Hidden | QDir::System)) {
        const QString name = other.fileName().toLower();
        if (name != base + QStringLiteral(".log1") && name != base + QStringLiteral(".log2"))
            continue;
        QFile log(other.filePath());
        if (log.open(QIODevice::ReadOnly))
            logs << log.readAll();
    }
    return load(std::move(hive), logs, error);
}

bool Hive::load(QByteArray hive, const QVector<QByteArray> &logs, QString *error)
{
    m_data = std::move(hive);
    m_root = 0;
    m_recovered = 0;
    m_behind = false;
    m_problems = 0;
    m_changed.clear();
    if (m_data.size() < kBase || !m_data.startsWith("regf")) {
        *error = QStringLiteral("not a Windows registry file");
        return false;
    }
    const bool baseOk = validBase(m_data);
    m_clean = baseOk && u32(m_data.constData() + 4) == u32(m_data.constData() + 8);
    if (!m_clean && !applyLogs(logs))
        m_behind = true;

    m_minor = u32(m_data.constData() + 24);
    m_binsSize = u32(m_data.constData() + 40);
    if (quint64(m_binsSize) > quint64(m_data.size() - kBase)) {
        // Cut short (a copy that stopped early): what's there is read.
        m_binsSize = quint32((m_data.size() - kBase) / 4096 * 4096);
        ++m_problems;
    }
    const quint32 root = u32(m_data.constData() + 36);
    if (!isKey(root)) {
        *error = QStringLiteral("the registry file is damaged: its first key isn't there");
        return false;
    }
    m_root = root;
    return true;
}

// Puts back what the newest log has that the hive doesn't. Windows uses the two logs in
// turn, starting the other one each time it has brought the hive up to date, so the newest
// log has everything since then.
bool Hive::applyLogs(const QVector<QByteArray> &logs)
{
    std::optional<Log> newest;
    for (const QByteArray &bytes : logs) {
        std::optional<Log> log = readLog(bytes);
        if (log && (!newest || log->sequence > newest->sequence))
            newest = std::move(log);
    }
    const bool baseOk = validBase(m_data);
    if (!newest || (baseOk && newest->sequence < u32(m_data.constData() + 8)))
        return false; // no log, or only one older than the hive
    if (!baseOk)
        std::memcpy(m_data.data(), newest->base.constData(), 512);
    quint32 binsSize = u32(m_data.constData() + 40);
    for (const LogEntry &e : std::as_const(newest->entries)) {
        if (e.binsSize % 4096 != 0 || e.binsSize > (1u << 31))
            break;
        if (e.binsSize > binsSize) {
            if (m_data.size() < kBase + qsizetype(e.binsSize))
                m_data.resize(kBase + qsizetype(e.binsSize), '\0');
        }
        binsSize = e.binsSize;
        const char *page = e.data;
        for (const auto &[offset, size] : e.pages) {
            if (quint64(offset) + size <= binsSize)
                std::memcpy(m_data.data() + kBase + offset, page, size);
            page += size;
        }
        ++m_recovered;
        char *base = m_data.data();
        put32(base + 4, e.sequence);
        put32(base + 8, e.sequence);
        put32(base + 40, binsSize);
        put32(base + 508, checksum(base));
    }
    return true;
}

Hive::Cell Hive::cell(quint32 offset) const
{
    if (offset == kNoCell || quint64(offset) + 4 > m_binsSize) {
        ++m_problems;
        return {};
    }
    const char *p = m_data.constData() + kBase + offset;
    const qint64 size = qint32(u32(p));
    // In use: negative. A free one isn't anything's any more.
    if (size >= 0 || -size < 4 || quint64(offset) + quint64(-size) > m_binsSize) {
        ++m_problems;
        return {};
    }
    return {p + 4, quint32(-size - 4)};
}

bool Hive::isKey(quint32 offset) const
{
    const Cell c = cell(offset);
    return c.data && c.size >= 76 && std::memcmp(c.data, "nk", 2) == 0;
}

QString Hive::name(Key key) const
{
    const Cell c = cell(key);
    if (!c.data || c.size < 76)
        return {};
    const quint16 length = u16(c.data + 72);
    if (76 + quint32(length) > c.size) {
        ++m_problems;
        return {};
    }
    return recordName(c.data + 76, length, u16(c.data + 2) & kKeyCompressedName);
}

QDateTime Hive::written(Key key) const
{
    const Cell c = cell(key);
    return c.data && c.size >= 76 ? fromFiletime(u64(c.data + 4)) : QDateTime();
}

void Hive::collectSubkeys(quint32 list, QVector<Key> &out, int depth) const
{
    const Cell c = cell(list);
    if (!c.data || c.size < 4)
        return;
    const quint16 count = u16(c.data + 2);
    if (std::memcmp(c.data, "lf", 2) == 0 || std::memcmp(c.data, "lh", 2) == 0) {
        for (quint32 i = 0; i < count && 4 + 8 * (i + 1) <= c.size && out.size() < kMostKeys; ++i)
            out.push_back(u32(c.data + 4 + 8 * i));
    } else if (std::memcmp(c.data, "li", 2) == 0) {
        for (quint32 i = 0; i < count && 4 + 4 * (i + 1) <= c.size && out.size() < kMostKeys; ++i)
            out.push_back(u32(c.data + 4 + 4 * i));
    } else if (std::memcmp(c.data, "ri", 2) == 0 && depth == 0) {
        // An index of lists, for keys with many subkeys; never deeper than that.
        for (quint32 i = 0; i < count && 4 + 4 * (i + 1) <= c.size; ++i)
            collectSubkeys(u32(c.data + 4 + 4 * i), out, depth + 1);
    } else {
        ++m_problems;
    }
}

QVector<Key> Hive::subkeys(Key key) const
{
    const Cell c = cell(key);
    QVector<Key> out;
    if (!c.data || c.size < 76 || u32(c.data + 20) == 0)
        return out;
    QVector<Key> listed;
    collectSubkeys(u32(c.data + 28), listed, 0);
    for (Key k : std::as_const(listed)) {
        if (isKey(k))
            out.push_back(k);
    }
    return out;
}

Key Hive::key(const QString &path, Key from) const
{
    Key at = from ? from : m_root;
    for (const QString &part : path.split(QLatin1Char('\\'), Qt::SkipEmptyParts)) {
        Key next = 0;
        for (Key k : subkeys(at)) {
            if (name(k).compare(part, Qt::CaseInsensitive) == 0) {
                next = k;
                break;
            }
        }
        if (!next)
            return 0;
        at = next;
    }
    return at;
}

QByteArray Hive::valueData(quint32 size, quint32 offset) const
{
    if (size & 0x80000000) {
        // Four bytes or fewer, kept where the data's cell would be named.
        char inline4[4];
        put32(inline4, offset);
        return QByteArray(inline4, std::min<quint32>(size & 0x7fffffff, 4));
    }
    if (size == 0 || size > kMostData)
        return {};
    const Cell c = cell(offset);
    if (!c.data)
        return {};
    if (size > kSegment && m_minor >= 4 && c.size >= 8 && std::memcmp(c.data, "db", 2) == 0) {
        // A big value, in pieces of up to 16344 bytes.
        const quint16 count = u16(c.data + 2);
        const Cell list = cell(u32(c.data + 4));
        QByteArray out;
        for (quint32 i = 0; list.data && i < count && 4 * (i + 1) <= list.size && quint32(out.size()) < size; ++i) {
            const Cell piece = cell(u32(list.data + 4 * i));
            if (!piece.data)
                break;
            out.append(piece.data, std::min({piece.size, kSegment, size - quint32(out.size())}));
        }
        return out;
    }
    return QByteArray(c.data, std::min(size, c.size));
}

std::optional<Value> Hive::readValue(quint32 offset) const
{
    const Cell c = cell(offset);
    if (!c.data || c.size < 20 || std::memcmp(c.data, "vk", 2) != 0) {
        ++m_problems;
        return std::nullopt;
    }
    const quint16 nameLength = u16(c.data + 2);
    if (20 + quint32(nameLength) > c.size) {
        ++m_problems;
        return std::nullopt;
    }
    Value v;
    v.cell = offset;
    v.type = u32(c.data + 12);
    v.name = recordName(c.data + 20, nameLength, u16(c.data + 16) & kValueCompressedName);
    v.data = valueData(u32(c.data + 4), u32(c.data + 8));
    return v;
}

QVector<Value> Hive::values(Key key) const
{
    QVector<Value> out;
    const Cell k = cell(key);
    if (!k.data || k.size < 76)
        return out;
    const quint32 count = u32(k.data + 36);
    if (count == 0)
        return out;
    const Cell list = cell(u32(k.data + 40));
    for (quint32 i = 0; list.data && i < count && 4 * (i + 1) <= list.size; ++i) {
        if (std::optional<Value> v = readValue(u32(list.data + 4 * i)))
            out.push_back(std::move(*v));
    }
    return out;
}

std::optional<Value> Hive::value(Key key, const QString &name) const
{
    for (Value &v : values(key)) {
        if (v.name.compare(name, Qt::CaseInsensitive) == 0)
            return std::move(v);
    }
    return std::nullopt;
}

bool Hive::valueRecord(Key key, const QString &name, quint32 *listCell, int *index, quint32 *record) const
{
    const Cell k = cell(key);
    if (!k.data || k.size < 76)
        return false;
    const quint32 count = u32(k.data + 36);
    *listCell = u32(k.data + 40);
    const Cell list = cell(*listCell);
    for (quint32 i = 0; list.data && i < count && 4 * (i + 1) <= list.size; ++i) {
        const quint32 at = u32(list.data + 4 * i);
        std::optional<Value> v = readValue(at);
        if (v && v->name.compare(name, Qt::CaseInsensitive) == 0) {
            *index = int(i);
            *record = at;
            return true;
        }
    }
    return false;
}

void Hive::markChanged(quint32 offset, quint32 size)
{
    m_changed.push_back({quint32(kBase) + offset, size});
}

namespace {

QString notClosedProperly()
{
    return QStringLiteral("Windows didn't close this registry file properly (Fast Startup, a crash, or it's running), so it isn't changed. "
                          "Start Windows and shut it down fully first: hold Shift while clicking Shut down.");
}

} // namespace

bool Hive::setDword(Key key, const QString &name, quint32 number, QString *error)
{
    if (!canWrite()) {
        *error = notClosedProperly();
        return false;
    }
    quint32 list = 0, record = 0;
    int index = 0;
    if (!valueRecord(key, name, &list, &index, &record)) {
        *error = QStringLiteral("there's no value called %1").arg(name);
        return false;
    }
    char *vk = m_data.data() + kBase + record + 4;
    const quint32 size = u32(vk + 4);
    if (u32(vk + 12) != Dword || (size & 0x7fffffff) != 4) {
        *error = QStringLiteral("%1 isn't a number").arg(name);
        return false;
    }
    if (size & 0x80000000) {
        put32(vk + 8, number);
        markChanged(record + 4 + 8, 4);
        return true;
    }
    const quint32 dataCell = u32(vk + 8);
    if (!cell(dataCell).data) {
        *error = QStringLiteral("the registry file is damaged");
        return false;
    }
    put32(m_data.data() + kBase + dataCell + 4, number);
    markChanged(dataCell + 4, 4);
    return true;
}

bool Hive::setStrings(Key key, const QString &name, const QStringList &strings, QString *error)
{
    // Stored like one string with NULs between, and two at the end.
    QString all;
    for (const QString &s : strings)
        all += s + QChar(0);
    return setText(key, name, all, MultiSz, error);
}

bool Hive::setString(Key key, const QString &name, const QString &text, QString *error)
{
    return setText(key, name, text, 0, error);
}

bool Hive::setText(Key key, const QString &name, const QString &text, quint32 wantedType, QString *error)
{
    if (!canWrite()) {
        *error = notClosedProperly();
        return false;
    }
    quint32 list = 0, record = 0;
    int index = 0;
    if (!valueRecord(key, name, &list, &index, &record)) {
        *error = QStringLiteral("there's no value called %1").arg(name);
        return false;
    }
    char *vk = m_data.data() + kBase + record + 4;
    const quint32 type = u32(vk + 12), size = u32(vk + 4);
    if (wantedType ? type != wantedType : type != Sz && type != ExpandSz) {
        *error = QStringLiteral("%1 isn't the kind of value expected").arg(name);
        return false;
    }
    QByteArray bytes;
    for (QChar ch : text) {
        char two[2];
        qToLittleEndian<quint16>(ch.unicode(), two);
        bytes.append(two, 2);
    }
    bytes.append(2, '\0');
    const quint32 dataCell = u32(vk + 8);
    const Cell c = (size & 0x80000000) || size > kSegment ? Cell{} : cell(dataCell);
    if (!c.data || quint32(bytes.size()) > c.size) {
        *error = QStringLiteral("the new text doesn't fit where the old one was");
        return false;
    }
    std::memcpy(m_data.data() + kBase + dataCell + 4, bytes.constData(), bytes.size());
    put32(vk + 4, quint32(bytes.size()));
    markChanged(dataCell + 4, quint32(bytes.size()));
    markChanged(record + 4 + 4, 4);
    return true;
}

bool Hive::removeValue(Key key, const QString &name, QString *error)
{
    if (!canWrite()) {
        *error = notClosedProperly();
        return false;
    }
    quint32 list = 0, record = 0;
    int index = 0;
    if (!valueRecord(key, name, &list, &index, &record)) {
        *error = QStringLiteral("there's no value called %1").arg(name);
        return false;
    }
    // A cell is freed by making its size positive; Windows finds free cells when it loads
    // the hive.
    auto release = [this](quint32 offset) {
        if (!cell(offset).data)
            return;
        char *p = m_data.data() + kBase + offset;
        put32(p, quint32(-qint32(u32(p))));
        markChanged(offset, 4);
    };
    const char *vk = m_data.constData() + kBase + record + 4;
    const quint32 size = u32(vk + 4), dataCell = u32(vk + 8);
    if (!(size & 0x80000000) && size > 0) {
        const Cell data = cell(dataCell);
        if (data.data && size > kSegment && m_minor >= 4 && data.size >= 8 && std::memcmp(data.data, "db", 2) == 0) {
            const Cell pieces = cell(u32(data.data + 4));
            for (quint32 i = 0; pieces.data && i < u16(data.data + 2) && 4 * (i + 1) <= pieces.size; ++i)
                release(u32(pieces.data + 4 * i));
            release(u32(data.data + 4));
        }
        release(dataCell);
    }
    release(record);

    // Out of the key's list of values.
    char *nk = m_data.data() + kBase + key + 4;
    const quint32 count = u32(nk + 36);
    char *values = m_data.data() + kBase + list + 4;
    std::memmove(values + 4 * index, values + 4 * (index + 1), 4 * (count - 1 - quint32(index)));
    put32(values + 4 * (count - 1), 0);
    markChanged(list + 4, 4 * count);
    put32(nk + 36, count - 1);
    if (count == 1) {
        put32(nk + 40, kNoCell);
        release(list);
    }
    qToLittleEndian<quint64>(filetimeNow(), nk + 4);
    markChanged(key + 4, 44);
    return true;
}

bool Hive::save(const QString &path, QString *error)
{
    if (m_changed.isEmpty())
        return true;
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite)) {
        *error = f.errorString();
        return false;
    }
    for (const auto &[at, size] : std::as_const(m_changed)) {
        if (!f.seek(at) || f.write(m_data.constData() + at, size) != qint64(size)) {
            *error = f.errorString();
            return false;
        }
    }
    if (!blockio::flushFile(f)) {
        *error = f.errorString();
        return false;
    }
    m_changed.clear();
    return true;
}

} // namespace reg
