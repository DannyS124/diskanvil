// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Hardware Check: the parts of a PC people ask a repair shop about, besides its drives. The
// processor worked hard for a while, with its temperature and speed watched (overheating, a fan
// or paste that's given up, a processor slowing itself down to cope); the battery's wear (what it
// holds now against what it held new); and what's in it. The keyboard, screen and camera checks
// are in the window.
//
// Linux reads the kernel's own figures (/sys: hwmon, cpufreq, power_supply); Windows asks WMI.

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>

namespace hardware {

struct Battery {
    QString name;          // "BAT0", or the maker's model
    double designWh = -1;  // what it held new
    double fullWh = -1;    // what it holds when full now
    int cycles = -1;
    QString status;        // "Charging", "Discharging", "Full"
    int wearPercent() const; // how much it's lost; -1 not known
};

struct Summary {
    QString cpu;      // "AMD Ryzen 7 5800X 8-Core Processor"
    int threads = 0;
    quint64 memory = 0; // bytes
    QString board;    // maker and model of the PC or its board
    QString firmware; // BIOS / UEFI version and date
    QVector<Battery> batteries;
};
Summary read();
// The same from given folders (the tests): /proc/cpuinfo, /proc/meminfo, /sys/class/power_supply, /sys/class/dmi/id.
Summary readFrom(const QString &cpuinfo, const QString &meminfo, const QString &powerSupply, const QString &dmi);

// The processor's temperature now, °C (the hottest sensor it has); -1 when there's none to read.
double cpuTemperature();
double cpuTemperatureFrom(const QString &hwmon);
// Its speed now, MHz (the average over its cores), and the most it's rated for; -1 not known.
double cpuMhz();
double cpuMaxMhz();

struct Sample {
    double seconds = 0;
    double celsius = -1;
    double mhz = -1;
};

// What a stress run showed.
struct StressResult {
    bool completed = false;
    double idleCelsius = -1, maxCelsius = -1;
    double startMhz = -1, endMhz = -1, maxMhz = -1;
    QVector<Sample> samples;
    bool tooHot = false;     // 95 °C or more
    bool throttled = false;  // the speed fell a lot as it warmed up
};
QString describe(const StressResult &r);
// The verdict from the samples (the tests).
StressResult judge(QVector<Sample> samples, double idleCelsius, double maxMhz);

// Every thread kept busy for `seconds`, the temperature and speed read every second.
class Stress : public QObject
{
    Q_OBJECT
public:
    explicit Stress(int seconds);
    void cancel() { m_cancel = true; }

public slots:
    void run();

signals:
    void sample(const hardware::Sample &s);
    void finished(const hardware::StressResult &r);

private:
    int m_seconds;
    std::atomic<bool> m_cancel{false};
};

QString batteryText(const Battery &b);
// The whole check as a page of HTML (the report and its PDF).
QString html(const Summary &s, const StressResult *stress, const QStringList &other);

} // namespace hardware

Q_DECLARE_METATYPE(hardware::Sample)
Q_DECLARE_METATYPE(hardware::StressResult)
