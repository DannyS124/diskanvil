// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "evtx.h"

#include <QHash>
#include <QSet>
#include <QTimeZone>

#include <algorithm>
#include <cstring>
#include <optional>

namespace evtx {

QString Event::value(const QString &name) const
{
    for (const auto &d : data)
        if (d.first.compare(name, Qt::CaseInsensitive) == 0)
            return d.second;
    return {};
}

QString Event::at(int index) const
{
    return index >= 0 && index < data.size() ? data[index].second : QString();
}

namespace {

constexpr int kChunk = 65536;
constexpr int kDepth = 64; // how deep elements and nested fragments may go

// A rendered element: just enough of XML for what's read from it.
struct Node {
    QString name;
    QVector<QPair<QString, QString>> attributes;
    QString text;
    QVector<Node> children;

    const Node *child(const QString &n) const
    {
        for (const Node &c : children)
            if (c.name == n)
                return &c;
        return nullptr;
    }
    QString attribute(const QString &n) const
    {
        for (const auto &a : attributes)
            if (a.first == n)
                return a.second;
        return {};
    }
};

// A substitution value: its type and bytes (and where they are in the chunk, for nested ones).
struct Value {
    quint8 type = 0;
    int offset = 0; // chunk-relative
    int size = 0;
};

QString fileTime(quint64 t)
{
    // 100 ns since 1601 (UTC).
    if (t == 0)
        return {};
    const qint64 ms = qint64(t / 10000) - 11644473600000LL;
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODateWithMs);
}

class Chunk
{
public:
    explicit Chunk(const char *data)
        : d(reinterpret_cast<const uchar *>(data))
    {
    }

    bool ok = true;

    template<typename T>
    T get(int at)
    {
        if (at < 0 || at + int(sizeof(T)) > kChunk) {
            ok = false;
            return T();
        }
        T v;
        std::memcpy(&v, d + at, sizeof(T));
        return v;
    }
    QString utf16(int at, int chars)
    {
        if (chars < 0 || at < 0 || at + 2 * chars > kChunk) {
            ok = false;
            return {};
        }
        QString s(chars, Qt::Uninitialized);
        for (int i = 0; i < chars; ++i)
            s[i] = QChar(quint16(d[at + 2 * i] | (d[at + 2 * i + 1] << 8)));
        return s;
    }
    // A name: u32 next, u16 hash, u16 characters, then them and a NUL. Its size, so an inline one can be skipped.
    QString name(int at, int *size = nullptr)
    {
        const auto it = m_names.constFind(at);
        const int n = get<quint16>(at + 6);
        if (size)
            *size = 8 + 2 * n + 2;
        if (it != m_names.cend())
            return *it;
        const QString s = utf16(at + 8, n);
        m_names.insert(at, s);
        return s;
    }

    // Renders the fragment from `at` (stopping at its end, or at `end`): the elements it holds.
    QVector<Node> fragment(int &at, int end, const QVector<Value> *values, int depth)
    {
        QVector<Node> out;
        if (depth > kDepth) {
            ok = false;
            return out;
        }
        while (ok && at < end) {
            const quint8 token = get<quint8>(at);
            switch (token & 0x1f) {
            case 0x00: // end of the fragment
                ++at;
                return out;
            case 0x0f: // fragment header
                at += 4;
                break;
            case 0x0c:
                out += templateInstance(at, end, depth);
                break;
            case 0x01:
                out += element(at, end, values, depth);
                break;
            default:
                // Anything else at the top: not an event this reads.
                ok = false;
                return out;
            }
        }
        return out;
    }

private:
    const uchar *d;
    QHash<int, QString> m_names;

    QVector<Node> templateInstance(int &at, int end, int depth)
    {
        const int definition = int(get<quint32>(at + 6));
        at += 10;
        if (definition == at) { // the definition is here: skip it
            const int size = int(get<quint32>(at + 20));
            at += 24 + size;
        }
        const int count = int(get<quint32>(at));
        at += 4;
        if (!ok || count < 0 || count > 4096 || at + 4 * count > end) {
            ok = false;
            return {};
        }
        QVector<Value> values(count);
        for (int i = 0; i < count; ++i) {
            values[i].size = get<quint16>(at);
            values[i].type = get<quint8>(at + 2);
            at += 4;
        }
        for (Value &v : values) {
            v.offset = at;
            at += v.size;
        }
        if (at > end) {
            ok = false;
            return {};
        }
        // The definition: u32 next, GUID, u32 size, then its body (a fragment with the substitutions).
        const int size = int(get<quint32>(definition + 20));
        int body = definition + 24;
        if (!ok || size < 0 || body + size > kChunk) {
            ok = false;
            return {};
        }
        return fragment(body, body + size, &values, depth + 1);
    }

