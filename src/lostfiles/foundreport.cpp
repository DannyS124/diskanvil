// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "foundreport.h"

#include "filetypes.h"
#include "format.h"
#include "places.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QTimeZone>

namespace foundreport {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("foundreport", text);
}

// Times in reports are written so there's no doubt: 2024-07-14T13:22:05+02:00, in this PC's zone.
QString stamp(qint64 ms)
{
    return ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::systemTimeZone()).toString(Qt::ISODate) : QString();
}

QString condition(lost::Condition c)
{
    switch (c) {
    case lost::Condition::Good:
        return tr("looks whole");
    case lost::Condition::MaybeDamaged:
        return tr("may be damaged");
    case lost::Condition::Overwritten:
        return tr("partly overwritten");
    case lost::Condition::Unreadable:
        break;
    }
    return tr("partly unreadable");
}

QString kind(const lost::Found &f)
{
    return f.type >= 0 ? filetypes::types()[f.type].name : QString();
}

struct Column {
    QString title;
    std::function<QString(const lost::Found &, const Row &)> value;
};

QVector<Column> columns(bool prints)
{
    QVector<Column> out = {
        {tr("Name"), [](const lost::Found &f, const Row &) { return f.name; }},
        {tr("Where it was"), [](const lost::Found &f, const Row &) { return f.folder; }},
        {tr("Kind"), [](const lost::Found &f, const Row &) { return kind(f); }},
        {tr("Size (bytes)"), [](const lost::Found &f, const Row &) { return f.nameOnly ? QString() : QString::number(f.size); }},
        {tr("Deleted"), [](const lost::Found &f, const Row &) { return f.deleted ? tr("yes") : tr("no"); }},
        {tr("Condition"), [](const lost::Found &f, const Row &) { return f.nameOnly ? tr("only its name is left") : condition(f.condition); }},
        {tr("Found by"), [](const lost::Found &f, const Row &) { return f.origin == lost::Origin::FileSystem ? tr("the file system's records") : tr("what's in it"); }},
        {tr("Created"), [](const lost::Found &f, const Row &) { return stamp(f.created); }},
        {tr("Changed"), [](const lost::Found &f, const Row &) { return stamp(f.modified); }},
        {tr("Last opened"), [](const lost::Found &f, const Row &) { return f.accessedDayOnly ? stamp(f.accessed).left(10) : stamp(f.accessed); }},
        {tr("Deleted at"), [](const lost::Found &f, const Row &) { return stamp(f.deletedAt); }},
        {tr("Its own date"), [](const lost::Found &f, const Row &r) { return stamp(r.spot.when > 0 ? r.spot.when : f.inside); }},
        {tr("Place"), [](const lost::Found &, const Row &r) { return r.place; }},
        {tr("Latitude"), [](const lost::Found &, const Row &r) { return r.spot.known ? QString::number(r.spot.latitude, 'f', 6) : QString(); }},
        {tr("Longitude"), [](const lost::Found &, const Row &r) { return r.spot.known ? QString::number(r.spot.longitude, 'f', 6) : QString(); }},
    };
    if (prints) {
        out.append(Column{QStringLiteral("MD5"), [](const lost::Found &, const Row &r) { return QString::fromLatin1(r.prints.md5); }});
        out.append(Column{QStringLiteral("SHA-1"), [](const lost::Found &, const Row &r) { return QString::fromLatin1(r.prints.sha1); }});
        out.append(Column{QStringLiteral("SHA-256"), [](const lost::Found &, const Row &r) { return QString::fromLatin1(r.prints.sha256); }});
    }
    out.append(Column{tr("Note"), [](const lost::Found &f, const Row &) { return f.note; }});
    return out;
}

bool anyPrints(const QVector<Row> &rows)
{
    return std::any_of(rows.cbegin(), rows.cend(), [](const Row &r) { return r.prints.done; });
}

} // namespace

Fingerprints fingerprints(const std::shared_ptr<lost::Source> &source, const lost::Found &file, const std::function<bool()> &stop)
{
    Fingerprints out;
    if (file.nameOnly)
        return out;
    QCryptographicHash md5(QCryptographicHash::Md5), sha1(QCryptographicHash::Sha1), sha256(QCryptographicHash::Sha256);
    lost::FileReader reader(source, file);
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    for (quint64 at = 0; at < file.size;) {
        if (stop && stop())
            return out;
        const qint64 n = reader.read(at, buffer.data(), buffer.size());
        if (n <= 0)
            return out;
        const QByteArrayView part(buffer.constData(), n);
        md5.addData(part);
        sha1.addData(part);
        sha256.addData(part);
        at += quint64(n);
    }
    out.md5 = md5.result().toHex();
    out.sha1 = sha1.result().toHex();
    out.sha256 = sha256.result().toHex();
    out.done = true;
    return out;
}

