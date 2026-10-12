// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "farm.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>

namespace farm {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("farm", text);
}

qint64 number(const QJsonValue &v)
{
    return v.isDouble() ? qint64(v.toDouble()) : v.isString() ? v.toString().toLongLong() : -1;
}

} // namespace

bool keeps(const QString &model)
{
    const QString m = model.trimmed().toUpper();
    // Seagate's models start "ST" and a size (ST4000DM004, ST16000NM001G), or say Seagate.
    return m.startsWith(QLatin1String("SEAGATE")) || (m.startsWith(QLatin1String("ST")) && m.size() > 3 && m.at(2).isDigit());
}

QStringList command(const QString &device)
{
    return {QStringLiteral("smartctl"), QStringLiteral("-j"), QStringLiteral("-a"), QStringLiteral("-l"), QStringLiteral("farm"), device};
}

Result parse(const QByteArray &json)
{
    Result r;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    if (root.isEmpty()) {
        r.problem = tr("smartctl's answer couldn't be read");
        return r;
    }
    r.smartHours = number(root.value(QLatin1String("power_on_time")).toObject().value(QLatin1String("hours")));
    if (r.smartHours < 0) { // older drives: attribute 9's raw value
        for (const QJsonValue &a : root.value(QLatin1String("ata_smart_attributes")).toObject().value(QLatin1String("table")).toArray()) {
            if (a.toObject().value(QLatin1String("id")).toInt() == 9)
                r.smartHours = number(a.toObject().value(QLatin1String("raw")).toObject().value(QLatin1String("value")));
        }
    }
    const QJsonObject log = root.value(QLatin1String("seagate_farm_log")).toObject();
    if (log.value(QLatin1String("supported")) == QJsonValue(false)) {
        r.problem = tr("this drive doesn't keep one");
        return r;
    }
    const QJsonObject info = log.value(QLatin1String("page_1_drive_information")).toObject();
    if (info.isEmpty()) {
        // smartctl says why in its messages: no FARM log, no permission...
        QStringList said;
        for (const QJsonValue &m : root.value(QLatin1String("smartctl")).toObject().value(QLatin1String("messages")).toArray())
            said << m.toObject().value(QLatin1String("string")).toString();
        r.problem = said.isEmpty() ? tr("the drive gave no FARM record") : said.join(QStringLiteral("; "));
        return r;
    }
    r.read = true;
    r.farmHours = number(info.value(QLatin1String("poh")));
    r.spindleHours = number(info.value(QLatin1String("spoh")));
    r.headFlightHours = number(info.value(QLatin1String("head_flight_hours")));
    r.powerCycles = number(info.value(QLatin1String("power_cycle_count")));
    return r;
}

QString describe(const Result &r)
{
    if (!r.read)
        return tr("Its FARM record couldn't be read: %1.").arg(r.problem);
    const QLocale l;
    const QString hours = tr("Its own record (FARM) says %1 hours powered on; SMART says %2.").arg(l.toString(r.farmHours), l.toString(r.smartHours));
    if (r.reset())
        return hours + QLatin1Char(' ')
             + tr("Its SMART record was wiped: this drive has been used for about %1 days, whatever it was sold as.")
                   .arg(l.toString((r.farmHours - r.smartHours) / 24));
    return hours + QLatin1Char(' ') + tr("They agree: its history is what it says.");
}

} // namespace farm