    // An attribute's or element's value: text, a substitution, or a character.
    // Returns false at a token that isn't a value (what comes next is something else).
    bool valuePiece(int &at, const QVector<Value> *values, QString *text, QVector<Node> *nested, QStringList *items, bool *dropped, int depth)
    {
        const quint8 token = get<quint8>(at);
        switch (token & 0x1f) {
        case 0x05: { // text
            const int n = get<quint16>(at + 2);
            *text += utf16(at + 4, n);
            at += 4 + 2 * n;
            return true;
        }
        case 0x07: { // CDATA
            const int n = get<quint16>(at + 1);
            *text += utf16(at + 3, n);
            at += 3 + 2 * n;
            return true;
        }
        case 0x08: // a character by number
            *text += QChar(get<quint16>(at + 1));
            at += 3;
            return true;
        case 0x09: { // &lt; and the like
            const int offset = int(get<quint32>(at + 1));
            at += 5;
            int size = 0;
            const QString n = name(offset, &size);
            if (offset == at)
                at += size;
            static const QHash<QString, QString> entities = {{QStringLiteral("lt"), QStringLiteral("<")}, {QStringLiteral("gt"), QStringLiteral(">")},
                                                             {QStringLiteral("amp"), QStringLiteral("&")}, {QStringLiteral("quot"), QStringLiteral("\"")},
                                                             {QStringLiteral("apos"), QStringLiteral("'")}};
            *text += entities.value(n);
            return true;
        }
        case 0x0d:
        case 0x0e: { // substitution: one of the template's values
            const bool optional = (token & 0x1f) == 0x0e;
            const int index = get<quint16>(at + 1);
            at += 4;
            if (!values || index >= values->size()) {
                if (optional)
                    *dropped = true;
                return true;
            }
            const Value &v = (*values)[index];
            if (v.type == 0 || v.size == 0) {
                if (optional)
                    *dropped = true;
                return true;
            }
            if (v.type == 0x21) { // nested binary XML
                int from = v.offset;
                *nested += fragment(from, v.offset + v.size, nullptr, depth + 1);
                return true;
            }
            if (v.type & 0x80)
                *items += arrayItems(v);
            else
                *text += render(v.type, v.offset, v.size);
            return true;
        }
        default:
            return false;
        }
    }

    QVector<Node> element(int &at, int end, const QVector<Value> *values, int depth)
    {
        if (depth > kDepth) {
            ok = false;
            return {};
        }
        const quint8 token = get<quint8>(at);
        const int nameOffset = int(get<quint32>(at + 7));
        at += 11;
        Node node;
        int size = 0;
        node.name = name(nameOffset, &size);
        if (nameOffset == at)
            at += size;
        if (token & 0x40)
            at += 4; // the attribute list's size
        bool dropped = false;
        // Attributes.
        while (ok && at < end && (get<quint8>(at) & 0x1f) == 0x06) {
            const quint8 attributeToken = get<quint8>(at);
            const int attributeName = int(get<quint32>(at + 1));
            at += 5;
            int nsize = 0;
            const QString n = name(attributeName, &nsize);
            if (attributeName == at)
                at += nsize;
            QString text;
            QVector<Node> nested;
            QStringList items;
            bool gone = false;
            while (ok && at < end && valuePiece(at, values, &text, &nested, &items, &gone, depth)) {
            }
            if (!gone)
                node.attributes << qMakePair(n, items.isEmpty() ? text : items.join(QLatin1Char(' ')));
            if (!(attributeToken & 0x40))
                break;
        }
        if (!ok || at >= end)
            return {};
        const quint8 close = get<quint8>(at);
        ++at;
        if (close == 0x03)
            return {node};
        if (close != 0x02) {
            ok = false;
            return {};
        }
        // Content, up to the end of the element.
        QStringList items;
        bool substituted = false;
        while (ok && at < end) {
            const quint8 t = get<quint8>(at);
            if (t == 0x04) {
                ++at;
                break;
            }
            if ((t & 0x1f) == 0x01) {
                node.children += element(at, end, values, depth + 1);
                continue;
            }
            if ((t & 0x1f) == 0x0a || (t & 0x1f) == 0x0b) { // processing instructions: skipped
                if ((t & 0x1f) == 0x0a) {
                    const int offset = int(get<quint32>(at + 1));
                    at += 5;
                    int nsize = 0;
                    name(offset, &nsize);
                    if (offset == at)
                        at += nsize;
                } else {
                    at += 3 + 2 * get<quint16>(at + 1);
                }
                continue;
            }
            const bool sub = (t & 0x1f) == 0x0d || (t & 0x1f) == 0x0e;
            bool gone = false;
            if (!valuePiece(at, values, &node.text, &node.children, &items, &gone, depth)) {
                ok = false;
                return {};
            }
            substituted = substituted || sub;
            dropped = dropped || gone;
        }
        if (dropped && node.text.isEmpty() && node.children.isEmpty() && items.isEmpty())
            return {};
        // An array put into an element: the element once for each of its items.
        if (!items.isEmpty()) {
            QVector<Node> out;
            for (const QString &item : items) {
                Node copy = node;
                copy.text += item;
                out << copy;
            }
            return out;
        }
        Q_UNUSED(substituted)
        return {node};
    }

