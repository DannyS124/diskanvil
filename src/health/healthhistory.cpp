// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "healthhistory.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <climits>
#include <functional>

namespace healthhistory {

namespace {

constexpr int kKeep = 730; // days

QString tr(const char *text, int n = -1)
{
    return QCoreApplication::translate("healthhistory", text, nullptr, n);
}

QString fileFor(const QString &key)
{
    // The key is a drive ID with slashes and odd characters in it: the file is named by its hash.
    return folder() + QLatin1Char('/') + QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(16))
         + QStringLiteral(".json");
}

QJsonObject toJson(const Point &p)
{
    QJsonObject o{{QStringLiteral("day"), p.day.toString(Qt::ISODate)}, {QStringLiteral("state"), p.state}};
    auto put = [&o](const char *name, double v) {
        if (v >= 0)
            o.insert(QLatin1String(name), v);
    };
    put("temp", p.temperatureC);
    put("hours", double(p.hours));
    put("replaced", double(p.replaced));
    put("pending", double(p.pending));
    put("unreadable", double(p.unreadable));
    put("cable", double(p.cable));
    put("wear", p.wear);
    put("media", double(p.mediaErrors));
    put("written", double(p.written));
    return o;
}

Point fromJson(const QJsonObject &o)
{
    Point p;
    p.day = QDate::fromString(o.value(QStringLiteral("day")).toString(), Qt::ISODate);
    p.state = o.value(QStringLiteral("state")).toInt();
    auto get = [&o](const char *name) { return o.value(QLatin1String(name)).toDouble(-1); };
    p.temperatureC = get("temp");
    p.hours = qint64(get("hours"));
    p.replaced = qint64(get("replaced"));
    p.pending = qint64(get("pending"));
    p.unreadable = qint64(get("unreadable"));
    p.cable = qint64(get("cable"));
    p.wear = int(get("wear"));
    p.mediaErrors = qint64(get("media"));
    p.written = qint64(get("written"));
    return p;
}

} // namespace

Point pointFor(const Disk &disk, const QVector<SmartAttribute> &attributes)
{
    const Health &h = disk.health;
    Point p;
    p.day = QDate::currentDate();
    p.temperatureC = h.temperatureC > 0 ? h.temperatureC : -1;
    p.hours = h.powerOnHours > 0 ? qint64(h.powerOnHours) : -1;
    p.state = int(h.state);
    p.wear = h.percentUsed >= 0 ? h.percentUsed : h.lifeLeft >= 0 ? 100 - h.lifeLeft : -1;
    for (const SmartAttribute &a : attributes) {
        const qint64 raw = std::max<qint64>(a.rawValue, 0);
        if (!a.key.isEmpty()) {
            if (a.key == QLatin1String("media_errors"))
                p.mediaErrors = raw;
            else if (a.key == QLatin1String("total_data_written"))
                p.written = raw;
            continue;
        }
        switch (a.id) {
        case 5: p.replaced = raw; break;
        case 197: p.pending = raw; break;
        case 198: p.unreadable = raw; break;
        case 199: p.cable = raw; break;
        default: break;
        }
    }
    return p;
}

QString folder()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/diskanvil/health");
}

History load(const QString &key)
{
    History h;
    h.key = key;
    QFile f(fileFor(key));
    if (!f.open(QIODevice::ReadOnly))
        return h;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.value(QStringLiteral("key")).toString() != key)
        return h; // another drive's, with the same hash: not this one's
    h.model = o.value(QStringLiteral("model")).toString();
    h.serial = o.value(QStringLiteral("serial")).toString();
    for (const QJsonValue &v : o.value(QStringLiteral("points")).toArray()) {
        const Point p = fromJson(v.toObject());
        if (p.day.isValid())
            h.points << p;
    }
    if (o.contains(QStringLiteral("checked")))
        h.checked = fromJson(o.value(QStringLiteral("checked")).toObject());
    return h;
}

bool save(const History &h)
{
    QDir().mkpath(folder());
    QJsonArray points;
    for (const Point &p : h.points.mid(std::max<qsizetype>(0, h.points.size() - kKeep)))
        points << toJson(p);
    QJsonObject o{{QStringLiteral("key"), h.key}, {QStringLiteral("model"), h.model}, {QStringLiteral("serial"), h.serial}, {QStringLiteral("points"), points}};
    if (h.checked.day.isValid())
        o.insert(QStringLiteral("checked"), toJson(h.checked));
    QSaveFile f(fileFor(h.key));
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    return f.commit();
}

