// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hardware.h"

#include "format.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLocale>
#include <QProcess>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace hardware {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("hardware", text);
}

QString tr(const char *text, const char *comment, int n)
{
    return QCoreApplication::translate("hardware", text, comment, n);
}

QString readText(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromLocal8Bit(f.readAll()).trimmed() : QString();
}

double readNumber(const QString &path, bool *ok = nullptr)
{
    bool good = false;
    const double v = readText(path).toDouble(&good);
    if (ok)
        *ok = good;
    return good ? v : -1;
}

#ifdef Q_OS_WIN
// One PowerShell CIM query, its values as "Name=Value" lines.
QStringList cim(const QString &script)
{
    QProcess ps;
    ps.start(QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-Command"), script});
    if (!ps.waitForFinished(20000))
        return {};
    return QString::fromLocal8Bit(ps.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}
#endif

} // namespace

int Battery::wearPercent() const
{
    if (designWh <= 0 || fullWh < 0)
        return -1;
    return std::clamp(int(std::lround((1 - fullWh / designWh) * 100)), 0, 100);
}

Summary readFrom(const QString &cpuinfo, const QString &meminfo, const QString &powerSupply, const QString &dmi)
{
    Summary s;
    for (const QString &line : readText(cpuinfo).split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("model name")) && s.cpu.isEmpty())
            s.cpu = line.section(QLatin1Char(':'), 1).trimmed();
        if (line.startsWith(QLatin1String("processor")))
            ++s.threads;
    }
    for (const QString &line : readText(meminfo).split(QLatin1Char('\n')))
        if (line.startsWith(QLatin1String("MemTotal:")))
            s.memory = line.section(QLatin1Char(':'), 1).simplified().section(QLatin1Char(' '), 0, 0).toULongLong() * 1024;
    const QString vendor = readText(dmi + QStringLiteral("/sys_vendor")), product = readText(dmi + QStringLiteral("/product_name"));
    const QString boardVendor = readText(dmi + QStringLiteral("/board_vendor")), board = readText(dmi + QStringLiteral("/board_name"));
    // Desktops often say "To Be Filled By O.E.M." for the PC itself: the board then.
    const bool filler = product.isEmpty() || product.contains(QLatin1String("O.E.M"), Qt::CaseInsensitive) || product == QLatin1String("System Product Name");
    s.board = filler ? QStringLiteral("%1 %2").arg(boardVendor, board).trimmed() : QStringLiteral("%1 %2").arg(vendor, product).trimmed();
    const QString bios = readText(dmi + QStringLiteral("/bios_version")), date = readText(dmi + QStringLiteral("/bios_date"));
    if (!bios.isEmpty())
        s.firmware = date.isEmpty() ? bios : tr("%1, from %2").arg(bios, date);
    for (const QString &name : QDir(powerSupply).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System)) {
        const QString dir = powerSupply + QLatin1Char('/') + name;
        if (readText(dir + QStringLiteral("/type")) != QLatin1String("Battery") || readText(dir + QStringLiteral("/scope")) == QLatin1String("Device"))
            continue; // a mouse's or a phone's isn't the PC's
        Battery b;
        const QString model = readText(dir + QStringLiteral("/model_name"));
        b.name = model.isEmpty() ? name : QStringLiteral("%1 (%2)").arg(model, name);
        bool ok = false;
        // Energy in µWh, or charge in µAh with the design voltage in µV.
        const double design = readNumber(dir + QStringLiteral("/energy_full_design"), &ok);
        if (ok) {
            b.designWh = design / 1e6;
            b.fullWh = readNumber(dir + QStringLiteral("/energy_full")) / 1e6;
        } else {
            const double volts = readNumber(dir + QStringLiteral("/voltage_min_design")) / 1e6;
            const double chargeDesign = readNumber(dir + QStringLiteral("/charge_full_design"));
            const double chargeFull = readNumber(dir + QStringLiteral("/charge_full"));
            if (volts > 0 && chargeDesign > 0) {
                b.designWh = chargeDesign / 1e6 * volts;
                b.fullWh = chargeFull > 0 ? chargeFull / 1e6 * volts : -1;
            }
        }
        b.cycles = int(readNumber(dir + QStringLiteral("/cycle_count")));
        if (b.cycles == 0)
            b.cycles = -1; // many batteries don't count: they say 0
        b.status = readText(dir + QStringLiteral("/status"));
        s.batteries << b;
    }
    return s;
}

