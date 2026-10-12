// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "health.h"

#include <QObject>
#include <QHash>
#include <QSettings>

#include <algorithm>

namespace {

QSettings settings()
{
    return QSettings(QStringLiteral("diskanvil"), QStringLiteral("diskanvil"));
}

qint64 rawOf(const QVector<SmartAttribute> &attributes, int id)
{
    for (const SmartAttribute &a : attributes) {
        if (a.id == id)
            return a.rawValue;
    }
    return -1;
}

const SmartAttribute *attribute(const QVector<SmartAttribute> &attributes, int id)
{
    for (const SmartAttribute &a : attributes) {
        if (a.id == id)
            return &a;
    }
    return nullptr;
}

void addReason(QVector<HealthReason> &reasons, HealthReason::Level level, const char *code, qint64 number, const QString &text)
{
    reasons.push_back({level, QLatin1String(code), number, text});
}

health::Verdict finish(QVector<HealthReason> reasons)
{
    std::stable_sort(reasons.begin(), reasons.end(), [](const HealthReason &a, const HealthReason &b) { return a.level > b.level; });
    health::Verdict v;
    v.reasons = reasons;
    const HealthReason::Level worst = reasons.isEmpty() ? HealthReason::Level::Note : reasons.first().level;
    if (reasons.isEmpty() || worst == HealthReason::Level::Note) {
        v.state = Health::State::Healthy;
        v.summary = QObject::tr("Healthy");
    } else if (worst == HealthReason::Level::Failing) {
        v.state = Health::State::Failing;
        v.summary = QObject::tr("Failing, back up now");
    } else {
        v.state = Health::State::Warning;
        v.summary = reasons.first().text.section(QLatin1String(". "), 0, 0); // the short part
    }
    return v;
}

} // namespace

health::AttributeInfo health::describe(int id)
{
    switch (id) {
    case 1: return {QObject::tr("Read error rate"), QObject::tr("How often reads needed fixing. Its meaning differs per maker, so it isn't judged."), false};
    case 3: return {QObject::tr("Spin-up time"), QObject::tr("How long the platters take to spin up."), false};
    case 4: return {QObject::tr("Starts and stops"), QObject::tr("How many times the platters started spinning."), false};
    case 5: return {QObject::tr("Replaced sectors"), QObject::tr("Weak sectors the drive swapped for spares. A few is normal wear; a growing number "
                                               "means the surface is going. One of the five numbers that most often go up before a "
                                               "drive fails."), true};
    case 7: return {QObject::tr("Seek error rate"), QObject::tr("How often the heads missed their track. Its meaning differs per maker."), false};
    case 9: return {QObject::tr("Hours powered on"), QObject::tr("How long the drive has been running in total."), false};
    case 10: return {QObject::tr("Spin-up retries"), QObject::tr("Times the platters didn't get up to speed on the first try: a mechanical or power "
                                               "problem."), true};
    case 12: return {QObject::tr("Power cycles"), QObject::tr("How many times the drive was switched on."), false};
    case 177: return {QObject::tr("Wear (life left)"), QObject::tr("For SSDs: how much of the flash memory's rated life is left, as the value column."), false};
    case 183: return {QObject::tr("Bad blocks found while running"), QObject::tr("Blocks the drive found bad during normal use."), false};
    case 184: return {QObject::tr("End-to-end errors"), QObject::tr("Data that got damaged inside the drive on its way between the cache and the "
                                                  "platters. Rare, and a bad sign when it goes up."), true};
    case 187: return {QObject::tr("Errors it couldn't fix"), QObject::tr("Reads the drive couldn't correct, even with its error correction. Part of "
                                                       "the data was lost. One of the five numbers that most often go up before a drive fails."), true};
    case 188: return {QObject::tr("Command timeouts"), QObject::tr("Commands that took too long. Often harmless (power or cable hiccups), but a "
                                                 "number that keeps going up is worth watching."), true};
    case 189: return {QObject::tr("High-fly writes"), QObject::tr("Writes where the head flew higher than it should."), false};
    case 190:
    case 194: return {QObject::tr("Temperature"), QObject::tr("The drive's temperature."), false};
    case 191: return {QObject::tr("Shocks"), QObject::tr("Times the drive felt a shock (dropped, bumped)."), false};
    case 192: return {QObject::tr("Emergency head parks"), QObject::tr("Times the power went off without the heads parking normally."), false};
    case 193: return {QObject::tr("Head loads"), QObject::tr("How many times the heads were parked and loaded again."), false};
    case 195: return {QObject::tr("Corrected errors"), QObject::tr("Errors the drive's error correction fixed."), false};
    case 196: return {QObject::tr("Replacement events"), QObject::tr("How many times the drive swapped sectors for spares."), true};
    case 197: return {QObject::tr("Unreadable sectors"), QObject::tr("Sectors the drive can't read right now. Rewriting them (Scan for Bad Sectors → "
                                                   "Repair) makes the drive swap them for spares; what was stored there is lost."), true};
    case 198: return {QObject::tr("Unreadable in offline scan"), QObject::tr("Sectors the drive's own background scan couldn't read."), true};
    case 199: return {QObject::tr("Connection errors"), QObject::tr("Data that got garbled between the drive and the PC. That's the cable or the "
                                                  "port, not the drive: try another cable or port if it goes up."), true};
    case 200: return {QObject::tr("Write error rate"), QObject::tr("How often writes needed fixing."), false};
    case 202: return {QObject::tr("Life left"), QObject::tr("For some SSDs: how much of the rated life is left."), false};
    case 223: return {QObject::tr("Head load retries"), QObject::tr("Times loading the heads needed another try."), false};
    case 231: return {QObject::tr("Life left"), QObject::tr("For some SSDs: how much of the rated life is left."), false};
    case 232: return {QObject::tr("Spare blocks left"), QObject::tr("For SSDs: how much spare flash is left."), false};
    case 233: return {QObject::tr("Wear"), QObject::tr("For SSDs: how worn the flash is; 100 is new."), false};
    case 240: return {QObject::tr("Head flying hours"), QObject::tr("Hours the heads spent over the platters."), false};
    case 241: return {QObject::tr("Data written"), QObject::tr("How much has been written to the drive. The unit differs per maker."), false};
    case 242: return {QObject::tr("Data read"), QObject::tr("How much has been read from the drive. The unit differs per maker."), false};
    default: return {};
    }
}