bool hasToday(const QString &key)
{
    const History h = load(key);
    return !h.points.isEmpty() && h.points.last().day == QDate::currentDate();
}

void note(const Disk &disk, const Point &point)
{
    if (disk.health.key.isEmpty())
        return;
    History h = load(disk.health.key);
    h.model = disk.model;
    h.serial = disk.serial;
    if (!h.points.isEmpty() && h.points.last().day == point.day)
        h.points.last() = point;
    else
        h.points << point;
    save(h);
}

bool noteIfDue(UDisks *udisks, const Disk &disk)
{
    if (disk.health.state == Health::State::Unknown || disk.health.key.isEmpty() || disk.isLoop || hasToday(disk.health.key))
        return false;
    note(disk, pointFor(disk, udisks->smartAttributes(disk)));
    return true;
}

QStringList worse(const Point &before, const Point &now)
{
    QStringList out;
    // Only from a verdict that was known: a drive whose data wasn't ready last time isn't "worse".
    if (before.day.isValid() && before.state != int(Health::State::Unknown) && now.state > before.state
        && now.state >= int(Health::State::Warning)) {
        out << (now.state == int(Health::State::Failing) ? tr("It's failing now (it was %1).") : tr("It needs watching now (it was %1)."))
                   .arg(before.state == int(Health::State::Healthy) ? tr("healthy") : tr("worth watching"));
    }
    auto grew = [&out](qint64 was, qint64 is, const char *what) {
        if (was >= 0 && is > was)
            out << tr(what, int(std::min<qint64>(is - was, INT_MAX))).arg(is);
    };
    grew(before.replaced, now.replaced, QT_TRANSLATE_NOOP("healthhistory", "%n more sector(s) replaced by spares (%1 in all)."));
    grew(before.pending, now.pending, QT_TRANSLATE_NOOP("healthhistory", "%n more sector(s) it can't read right now (%1 in all)."));
    grew(before.unreadable, now.unreadable, QT_TRANSLATE_NOOP("healthhistory", "%n more sector(s) it couldn't read in its own check (%1 in all)."));
    grew(before.cable, now.cable, QT_TRANSLATE_NOOP("healthhistory", "%n more data error(s) on the cable (%1 in all): check or change the cable."));
    grew(before.mediaErrors, now.mediaErrors, QT_TRANSLATE_NOOP("healthhistory", "%n more media error(s) (%1 in all)."));
    for (const int line : {80, 90, 100}) {
        if (before.wear >= 0 && before.wear < line && now.wear >= line)
            out << tr("It has used %1% of its life.").arg(now.wear);
    }
    return out;
}

QVector<Series> series(const History &h)
{
    QVector<Series> out;
    auto add = [&](const QString &name, const QString &unit, bool badWhenUp, const std::function<double(const Point &)> &value) {
        Series s{name, unit, badWhenUp, {}};
        for (const Point &p : h.points) {
            const double v = value(p);
            if (v >= 0)
                s.values.append({p.day, v});
        }
        if (!s.values.isEmpty())
            out << s;
    };
    add(tr("Temperature"), QStringLiteral("°C"), true, [](const Point &p) { return p.temperatureC; });
    add(tr("Replaced sectors"), QString(), true, [](const Point &p) { return double(p.replaced); });
    add(tr("Sectors it can't read now"), QString(), true, [](const Point &p) { return double(p.pending); });
    add(tr("Sectors unreadable in its own check"), QString(), true, [](const Point &p) { return double(p.unreadable); });
    add(tr("Cable errors"), QString(), true, [](const Point &p) { return double(p.cable); });
    add(tr("Life used"), QStringLiteral("%"), true, [](const Point &p) { return double(p.wear); });
    add(tr("Media errors"), QString(), true, [](const Point &p) { return double(p.mediaErrors); });
    add(tr("Data written"), QStringLiteral("bytes"), false, [](const Point &p) { return double(p.written); });
    return out;
}

} // namespace healthhistory