Summary read()
{
#ifdef Q_OS_WIN
    Summary s;
    for (const QString &line : cim(QStringLiteral("$p=Get-CimInstance Win32_Processor|Select -First 1; 'cpu='+$p.Name; "
                                                  "$c=Get-CimInstance Win32_ComputerSystem; 'board='+$c.Manufacturer+' '+$c.Model; 'mem='+$c.TotalPhysicalMemory; "
                                                  "$b=Get-CimInstance Win32_BIOS; 'fw='+$b.SMBIOSBIOSVersion+', '+$b.ReleaseDate.ToString('yyyy-MM-dd'); "
                                                  "Get-CimInstance -Namespace root/wmi BatteryStaticData -EA 0|%{'design='+$_.DesignedCapacity+'|'+$_.DeviceName}; "
                                                  "Get-CimInstance -Namespace root/wmi BatteryFullChargedCapacity -EA 0|%{'full='+$_.FullChargedCapacity}; "
                                                  "Get-CimInstance -Namespace root/wmi BatteryCycleCount -EA 0|%{'cycles='+$_.CycleCount}"))) {
        const QString key = line.section(QLatin1Char('='), 0, 0), value = line.section(QLatin1Char('='), 1).trimmed();
        if (key == QLatin1String("cpu"))
            s.cpu = value;
        else if (key == QLatin1String("board"))
            s.board = value;
        else if (key == QLatin1String("mem"))
            s.memory = value.toULongLong();
        else if (key == QLatin1String("fw"))
            s.firmware = value;
        else if (key == QLatin1String("design")) {
            Battery b;
            b.designWh = value.section(QLatin1Char('|'), 0, 0).toDouble() / 1000; // mWh
            b.name = value.section(QLatin1Char('|'), 1);
            s.batteries << b;
        } else if (key == QLatin1String("full") && !s.batteries.isEmpty()) {
            s.batteries.last().fullWh = value.toDouble() / 1000;
        } else if (key == QLatin1String("cycles") && !s.batteries.isEmpty()) {
            s.batteries.last().cycles = value.toInt() > 0 ? value.toInt() : -1;
        }
    }
    s.threads = int(std::thread::hardware_concurrency());
    return s;
#else
    return readFrom(QStringLiteral("/proc/cpuinfo"), QStringLiteral("/proc/meminfo"), QStringLiteral("/sys/class/power_supply"),
                    QStringLiteral("/sys/class/dmi/id"));
#endif
}

double cpuTemperatureFrom(const QString &hwmon)
{
    // The processor's own sensors: Intel's coretemp, AMD's k10temp and zenpower, ARM's cpu_thermal.
    static const QStringList cpuDrivers = {QStringLiteral("coretemp"), QStringLiteral("k10temp"), QStringLiteral("zenpower"),
                                           QStringLiteral("cpu_thermal"), QStringLiteral("k8temp"), QStringLiteral("via_cputemp")};
    double hottest = -1;
    for (const QString &dir : QDir(hwmon).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System)) {
        const QString path = hwmon + QLatin1Char('/') + dir;
        if (!cpuDrivers.contains(readText(path + QStringLiteral("/name"))))
            continue;
        for (const QString &f : QDir(path).entryList({QStringLiteral("temp*_input")}, QDir::Files)) {
            const double milli = readNumber(path + QLatin1Char('/') + f);
            if (milli > 0)
                hottest = std::max(hottest, milli / 1000);
        }
    }
    return hottest;
}

