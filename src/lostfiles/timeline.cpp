// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timeline.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QTimeZone>

#include <algorithm>

namespace timeline {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("timeline", text);
}

QByteArray csvField(const QString &text)
{
    QString t = text;
    if (t.contains(QLatin1Char(',')) || t.contains(QLatin1Char('"')) || t.contains(QLatin1Char('\n'))) {
        t.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        t = QLatin1Char('"') + t + QLatin1Char('"');
    }
    return t.toUtf8();
}

} // namespace

QVector<Event> events(const QVector<lost::Found> &files, qint64 now)
{
    const qint64 earliest = QDateTime(QDate(1980, 1, 1), QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();
    const qint64 latest = now + 24LL * 3600 * 1000;
    QVector<Event> out;
    for (int i = 0; i < files.size(); ++i) {
        const lost::Found &f = files.at(i);
        const auto add = [&](qint64 when, Event::Kind kind, bool dayOnly = false) {
            if (when >= earliest && when <= latest)
                out.push_back({when, kind, i, dayOnly});
        };
        add(f.created, Event::Kind::Created);
        add(f.modified, Event::Kind::Written);
        add(f.accessed, Event::Kind::Opened, f.accessedDayOnly);
        // NTFS's record time: only when it says something the others don't.
        if (f.changed != f.modified && f.changed != f.created)
            add(f.changed, Event::Kind::Changed);
        add(f.deletedAt, Event::Kind::Deleted);
        add(f.inside, Event::Kind::Taken);
    }
    std::stable_sort(out.begin(), out.end(), [](const Event &a, const Event &b) { return a.when < b.when; });
    return out;
}

QString kindName(Event::Kind kind)
{
    switch (kind) {
    case Event::Kind::Created:
        return tr("Made");
    case Event::Kind::Written:
        return tr("Written");
    case Event::Kind::Opened:
        return tr("Opened");
    case Event::Kind::Changed:
        return tr("Renamed or moved");
    case Event::Kind::Deleted:
        return tr("Deleted");
    case Event::Kind::Taken:
        break;
    }
    return tr("Its own date (a photo taken, an email sent)");
}

QByteArray csv(const QVector<Event> &events, const QVector<lost::Found> &files)
{
    QByteArray out = csvField(tr("When")) + ',' + csvField(tr("What happened")) + ',' + csvField(tr("Name")) + ',' + csvField(tr("Folder")) + ','
                   + csvField(tr("Size")) + ',' + csvField(tr("Deleted")) + "\r\n";
    for (const Event &e : events) {
        const lost::Found &f = files.at(e.file);
        const QDateTime when = QDateTime::fromMSecsSinceEpoch(e.when);
        out += csvField(e.dayOnly ? when.date().toString(Qt::ISODate) : when.toString(Qt::ISODate)) + ',' + csvField(kindName(e.kind)) + ','
             + csvField(f.name) + ',' + csvField(f.folder) + ',' + QByteArray::number(f.size) + ',' + (f.deleted ? "yes" : "no") + "\r\n";
    }
    return out;
}

} // namespace timeline
