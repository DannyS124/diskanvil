// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "smartdata.h"

#include <QCoreApplication>
#include <QHash>
#include <QtEndian>

namespace smart {

namespace {

constexpr int kEntries = 30; // attribute entries in a page, 12 bytes each from byte 2

QString tr(const char *text)
{
    return QCoreApplication::translate("UDisks", text);
}

// Sector counts are sometimes packed with other fields in the raw value (HGST and Hitachi put
// more in the upper bytes); the count is the low 16 bits. The same rule as with UDisks.
qint64 sectorCount(qint64 raw)
{
    return raw > 0xFFFF ? (raw & 0xFFFF) : raw;
}

quint64 le128low(const uchar *p)
{
    // The counters are 128-bit; nothing real goes past the low 64.
    return qFromLittleEndian<quint64>(p);
}

// The attributes' usual names (the ones smartmontools and UDisks use), for the table's
// technical column; the plain words come from health::describe().
QString nameOf(int id)
{
    static const QHash<int, const char *> names = {
        {1, "raw-read-error-rate"},          {2, "throughput-performance"},     {3, "spin-up-time"},
        {4, "start-stop-count"},             {5, "reallocated-sector-count"},   {7, "seek-error-rate"},
        {8, "seek-time-performance"},        {9, "power-on-hours"},             {10, "spin-retry-count"},
        {11, "calibration-retry-count"},     {12, "power-cycle-count"},         {13, "read-soft-error-rate"},
        {170, "available-reserved-space"},   {171, "program-fail-count"},       {172, "erase-fail-count"},
        {173, "wear-leveling-count"},        {174, "unexpected-power-loss"},    {177, "wear-leveling-count"},
        {179, "used-reserved-block-count"},  {181, "program-fail-count"},       {182, "erase-fail-count"},
        {183, "runtime-bad-block"},          {184, "end-to-end-error"},         {187, "reported-uncorrect"},
        {188, "command-timeout"},            {189, "high-fly-writes"},          {190, "airflow-temperature-celsius"},
        {191, "g-sense-error-rate"},         {192, "power-off-retract-count"},  {193, "load-cycle-count"},
        {194, "temperature-celsius-2"},      {195, "hardware-ecc-recovered"},   {196, "reallocated-event-count"},
        {197, "current-pending-sector"},     {198, "offline-uncorrectable"},    {199, "udma-crc-error-count"},
        {200, "multi-zone-error-rate"},      {202, "percent-lifetime-remain"},  {206, "flying-height"},
        {220, "disk-shift"},                 {222, "loaded-hours"},             {223, "load-retry-count"},
        {225, "load-cycle-count"},           {226, "load-in-time"},             {230, "head-amplitude"},
        {231, "temperature-celsius"},        {232, "endurance-remaining"},      {233, "power-on-seconds-2"},
        {235, "good-block-count"},           {240, "head-flying-hours"},        {241, "total-lbas-written"},
        {242, "total-lbas-read"},            {254, "free-fall-sensor"},
    };
    const char *name = names.value(id);
    return name ? QString::fromLatin1(name) : QStringLiteral("attribute-%1").arg(id);
}

double kelvin(quint16 k)
{
    return k > 0 ? k - 273.15 : -1;
}

} // namespace

QVector<SmartAttribute> ataAttributes(const QByteArray &data, const QByteArray &thresholds)
{
    QVector<SmartAttribute> out;
    if (data.size() < 512)
        return out;
    const auto *d = reinterpret_cast<const uchar *>(data.constData());
    const auto *t = thresholds.size() >= 512 ? reinterpret_cast<const uchar *>(thresholds.constData()) : nullptr;
    for (int i = 0; i < kEntries; ++i) {
        const uchar *e = d + 2 + i * 12;
        if (e[0] == 0)
            continue;
        SmartAttribute a;
        a.id = e[0];
        a.name = nameOf(a.id);
        a.value = e[3];
        a.worst = e[4];
        qint64 raw = 0;
        for (int b = 5; b >= 0; --b)
            raw = (raw << 8) | e[5 + b];
        // Each threshold sits in the entry with the same number in the threshold page.
        for (int k = 0; t && k < kEntries; ++k) {
            if (t[2 + k * 12] == e[0])
                a.threshold = t[2 + k * 12 + 1];
        }
        a.failing = a.threshold > 0 && a.value > 0 && a.value <= a.threshold;
        switch (a.id) {
        case 9: // power-on hours
            a.rawValue = raw & 0xFFFFFFFF;
            a.raw = tr("%L1 hours").arg(a.rawValue);
            break;
        case 5:
        case 197:
        case 198: // sectors: replaced, waiting, unreadable
            a.rawValue = sectorCount(raw);
            a.raw = tr("%L1 sectors").arg(a.rawValue);
            break;
        case 190:
        case 194: // the temperature now is the low byte
            a.rawValue = raw & 0xFF;
            a.raw = QStringLiteral("%1 °C").arg(a.rawValue);
            break;
        default:
            // Plain counts. Seagate packs three counters into 188; the low 16 bits matter.
            a.rawValue = a.id == 188 ? sectorCount(raw) : raw;
            a.raw = QString::number(raw);
        }
        out.push_back(a);
    }
    return out;
}

AtaSelftest ataSelftest(const QByteArray &data)
{
    AtaSelftest s;
    if (data.size() < 512)
        return s;
    const uchar b = uchar(data[363]);
    static const char *const status[] = {"success",          "aborted",     "interrupted", "fatal",          "error_unknown",
                                         "error_electrical", "error_servo", "error_read",  "error_handling"};
    const int code = b >> 4;
    if (code == 15) {
        s.status = QStringLiteral("inprogress");
        s.percentRemaining = (b & 0x0F) * 10;
    } else if (code < int(std::size(status))) {
        s.status = QString::fromLatin1(status[code]);
    }
    return s;
}

NvmeHealth nvmeHealth(const QByteArray &page)
{
    NvmeHealth h;
    if (page.size() < 512)
        return h;
    const auto *p = reinterpret_cast<const uchar *>(page.constData());
    h.valid = true;
    static const char *const warnings[] = {"spare", "temperature", "degraded", "readonly", "volatile_mem", "pmr_readonly"};
    for (int bit = 0; bit < 6; ++bit) {
        if (p[0] & (1 << bit))
            h.criticalWarnings << QString::fromLatin1(warnings[bit]);
    }
    h.temperatureC = kelvin(qFromLittleEndian<quint16>(p + 1));
    h.availableSpare = p[3];
    h.spareThreshold = p[4];
    h.percentUsed = p[5];
    h.bytesRead = le128low(p + 32) * 512000; // counted in thousands of 512-byte units
    h.bytesWritten = le128low(p + 48) * 512000;
    h.powerCycles = le128low(p + 112);
    h.powerOnHours = le128low(p + 128);
    h.unsafeShutdowns = le128low(p + 144);
    h.mediaErrors = le128low(p + 160);
    h.errorLogEntries = le128low(p + 176);
    h.warningTempMinutes = qFromLittleEndian<quint32>(p + 192);
    h.criticalTempMinutes = qFromLittleEndian<quint32>(p + 196);
    for (int i = 0; i < 8; ++i) {
        if (const quint16 k = qFromLittleEndian<quint16>(p + 200 + i * 2))
            h.sensorsC << kelvin(k);
    }
    return h;
}

void nvmeTemperatureLimits(const QByteArray &identify, double *warningC, double *criticalC)
{
    *warningC = *criticalC = -1;
    if (identify.size() < 270)
        return;
    const auto *p = reinterpret_cast<const uchar *>(identify.constData());
    *warningC = kelvin(qFromLittleEndian<quint16>(p + 266));
    *criticalC = kelvin(qFromLittleEndian<quint16>(p + 268));
}

} // namespace smart