health::Verdict health::ata(const AtaInput &in)
{
    QVector<HealthReason> r;
    using L = HealthReason::Level;
    if (in.driveSaysFailing)
        addReason(r, L::Failing, "drive-failing", -1, QObject::tr("The drive itself says it's failing. Copy what you want to keep to another drive now."));
    if (in.failingNow > 0)
        addReason(r, L::Failing, "attribute-failing", in.failingNow,
            QObject::tr("%n of its health numbers is past the limit its maker set. Copy what you want to keep to another drive now.", nullptr,
               in.failingNow));

    const qint64 pending = rawOf(in.attributes, 197), offline = rawOf(in.attributes, 198);
    const qint64 reallocated = rawOf(in.attributes, 5), uncorrect = rawOf(in.attributes, 187), spin = rawOf(in.attributes, 10);
    if (pending > 0)
        addReason(r, L::Warning, "pending", pending,
            QObject::tr("%n unreadable sector(s). Scan for Bad Sectors can repair them; what was stored there is already lost.", nullptr, int(pending)));
    if (offline > 0 && offline != pending)
        addReason(r, L::Warning, "offline", offline, QObject::tr("%n sector(s) its own scan couldn't read. Keep backups.", nullptr, int(offline)));
    if (uncorrect > 0)
        addReason(r, L::Warning, "uncorrectable", uncorrect,
            QObject::tr("%n read error(s) it couldn't fix. Drives that have had these fail far more often: keep backups and think about "
               "replacing it.", nullptr, int(uncorrect)));
    if (reallocated > 0)
        addReason(r, L::Warning, "reallocated", reallocated,
            QObject::tr("%n sector(s) replaced. That's what drives are built to do, but keep backups and watch whether the number grows.",
               nullptr, int(reallocated)));
    if (spin > 0)
        addReason(r, L::Warning, "spin-retries", spin, QObject::tr("%n spin-up retry(s): a mechanical or power problem.", nullptr, int(spin)));

    // Counters that never go down: a warning only when they've grown since last seen.
    const qint64 endToEnd = rawOf(in.attributes, 184), timeouts = rawOf(in.attributes, 188), crc = rawOf(in.attributes, 199);
    auto grew = [&in](int id, qint64 now) { return in.seen.contains(id) && now > in.seen.value(id); };
    if (endToEnd > 0)
        addReason(r, grew(184, endToEnd) ? L::Warning : L::Note, "end-to-end", endToEnd,
            QObject::tr("%n end-to-end error(s): data damaged inside the drive.", nullptr, int(endToEnd)));
    if (crc > 0)
        addReason(r, grew(199, crc) ? L::Warning : L::Note, "crc", crc,
            grew(199, crc) ? QObject::tr("Connection problems: %n new error(s) between the drive and the PC. Check the cable or try another "
                                "port; the drive itself may be fine.", nullptr, int(crc - in.seen.value(199)))
                           : QObject::tr("%n connection error(s) in the past (cable or port).", nullptr, int(crc)));
    if (timeouts > 0)
        addReason(r, grew(188, timeouts) ? L::Warning : L::Note, "timeouts", timeouts,
            QObject::tr("%n command timeout(s).", nullptr, int(timeouts)));
    if (in.failedBefore > 0 && in.failingNow == 0)
        addReason(r, L::Note, "failed-before", in.failedBefore, QObject::tr("Some health numbers were past their limit before."));

    const double hot = in.ssd ? 70 : 55;
    if (in.temperatureC >= hot)
        addReason(r, L::Warning, "hot", qRound(in.temperatureC),
            QObject::tr("Running hot (%1 °C). Check the airflow around it.").arg(qRound(in.temperatureC)));

    Verdict v = finish(r);
    // SSD wear, where the drive reports it as a "life left" value.
    for (const int id : {231, 233, 177, 202}) {
        if (const SmartAttribute *a = attribute(in.attributes, id); a && in.ssd && a->value > 0 && a->value <= 100) {
            v.lifeLeft = a->value;
            break;
        }
    }
    return v;
}