double cpuTemperature()
{
#ifdef Q_OS_WIN
    // ACPI's thermal zones, in tenths of a kelvin; many PCs don't fill them in.
    double hottest = -1;
    for (const QString &line : cim(QStringLiteral("Get-CimInstance -Namespace root/wmi MSAcpi_ThermalZoneTemperature -EA 0|%{$_.CurrentTemperature}")))
        if (const double t = line.trimmed().toDouble(); t > 2732)
            hottest = std::max(hottest, t / 10 - 273.15);
    return hottest;
#else
    return cpuTemperatureFrom(QStringLiteral("/sys/class/hwmon"));
#endif
}

double cpuMhz()
{
#ifdef Q_OS_WIN
    const QStringList out = cim(QStringLiteral("(Get-Counter '\\Processor Information(_Total)\\% Processor Performance' -EA 0).CounterSamples.CookedValue; "
                                               "(Get-CimInstance Win32_Processor|Select -First 1).MaxClockSpeed"));
    return out.size() >= 2 ? out[0].trimmed().toDouble() / 100 * out[1].trimmed().toDouble() : -1;
#else
    double sum = 0;
    int n = 0;
    for (const QString &cpu : QDir(QStringLiteral("/sys/devices/system/cpu")).entryList({QStringLiteral("cpu[0-9]*")}, QDir::Dirs)) {
        const double khz = readNumber(QStringLiteral("/sys/devices/system/cpu/%1/cpufreq/scaling_cur_freq").arg(cpu));
        if (khz > 0) {
            sum += khz / 1000;
            ++n;
        }
    }
    return n ? sum / n : -1;
#endif
}

double cpuMaxMhz()
{
#ifdef Q_OS_WIN
    const QStringList out = cim(QStringLiteral("(Get-CimInstance Win32_Processor|Select -First 1).MaxClockSpeed"));
    return out.isEmpty() ? -1 : out[0].trimmed().toDouble();
#else
    const double khz = readNumber(QStringLiteral("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq"));
    return khz > 0 ? khz / 1000 : -1;
#endif
}

StressResult judge(QVector<Sample> samples, double idleCelsius, double maxMhz)
{
    StressResult r;
    r.samples = samples;
    r.idleCelsius = idleCelsius;
    r.maxMhz = maxMhz;
    if (samples.isEmpty())
        return r;
    for (const Sample &s : std::as_const(samples))
        r.maxCelsius = std::max(r.maxCelsius, s.celsius);
    // The speed over the first and the last fifth of the run.
    auto average = [](const QVector<Sample> &part) {
        double sum = 0;
        int n = 0;
        for (const Sample &s : part)
            if (s.mhz > 0) {
                sum += s.mhz;
                ++n;
            }
        return n ? sum / n : -1.0;
    };
    const int fifth = std::max<int>(1, int(samples.size()) / 5);
    r.startMhz = average(samples.mid(0, fifth));
    r.endMhz = average(samples.mid(samples.size() - fifth));
    r.tooHot = r.maxCelsius >= 95;
    // Slowing a fifth or more while it heats up (not a turbo settling, which is a few percent).
    r.throttled = r.startMhz > 0 && r.endMhz > 0 && r.endMhz < r.startMhz * 0.8 && r.maxCelsius >= 80;
    return r;
}

QString describe(const StressResult &r)
{
    QStringList out;
    if (r.maxCelsius > 0)
        out << tr("Its temperature went from %1 °C to %2 °C under full load.").arg(qRound(r.idleCelsius)).arg(qRound(r.maxCelsius));
    else
        out << tr("It doesn't tell its temperature (on Windows many PCs don't): only its speed was watched.");
    if (r.startMhz > 0 && r.endMhz > 0)
        out << tr("It ran at %1 MHz at first and %2 MHz at the end.").arg(qRound(r.startMhz)).arg(qRound(r.endMhz));
    if (r.tooHot && r.throttled)
        out << tr("It got far too hot and slowed itself down to cope: the cooling isn't keeping up. Clean the dust out; on an older PC "
                  "the paste between the processor and its cooler has likely dried out.");
    else if (r.tooHot)
        out << tr("It got very hot. Laptops run hot, but this is at the limit: clean the dust out of its vents and fan.");
    else if (r.throttled)
        out << tr("It slowed down a lot as it warmed up: its cooling or its power is holding it back.");
    else if (r.completed)
        out << tr("That's fine: it kept cool enough and kept its speed.");
    return out.join(QLatin1Char(' '));
}

