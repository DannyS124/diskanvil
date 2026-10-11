// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "drivepower.h"

#include <QObject>
#include <QRegularExpression>

namespace {

const QString kStandby = QStringLiteral("ata-pm-standby");
const QString kApm = QStringLiteral("ata-apm-level");
const QString kWriteCache = QStringLiteral("ata-write-cache-enabled");
const QString kAam = QStringLiteral("ata-aam-level");
const QString kLookahead = QStringLiteral("ata-read-lookahead-enabled");

} // namespace

QVector<drivepower::Choice> drivepower::standbyChoices()
{
    return {{0, QObject::tr("Never")},
            {60, QObject::tr("After 5 minutes")},
            {120, QObject::tr("After 10 minutes")},
            {240, QObject::tr("After 20 minutes")},
            {241, QObject::tr("After 30 minutes")},
            {242, QObject::tr("After 1 hour")},
            {244, QObject::tr("After 2 hours")}};
}

QVector<drivepower::Choice> drivepower::apmChoices()
{
    return {{254, QObject::tr("Most performance")},
            {128, QObject::tr("Balanced (never spins down by itself)")},
            {1, QObject::tr("Most saving (spins down and parks the heads often)")}};
}

QVector<drivepower::Choice> drivepower::aamChoices()
{
    return {{254, QObject::tr("Fastest (loudest seeking)")}, {192, QObject::tr("In between")}, {128, QObject::tr("Quietest (slower seeking)")}};
}

QString drivepower::standbyText(int value)
{
    if (value == 0)
        return QObject::tr("never");
    if (value >= 1 && value <= 240) {
        const int seconds = value * 5;
        return seconds % 60 == 0 ? QObject::tr("%n minute(s)", nullptr, seconds / 60) : QObject::tr("%n second(s)", nullptr, seconds);
    }
    if (value >= 241 && value <= 251) {
        const int minutes = (value - 240) * 30;
        return minutes % 60 == 0 ? QObject::tr("%n hour(s)", nullptr, minutes / 60) : QObject::tr("%n minute(s)", nullptr, minutes);
    }
    if (value == 252)
        return QObject::tr("%n minute(s)", nullptr, 21);
    return QObject::tr("the drive's own time");
}

QVariantMap drivepower::configuration(const QVariantMap &current, const Changes &c)
{
    QVariantMap out = current;
    auto apply = [&out](const QString &key, int value, int low, int high) {
        if (value == kDriveDefault)
            out.remove(key);
        else if (value >= low && value <= high)
            out.insert(key, value);
    };
    apply(kStandby, c.standby, 0, 255);
    apply(kApm, c.apm, 1, 255);
    apply(kAam, c.aam, 128, 254);
    for (const auto &[key, value] : {qMakePair(kWriteCache, c.writeCache), qMakePair(kLookahead, c.lookahead)}) {
        if (value == 0 || value == 1)
            out.insert(key, value == 1);
        else if (value == kDriveDefault)
            out.remove(key);
    }
    return out;
}

QVariantMap drivepower::configuration(const QVariantMap &current, int standby, int apm, int writeCache)
{
    Changes c;
    c.standby = standby;
    c.apm = apm;
    c.writeCache = writeCache;
    return configuration(current, c);
}

QString drivepower::stateText(int state)
{
    switch (state) {
    case 0x00:
        return QObject::tr("Asleep (spun down)");
    case 0x40:
    case 0x41:
        return QObject::tr("Resting (the cache is in use)");
    case 0x80:
    case 0x81:
    case 0x82:
    case 0x83:
        return QObject::tr("Idle");
    case 0xff:
        return QObject::tr("Spinning");
    default:
        return QObject::tr("Unknown");
    }
}

drivepower::HiddenArea drivepower::parseHdparmN(const QString &output)
{
    HiddenArea a;
    static const QRegularExpression line(QStringLiteral("max sectors\\s*=\\s*(\\d+)\\s*/\\s*(\\d+)"));
    const QRegularExpressionMatch m = line.match(output);
    if (!m.hasMatch())
        return a;
    a.visible = m.captured(1).toULongLong();
    a.native = m.captured(2).toULongLong();
    a.known = a.native > 0 && a.visible > 0;
    return a;
}