health::Verdict health::nvme(const NvmeInput &in)
{
    QVector<HealthReason> r;
    using L = HealthReason::Level;
    static const QStringList serious = {QStringLiteral("spare"), QStringLiteral("degraded"), QStringLiteral("readonly"),
                                        QStringLiteral("volatile_mem"), QStringLiteral("pmr_readonly")};
    for (const QString &w : in.criticalWarnings) {
        if (serious.contains(w))
            addReason(r, L::Failing, "critical", -1,
                QObject::tr("The drive reports a critical problem (%1). Copy what you want to keep to another drive now.").arg(w));
    }
    if (in.availableSpare >= 0 && in.spareThreshold > 0 && in.availableSpare < in.spareThreshold && !in.criticalWarnings.contains(QStringLiteral("spare")))
        addReason(r, L::Failing, "spare", in.availableSpare,
            QObject::tr("It's out of spare blocks (%1% left). Copy what you want to keep to another drive now.").arg(in.availableSpare));
    else if (in.availableSpare >= 0 && in.spareThreshold > 0 && in.availableSpare <= in.spareThreshold + 10)
        addReason(r, L::Warning, "spare-low", in.availableSpare, QObject::tr("It's running low on spare blocks (%1% left).").arg(in.availableSpare));
    if (in.criticalWarnings.contains(QStringLiteral("temperature")) || (in.warningTempC > 0 && in.temperatureC >= in.warningTempC))
        addReason(r, L::Warning, "hot", qRound(in.temperatureC),
            QObject::tr("Running too hot (%1 °C). Check the airflow around it.").arg(qRound(in.temperatureC)));
    if (in.mediaErrors > 0)
        addReason(r, L::Warning, "media-errors", in.mediaErrors,
            QObject::tr("%n media error(s): data it couldn't read back correctly. Keep backups.", nullptr, int(in.mediaErrors)));
    if (in.percentUsed >= 100)
        addReason(r, L::Warning, "worn", in.percentUsed,
            QObject::tr("Past its rated life (%1% used). It may keep working, but keep backups and plan to replace it.").arg(in.percentUsed));
    return finish(r);
}

void health::add(Health &h, const HealthReason &reason)
{
    const auto at = std::find_if(h.reasons.begin(), h.reasons.end(), [&reason](const HealthReason &r) { return r.level < reason.level; });
    h.reasons.insert(at, reason);
    const Health::State state = reason.level == HealthReason::Level::Failing ? Health::State::Failing
        : reason.level == HealthReason::Level::Warning                     ? Health::State::Warning
                                                                           : Health::State::Healthy;
    if (state > h.state) {
        h.state = state;
        h.summary = state == Health::State::Failing ? QObject::tr("Failing, back up now") : reason.text.section(QLatin1String(". "), 0, 0);
    }
}

QString health::keyFor(const QString &driveId, const QString &drivePath)
{
    QString key = driveId.isEmpty() ? drivePath.section(QLatin1Char('/'), -1) : driveId;
    for (QChar &c : key) {
        // QSettings reads / and \ as group separators.
        if (!(c.isLetterOrNumber() && c.unicode() < 128) && c != QLatin1Char('-') && c != QLatin1Char('_') && c != QLatin1Char('.'))
            c = QLatin1Char('_');
    }
    return key;
}

QStringList health::signature(const Health &h)
{
    QStringList out;
    for (const HealthReason &r : h.reasons) {
        if (r.level != HealthReason::Level::Note)
            out << QStringLiteral("%1=%2").arg(r.code).arg(r.number);
    }
    return out;
}

bool health::worseThan(const Health &h, const QStringList &dismissed)
{
    QMap<QString, qint64> before;
    for (const QString &item : dismissed)
        before.insert(item.section(QLatin1Char('='), 0, 0), item.section(QLatin1Char('='), 1).toLongLong());
    for (const HealthReason &r : h.reasons) {
        if (r.level == HealthReason::Level::Note)
            continue;
        if (!before.contains(r.code) || r.number > before.value(r.code))
            return true;
    }
    return false;
}

