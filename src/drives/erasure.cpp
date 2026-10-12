// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "erasure.h"

#include "format.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QSaveFile>
#include <QStandardPaths>

namespace erasure {

QString storePath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/erasures.json");

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("erasure", text);
}

QJsonArray load()
{
    QFile f(storePath);
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).array() : QJsonArray();
}

void save(const QJsonArray &records)
{
    QDir().mkpath(QFileInfo(storePath).absolutePath());
    QSaveFile f(storePath);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(records).toJson());
        f.commit();
    }
}

} // namespace

QJsonObject Record::toJson() const
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("model"), model},
            {QStringLiteral("serial"), serial},
            {QStringLiteral("device"), device},
            {QStringLiteral("bus"), bus},
            {QStringLiteral("size"), QString::number(size)},
            {QStringLiteral("method"), method},
            {QStringLiteral("zeros"), zeros},
            {QStringLiteral("started"), started.toString(Qt::ISODate)},
            {QStringLiteral("finished"), finished.toString(Qt::ISODate)},
            {QStringLiteral("checked"), checked},
            {QStringLiteral("app"), app}};
}

Record Record::fromJson(const QJsonObject &j)
{
    Record r;
    r.id = j.value(QLatin1String("id")).toString();
    r.model = j.value(QLatin1String("model")).toString();
    r.serial = j.value(QLatin1String("serial")).toString();
    r.device = j.value(QLatin1String("device")).toString();
    r.bus = j.value(QLatin1String("bus")).toString();
    r.size = j.value(QLatin1String("size")).toString().toULongLong();
    r.method = j.value(QLatin1String("method")).toString();
    r.zeros = j.value(QLatin1String("zeros")).toBool();
    r.started = QDateTime::fromString(j.value(QLatin1String("started")).toString(), Qt::ISODate);
    r.finished = QDateTime::fromString(j.value(QLatin1String("finished")).toString(), Qt::ISODate);
    r.checked = j.value(QLatin1String("checked")).toString();
    r.app = j.value(QLatin1String("app")).toString();
    return r;
}

void remember(const Record &given)
{
    Record r = given;
    if (r.id.isEmpty()) {
        // Its number: from what it was, so it's the same whoever looks it up.
        const QByteArray what = (r.model + r.serial + r.method + r.finished.toString(Qt::ISODate)).toUtf8();
        r.id = QString::fromLatin1(QCryptographicHash::hash(what, QCryptographicHash::Sha256).toHex().left(12)).toUpper();
    }
    QJsonArray records = load();
    records.prepend(r.toJson());
    save(records);
}

QVector<Record> all()
{
    QVector<Record> out;
    for (const QJsonValue &v : load())
        out << Record::fromJson(v.toObject());
    return out;
}

void update(const Record &record)
{
    QJsonArray records = load();
    for (int i = 0; i < records.size(); ++i) {
        if (records[i].toObject().value(QLatin1String("id")).toString() == record.id)
            records[i] = record.toJson();
    }
    save(records);
}

QString certificateHtml(const Record &r, const QString &erasedBy, const QString &forWhom, const QString &notes)
{
    const QLocale l;
    const auto row = [](const QString &name, const QString &value) {
        return value.isEmpty() ? QString()
                               : QStringLiteral("<tr><td style=\"padding:4px 16px 4px 0; color:#555\">%1</td><td style=\"padding:4px 0\"><b>%2</b></td></tr>")
                                     .arg(name.toHtmlEscaped(), value.toHtmlEscaped());
    };
    QString html = QStringLiteral("<html><body style=\"font-family:sans-serif; font-size:11pt\">");
    html += QStringLiteral("<h1 style=\"font-size:20pt\">%1</h1>").arg(tr("Certificate of Erasure").toHtmlEscaped());
    html += QStringLiteral("<p>%1</p>").arg(tr("The drive below was erased with the method described here.").toHtmlEscaped());
    html += QStringLiteral("<h2 style=\"font-size:13pt\">%1</h2><table>").arg(tr("The drive").toHtmlEscaped());
    html += row(tr("Model"), r.model) + row(tr("Serial number"), r.serial) + row(tr("Size"), QStringLiteral("%1 (%2 bytes)").arg(formatSize(r.size)).arg(r.size))
          + row(tr("Connected as"), r.bus.isEmpty() ? r.device : QStringLiteral("%1 (%2)").arg(r.device, r.bus));
    html += QStringLiteral("</table><h2 style=\"font-size:13pt\">%1</h2><table>").arg(tr("The erasure").toHtmlEscaped());
    html += row(tr("Method"), r.method) + row(tr("Started"), l.toString(r.started, QLocale::LongFormat))
          + row(tr("Finished"), l.toString(r.finished, QLocale::LongFormat)) + row(tr("Checked"), r.checked) + row(tr("Done with"), r.app)
          + row(tr("Certificate number"), r.id);
    html += QStringLiteral("</table>");
    if (!forWhom.trimmed().isEmpty() || !erasedBy.trimmed().isEmpty() || !notes.trimmed().isEmpty()) {
        html += QStringLiteral("<h2 style=\"font-size:13pt\">%1</h2><table>").arg(tr("For").toHtmlEscaped());
        html += row(tr("Customer or order"), forWhom.trimmed()) + row(tr("Erased by"), erasedBy.trimmed()) + row(tr("Notes"), notes.trimmed());
        html += QStringLiteral("</table>");
    }
    html += QStringLiteral("<p style=\"margin-top:48px\">%1 ____________________________ &nbsp;&nbsp; %2 ______________</p>")
                .arg(tr("Signed").toHtmlEscaped(), tr("Date").toHtmlEscaped());
    html += QStringLiteral("</body></html>");
    return html;
}

} // namespace erasure