    QStringList arrayItems(const Value &v)
    {
        const quint8 type = v.type & 0x7f;
        QStringList out;
        if (type == 0x01) { // NUL-separated strings
            const QString all = utf16(v.offset, v.size / 2);
            out = all.split(QChar(0));
            if (!out.isEmpty() && out.last().isEmpty())
                out.removeLast();
            return out;
        }
        static const QHash<int, int> sizes = {{0x03, 1}, {0x04, 1}, {0x05, 2}, {0x06, 2}, {0x07, 4}, {0x08, 4}, {0x09, 8}, {0x0a, 8},
                                              {0x0b, 4}, {0x0c, 8}, {0x0d, 4}, {0x0f, 16}, {0x11, 8}, {0x12, 16}, {0x14, 4}, {0x15, 8}};
        const int each = sizes.value(type, 0);
        if (each == 0)
            return {render(0x0e, v.offset, v.size)};
        for (int i = 0; i + each <= v.size && out.size() < 4096; i += each)
            out << render(type, v.offset + i, each);
        return out;
    }

    QString render(quint8 type, int at, int size)
    {
        if (at < 0 || size < 0 || at + size > kChunk) {
            ok = false;
            return {};
        }
        switch (type) {
        case 0x01: {
            QString s = utf16(at, size / 2);
            while (s.endsWith(QChar(0)))
                s.chop(1);
            return s;
        }
        case 0x02: {
            QByteArray b(reinterpret_cast<const char *>(d + at), size);
            while (b.endsWith('\0'))
                b.chop(1);
            return QString::fromLatin1(b);
        }
        case 0x03:
            return QString::number(qint8(get<quint8>(at)));
        case 0x04:
            return QString::number(get<quint8>(at));
        case 0x05:
            return QString::number(get<qint16>(at));
        case 0x06:
            return QString::number(get<quint16>(at));
        case 0x07:
            return QString::number(get<qint32>(at));
        case 0x08:
            return QString::number(get<quint32>(at));
        case 0x09:
            return QString::number(get<qint64>(at));
        case 0x0a:
            return QString::number(get<quint64>(at));
        case 0x0b:
            return QString::number(double(get<float>(at)));
        case 0x0c:
            return QString::number(get<double>(at));
        case 0x0d:
            return get<quint32>(at) ? QStringLiteral("true") : QStringLiteral("false");
        case 0x0f: {
            const quint32 a = get<quint32>(at);
            const quint16 b = get<quint16>(at + 4), c = get<quint16>(at + 6);
            QString s = QStringLiteral("{%1-%2-%3-").arg(a, 8, 16, QLatin1Char('0')).arg(b, 4, 16, QLatin1Char('0')).arg(c, 4, 16, QLatin1Char('0'));
            for (int i = 8; i < 16; ++i) {
                if (i == 10)
                    s += QLatin1Char('-');
                s += QStringLiteral("%1").arg(d[at + i], 2, 16, QLatin1Char('0'));
            }
            return s.toUpper() + QLatin1Char('}');
        }
        case 0x10:
            return QStringLiteral("0x") + QString::number(size >= 8 ? get<quint64>(at) : get<quint32>(at), 16);
        case 0x11:
            return fileTime(get<quint64>(at));
        case 0x12: {
            const QDate date(get<quint16>(at), get<quint16>(at + 2), get<quint16>(at + 6));
            const QTime time(get<quint16>(at + 8), get<quint16>(at + 10), get<quint16>(at + 12), get<quint16>(at + 14));
            return QDateTime(date, time, QTimeZone::UTC).toString(Qt::ISODateWithMs);
        }
        case 0x13: {
            if (size < 8)
                return {};
            const int count = d[at + 1];
            quint64 authority = 0;
            for (int i = 0; i < 6; ++i)
                authority = (authority << 8) | d[at + 2 + i];
            QString s = QStringLiteral("S-%1-%2").arg(d[at]).arg(authority);
            for (int i = 0; i < count && 8 + 4 * i + 4 <= size; ++i)
                s += QLatin1Char('-') + QString::number(get<quint32>(at + 8 + 4 * i));
            return s;
        }
        case 0x14:
            return QStringLiteral("0x") + QString::number(get<quint32>(at), 16);
        case 0x15:
            return QStringLiteral("0x") + QString::number(get<quint64>(at), 16);
        default: // binary and anything not known: hex
            return QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(d + at), size).toHex().toUpper());
        }
    }
};