Stress::Stress(int seconds)
    : m_seconds(seconds)
{
}

void Stress::run()
{
    const double idle = cpuTemperature();
    const double maxMhz = cpuMaxMhz();
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    const int threads = std::max(1, int(std::thread::hardware_concurrency()));
    for (int i = 0; i < threads; ++i) {
        workers.emplace_back([&stop, i] {
            // Floating-point work that can't be skipped: square roots fed back into themselves.
            volatile double x = 1.0 + i;
            while (!stop.load(std::memory_order_relaxed)) {
                for (int k = 0; k < 100000; ++k)
                    x = std::sqrt(x * x + 1.000001) * 0.999999;
            }
        });
    }
    QVector<Sample> samples;
    QElapsedTimer t;
    t.start();
    while (!m_cancel && t.elapsed() < qint64(m_seconds) * 1000) {
        QThread::msleep(1000);
        Sample s{t.elapsed() / 1000.0, cpuTemperature(), cpuMhz()};
        samples << s;
        Q_EMIT sample(s);
    }
    stop = true;
    for (std::thread &w : workers)
        w.join();
    StressResult r = judge(samples, idle, maxMhz);
    r.completed = !m_cancel;
    Q_EMIT finished(r);
}

QString batteryText(const Battery &b)
{
    QStringList parts;
    const int wear = b.wearPercent();
    if (wear >= 0)
        parts << tr("holds %1 Wh of the %2 Wh it held new: %3% worn").arg(QLocale().toString(b.fullWh, 'f', 1), QLocale().toString(b.designWh, 'f', 1)).arg(wear);
    else
        parts << tr("its wear isn't known (it doesn't say what it held new)");
    if (b.cycles > 0)
        parts << tr("%n charge cycle(s)", nullptr, b.cycles);
    QString text = b.name + QStringLiteral(": ") + parts.join(QStringLiteral(", ")) + QLatin1Char('.');
    if (wear >= 40)
        text += QStringLiteral(" ") + tr("Worth replacing: it runs out much sooner than it did.");
    else if (wear >= 20)
        text += QStringLiteral(" ") + tr("Getting tired, but still fine.");
    return text;
}

QString html(const Summary &s, const StressResult *stress, const QStringList &other)
{
    auto esc = [](const QString &t) { return t.toHtmlEscaped(); };
    QString out = QStringLiteral("<h2>%1</h2><table cellpadding=\"3\">").arg(esc(tr("Hardware check")));
    auto row = [&](const QString &k, const QString &v) {
        if (!v.isEmpty())
            out += QStringLiteral("<tr><td><b>%1</b></td><td>%2</td></tr>").arg(esc(k), esc(v));
    };
    row(tr("Checked"), QLocale().toString(QDateTime::currentDateTime(), QLocale::ShortFormat));
    row(tr("Computer"), s.board);
    row(tr("Firmware"), s.firmware);
    row(tr("Processor"), s.threads ? tr("%1 (%2 threads)").arg(s.cpu).arg(s.threads) : s.cpu);
    row(tr("Memory"), s.memory ? formatSize(s.memory) : QString());
    for (const Battery &b : s.batteries)
        row(tr("Battery"), batteryText(b));
    if (s.batteries.isEmpty())
        row(tr("Battery"), tr("none"));
    if (stress && !stress->samples.isEmpty())
        row(tr("Under load"), describe(*stress));
    for (const QString &o : other)
        row(o.section(QLatin1Char('\t'), 0, 0), o.section(QLatin1Char('\t'), 1));
    out += QStringLiteral("</table>");
    return out;
}

} // namespace hardware