QList<int> health::watchedCounters()
{
    return {184, 188, 199};
}

QMap<int, qint64> health::seen(const QString &driveKey)
{
    QMap<int, qint64> out;
    QSettings s = settings();
    for (const int id : watchedCounters()) {
        const QVariant v = s.value(QStringLiteral("health/%1/%2").arg(driveKey).arg(id));
        if (v.isValid())
            out.insert(id, v.toLongLong());
    }
    return out;
}

void health::remember(const QString &driveKey, const QVector<SmartAttribute> &attributes)
{
    QSettings s = settings();
    for (const int id : watchedCounters()) {
        const QString key = QStringLiteral("health/%1/%2").arg(driveKey).arg(id);
        const qint64 now = rawOf(attributes, id);
        if (now >= 0 && !s.contains(key))
            s.setValue(key, now);
    }
}

void health::acknowledge(const QString &driveKey, const QVector<SmartAttribute> &attributes)
{
    QSettings s = settings();
    for (const int id : watchedCounters()) {
        const qint64 now = rawOf(attributes, id);
        if (now >= 0)
            s.setValue(QStringLiteral("health/%1/%2").arg(driveKey).arg(id), now);
    }
}

bool health::hasUnreadableSpots(const Health &h)
{
    if (h.state == Health::State::Failing)
        return true;
    for (const HealthReason &r : h.reasons) {
        if (r.code == QLatin1String("pending") || r.code == QLatin1String("offline") || r.code == QLatin1String("uncorrectable")
            || r.code == QLatin1String("media-errors"))
            return true;
    }
    return false;
}

namespace health {

namespace {

QString historyKey(const QString &key)
{
    return QStringLiteral("selftests/") + key;
}

} // namespace

void noteSelftestStarted(const QString &key, const QString &kind)
{
    if (!key.isEmpty())
        settings().setValue(QStringLiteral("selftest-started/") + key, kind);
}

void noteSelftestStatus(const QString &key, const QString &status)
{
    if (key.isEmpty() || status.isEmpty())
        return;
    QSettings s = settings();
    const QString running = QStringLiteral("selftest-running/") + key;
    if (status == QLatin1String("inprogress")) {
        if (!s.value(running).toBool())
            s.setValue(running, true);
        return;
    }
    if (!s.value(running).toBool())
        return;
    s.remove(running);
    const QString started = QStringLiteral("selftest-started/") + key;
    const QString kind = s.value(started).toString();
    s.remove(started);
    QStringList history = s.value(historyKey(key)).toStringList();
    history.prepend(QStringLiteral("%1|%2|%3").arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate), kind, status));
    s.setValue(historyKey(key), history.mid(0, 20));
}

QVector<SelftestRecord> selftestHistory(const QString &key)
{
    QVector<SelftestRecord> out;
    for (const QString &line : settings().value(historyKey(key)).toStringList()) {
        const QStringList parts = line.split(QLatin1Char('|'));
        if (parts.size() == 3)
            out.push_back({QDateTime::fromString(parts[0], Qt::ISODate), parts[1], parts[2]});
    }
    return out;
}

QString selftestResult(const QString &status)
{
    if (status == QLatin1String("success"))
        return QObject::tr("passed");
    if (status.startsWith(QLatin1String("abort")) || status == QLatin1String("interrupted"))
        return QObject::tr("stopped before it finished");
    static const QHash<QString, const char *> failures = {
        {QStringLiteral("error_read"), QT_TR_NOOP("failed: it couldn't read part of the drive")},
        {QStringLiteral("error_electrical"), QT_TR_NOOP("failed: an electrical fault")},
        {QStringLiteral("error_servo"), QT_TR_NOOP("failed: the heads couldn't be positioned")},
        {QStringLiteral("error_handling"), QT_TR_NOOP("failed: the drive handled something wrong")},
        {QStringLiteral("known_seg_fail"), QT_TR_NOOP("failed: a part of the test didn't pass")},
        {QStringLiteral("unknown_seg_fail"), QT_TR_NOOP("failed: a part of the test didn't pass")},
        {QStringLiteral("fatal"), QT_TR_NOOP("failed")},
        {QStringLiteral("fatal_error"), QT_TR_NOOP("failed")},
    };
    const char *words = failures.value(status);
    return words ? QObject::tr(words) : QObject::tr("failed (%1)").arg(status);
}

QString selftestKind(const QString &kind)
{
    if (kind == QLatin1String("short"))
        return QObject::tr("short");
    if (kind == QLatin1String("extended"))
        return QObject::tr("long");
    if (kind == QLatin1String("conveyance"))
        return QObject::tr("conveyance");
    return QObject::tr("a self-test");
}

} // namespace health