// What's read from an event's rendered tree.
std::optional<Event> fromTree(const QVector<Node> &nodes)
{
    const Node *event = nullptr;
    for (const Node &n : nodes)
        if (n.name == QLatin1String("Event"))
            event = &n;
    if (!event)
        return std::nullopt;
    const Node *system = event->child(QStringLiteral("System"));
    if (!system)
        return std::nullopt;
    Event e;
    if (const Node *p = system->child(QStringLiteral("Provider"))) {
        e.provider = p->attribute(QStringLiteral("Name"));
        if (e.provider.isEmpty())
            e.provider = p->attribute(QStringLiteral("EventSourceName"));
    }
    if (const Node *id = system->child(QStringLiteral("EventID")))
        e.id = id->text.trimmed().toUInt();
    if (const Node *level = system->child(QStringLiteral("Level")))
        e.level = level->text.trimmed().toInt();
    if (const Node *t = system->child(QStringLiteral("TimeCreated")))
        e.time = QDateTime::fromString(t->attribute(QStringLiteral("SystemTime")), Qt::ISODateWithMs);
    if (const Node *r = system->child(QStringLiteral("EventRecordID")))
        e.record = r->text.trimmed().toULongLong();
    if (const Node *data = event->child(QStringLiteral("EventData"))) {
        for (const Node &c : data->children) {
            if (c.name == QLatin1String("Data"))
                e.data << qMakePair(c.attribute(QStringLiteral("Name")), c.text);
            else if (c.name == QLatin1String("Binary"))
                e.binary = c.text;
        }
    } else if (const Node *user = event->child(QStringLiteral("UserData"))) {
        for (const Node &specific : user->children)
            for (const Node &c : specific.children)
                e.data << qMakePair(c.name, c.text);
    }
    return e;
}

} // namespace

Log read(const QByteArray &file, const std::atomic<bool> *cancel)
{
    Log log;
    if (file.size() < 4096 + kChunk || !file.startsWith(QByteArrayLiteral("ElfFile\0"))) {
        log.problem = QStringLiteral("not an event log");
        return log;
    }
    QSet<quint64> seen;
    // Every chunk the file has room for: the header's count can be behind after a crash.
    for (qint64 base = 4096; base + kChunk <= file.size(); base += kChunk) {
        if (cancel && *cancel)
            break;
        const char *chunk = file.constData() + base;
        if (std::memcmp(chunk, "ElfChnk\0", 8) != 0)
            continue;
        ++log.chunks;
        quint32 freeSpace = 0;
        std::memcpy(&freeSpace, chunk + 48, 4);
        const int official = int(std::clamp<quint32>(freeSpace, 512, kChunk));
        for (int at = 512; at + 28 <= kChunk;) {
            quint32 sig = 0, size = 0;
            std::memcpy(&sig, chunk + at, 4);
            std::memcpy(&size, chunk + at + 4, 4);
            quint32 copy = 0;
            const bool shaped = sig == 0x00002a2a && size >= 28 && at + qint64(size) <= kChunk
                             && (std::memcpy(&copy, chunk + at + size - 4, 4), copy == size);
            if (!shaped) {
                if (at < official) {
                    // A broken record inside the chunk: nothing after it can be found by size.
                    ++log.broken;
                }
                // Past the official end: look for the next record's signature, 8 bytes at a time.
                at += 8;
                if (at >= official && at + 28 > kChunk)
                    break;
                continue;
            }
            quint64 record = 0;
            std::memcpy(&record, chunk + at + 8, 8);
            Chunk c(chunk);
            int from = at + 24;
            const QVector<Node> nodes = c.fragment(from, at + int(size) - 4, nullptr, 0);
            if (c.ok && !seen.contains(record)) {
                if (std::optional<Event> e = fromTree(nodes)) {
                    e->record = e->record ? e->record : record;
                    e->recovered = at >= official;
                    seen.insert(record);
                    log.events << *e;
                }
            } else if (!c.ok) {
                ++log.broken;
            }
            at += int(size);
        }
    }
    std::sort(log.events.begin(), log.events.end(), [](const Event &a, const Event &b) { return a.record < b.record; });
    if (log.events.isEmpty() && log.problem.isEmpty())
        log.problem = QStringLiteral("no events could be read");
    return log;
}

} // namespace evtx