QVector<Row> collect(const std::shared_ptr<lost::Source> &source, const QVector<lost::Found> &files, const QVector<int> &indexes, bool prints,
                     const Progress &progress)
{
    QVector<Row> rows;
    rows.reserve(indexes.size());
    bool stopped = false;
    for (int n = 0; n < indexes.size() && !stopped; ++n) {
        const int i = indexes[n];
        if (i < 0 || i >= files.size())
            continue;
        const lost::Found &f = files[i];
        Row row;
        row.index = i;
        if (!f.nameOnly && f.size > 0) {
            lost::FileReader reader(source, f);
            row.spot = naming::where(f.type, f.size, [&reader](quint64 at, qint64 len) {
                QByteArray part(qsizetype(std::max<qint64>(0, len)), Qt::Uninitialized);
                part.truncate(qsizetype(std::max<qint64>(0, reader.read(at, part.data(), part.size()))));
                return part;
            });
            if (row.spot.known)
                row.place = places::describe(places::nearest(row.spot.latitude, row.spot.longitude));
            if (prints)
                row.prints = fingerprints(source, f, [&] { return progress && !progress(n, int(indexes.size())); });
        }
        rows.push_back(row);
        if (progress && !progress(n + 1, int(indexes.size())))
            stopped = true;
    }
    return rows;
}

QString html(const About &about, const QVector<lost::Found> &files, const QVector<Row> &rows)
{
    const auto e = [](const QString &s) { return s.toHtmlEscaped(); };
    int deleted = 0, located = 0;
    quint64 bytes = 0;
    for (const Row &r : rows) {
        deleted += files[r.index].deleted;
        located += r.spot.known;
        bytes += files[r.index].nameOnly ? 0 : files[r.index].size;
    }
    QString out = QStringLiteral("<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>%1</title><style>"
                                 "body{font-family:sans-serif;margin:24px;color:#111}h1{font-size:20px}table{border-collapse:collapse;font-size:12px}"
                                 "td,th{border:1px solid #bbb;padding:3px 6px;vertical-align:top;text-align:left}th{background:#eee}"
                                 ".about td:first-child{color:#555}.mono{font-family:monospace;word-break:break-all}</style></head><body>\n")
                      .arg(e(tr("Find Lost Files report: %1").arg(about.title)));
    out += QStringLiteral("<h1>%1</h1>\n<table class=\"about\">").arg(e(tr("What Find Lost Files found on %1").arg(about.title)));
    const QVector<std::pair<QString, QString>> facts = {
        {tr("Looked through"), about.source.isEmpty() ? about.title : about.title + QStringLiteral(" (") + about.source + QLatin1Char(')')},
        {tr("Report made"), about.made.toString(Qt::ISODate)},
        {tr("Made by"), QStringLiteral("DiskAnvil ") + about.version},
        {tr("Files"), tr("%1, %2 of them deleted, %3 in all").arg(rows.size()).arg(deleted).arg(formatSize(bytes))},
        {tr("Say where they were taken"), QString::number(located)},
        {tr("Fingerprints"), anyPrints(rows) ? tr("MD5, SHA-1 and SHA-256 of each file, as it was read from the drive")
                                             : tr("not worked out for this report")},
        {tr("Places"), tr("the nearest town or city, from GeoNames (geonames.org, CC BY 4.0)")},
    };
    for (const auto &[label, value] : facts)
        out += QStringLiteral("<tr><td>%1</td><td>%2</td></tr>").arg(e(label), e(value));
    out += QStringLiteral("</table>\n<p></p><table><tr>");
    const QVector<Column> cols = columns(anyPrints(rows));
    for (const Column &c : cols)
        out += QStringLiteral("<th>%1</th>").arg(e(c.title));
    out += QStringLiteral("</tr>\n");
    for (const Row &r : rows) {
        out += QStringLiteral("<tr>");
        for (const Column &c : cols) {
            const bool mono = c.title == QLatin1String("MD5") || c.title.startsWith(QLatin1String("SHA"));
            out += (mono ? QStringLiteral("<td class=\"mono\">%1</td>") : QStringLiteral("<td>%1</td>")).arg(e(c.value(files[r.index], r)));
        }
        out += QStringLiteral("</tr>\n");
    }
    out += QStringLiteral("</table>\n</body></html>\n");
    return out;
}

QByteArray csv(const QVector<lost::Found> &files, const QVector<Row> &rows)
{
    // Cells a spreadsheet would run as a formula get a ' first.
    const auto quote = [](QString s) {
        s.replace(QLatin1Char('\n'), QLatin1Char(' ')).replace(QLatin1Char('\r'), QLatin1Char(' '));
        if (s.startsWith(QLatin1Char('=')) || s.startsWith(QLatin1Char('+')) || s.startsWith(QLatin1Char('@')))
            s.prepend(QLatin1Char('\''));
        if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"')))
            s = QLatin1Char('"') + s.replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"');
        return s;
    };
    const QVector<Column> cols = columns(anyPrints(rows));
    QStringList lines;
    QStringList head;
    for (const Column &c : cols)
        head << quote(c.title);
    lines << head.join(QLatin1Char(','));
    for (const Row &r : rows) {
        QStringList cells;
        for (const Column &c : cols)
            cells << quote(c.value(files[r.index], r));
        lines << cells.join(QLatin1Char(','));
    }
    return "\xEF\xBB\xBF" + lines.join(QStringLiteral("\r\n")).toUtf8() + "\r\n";
}

} // namespace foundreport
