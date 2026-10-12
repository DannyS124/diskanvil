// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// The health verdict, from recorded SMART values: what counts, what's only a note, and
// counters that never go down only warning when they grow.

#include "testkit.h"

#include "../src/health/btrfscheck.h"
#include "../src/drives/drivepower.h"
#include "../src/health/firmware.h"
#include "../src/health/health.h"
#include "../src/drives/powerbox.h"
#include "../src/health/smartdata.h"
#include "../src/health/triage.h"
#include "../src/health/drivefacts.h"
#include "../src/health/farm.h"
#include "../src/drives/closedsets.h"
#include "../src/drives/blockio.h"
#include "../src/health/surfacescan.h"
#include "../src/tools/systemd.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>

namespace {

SmartAttribute attr(int id, qint64 raw, int value = 100, int threshold = 0)
{
    SmartAttribute a;
    a.id = id;
    a.rawValue = raw;
    a.value = value;
    a.worst = value;
    a.threshold = threshold;
    a.failing = threshold > 0 && value <= threshold;
    return a;
}

QStringList codes(const health::Verdict &v)
{
    QStringList out;
    for (const HealthReason &r : v.reasons)
        out << r.code + QLatin1Char(r.level == HealthReason::Level::Failing ? '!' : r.level == HealthReason::Level::Warning ? '?' : '.');
    return out;
}

} // namespace

void hiddenArea()
{
    using drivepower::parseHdparmN;
    const auto none = parseHdparmN(QStringLiteral("\n/dev/sda:\n max sectors   = 976773168/976773168, HPA is disabled\n"));
    const auto some = parseHdparmN(QStringLiteral("/dev/sdb:\n max sectors   = 976771055/976773168, HPA is enabled\n"));
    const auto odd = parseHdparmN(QStringLiteral("/dev/sdc:\n SG_IO: bad/missing sense data\n"));
    report(none.known && none.hidden() == 0 && some.known && some.hidden() == 2113 && !odd.known,
           QStringLiteral("hidden area: hdparm -N's answer is read, and what's hidden worked out"));
    // hdparm -I's sector size lines, in its own layout (identify.c: "\t%-31s %11u bytes").
    const auto switchable = drivepower::parseHdparmI(QStringLiteral("\tLogical  Sector size:                   512 bytes [ Supported: 512 4096 ]\n"
                                                                     "\tPhysical Sector size:                  4096 bytes\n"));
    const auto fixed = drivepower::parseHdparmI(QStringLiteral("\tLogical  Sector size:                   512 bytes\n\tPhysical Sector size:                  4096 bytes\n"));
    const auto old = drivepower::parseHdparmI(QStringLiteral("\tLogical/Physical Sector size:           512 bytes\n"));
    report(switchable.logical == 512 && switchable.physical == 4096 && switchable.canChange() && switchable.supported == QVector<int>{512, 4096}
               && fixed.logical == 512 && !fixed.canChange() && old.logical == 512 && old.physical == 512 && drivepower::parseHdparmI(QString()).logical == 0,
           QStringLiteral("sector size: hdparm -I's sizes, and which ones a drive can be switched to"));
    report(drivepower::setSectorSizeCommand(QStringLiteral("/dev/sdb"), 4096).join(QLatin1Char(' '))
               == QLatin1String("hdparm --set-sector-size 4096 --please-destroy-my-drive /dev/sdb"),
           QStringLiteral("sector size: hdparm's command to switch"));
    drivepower::Changes c;
    c.aam = 128;
    c.lookahead = 0;
    c.writeCache = drivepower::kDriveDefault;
    const QVariantMap next = drivepower::configuration({{QStringLiteral("ata-write-cache-enabled"), true}}, c);
    report(next.value(QStringLiteral("ata-aam-level")).toInt() == 128 && next.value(QStringLiteral("ata-read-lookahead-enabled")) == QVariant(false)
               && !next.contains(QStringLiteral("ata-write-cache-enabled")),
           QStringLiteral("drive settings: seek noise, reading ahead, and a switch back to the drive's own"));
}

// The raw SMART and NVMe pages DiskForge reads itself on Windows: decoded the way UDisks decodes
// them on Linux. The ATA page is QEMU's own (hw/ide/core.c), the drive the Windows VM sees.
void rawPages()
{
    QByteArray data(512, '\0'), thresholds(512, '\0');
    const uchar qemu[][12] = {
        {0x01, 0x03, 0x00, 0x64, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06}, {0x03, 0x03, 0x00, 0x64, 0x64, 0, 0, 0, 0, 0, 0, 0x00},
        {0x04, 0x02, 0x00, 0x64, 0x64, 0, 0, 0, 0, 0, 0, 0x14},                   {0x05, 0x03, 0x00, 0x64, 0x64, 0, 0, 0, 0, 0, 0, 0x24},
        {0x09, 0x03, 0x00, 0x64, 0x64, 0, 0, 0, 0, 0, 0, 0x00},                   {0x0c, 0x03, 0x00, 0x64, 0x64, 0, 0, 0, 0, 0, 0, 0x00},
        {190, 0x03, 0x00, 0x45, 0x45, 0x1f, 0x00, 0x1f, 0x1f, 0x00, 0x00, 0x32},
    };
    for (int i = 0; i < int(std::size(qemu)); ++i) {
        // QEMU keeps each threshold in the last byte of its table; the drive hands it over apart.
        memcpy(data.data() + 2 + i * 12, qemu[i], 11);
        thresholds[2 + i * 12] = char(qemu[i][0]);
        thresholds[2 + i * 12 + 1] = char(qemu[i][11]);
    }
    // And a drive that's had a bad time: 300 hours, 1234 replaced sectors at their limit.
    const uchar worn[12] = {0x05, 0x33, 0x00, 0x24, 0x24, 0xd2, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
    memcpy(data.data() + 2 + 7 * 12, worn, 12);
    data[2 + 7 * 12] = char(197);
    data[2 + 4 * 12 + 5] = char(0x2c); // power-on: 300 hours
    data[2 + 4 * 12 + 6] = char(0x01);
    thresholds[2 + 7 * 12] = char(197);
    thresholds[2 + 7 * 12 + 1] = char(0x30);
    const QVector<SmartAttribute> a = smart::ataAttributes(data, thresholds);
    QStringList seen;
    for (const SmartAttribute &x : a)
        seen << QStringLiteral("%1:%2/%3/%4 %5%6").arg(x.id).arg(x.value).arg(x.threshold).arg(x.rawValue).arg(x.raw, x.failing ? QStringLiteral(" failing") : QString());
    report(seen == QStringList({QStringLiteral("1:100/6/0 0"), QStringLiteral("3:100/0/0 0"), QStringLiteral("4:100/20/0 0"), QStringLiteral("5:100/36/0 0 sectors"),
                                QStringLiteral("9:100/0/300 300 hours"), QStringLiteral("12:100/0/0 0"), QStringLiteral("190:69/50/31 31 °C"),
                                QStringLiteral("197:36/48/1234 1,234 sectors failing")}),
           QStringLiteral("ATA SMART pages: QEMU's attributes, hours, sectors, temperature, a failing one"), seen.join(QStringLiteral(", ")));

    QByteArray testing(512, '\0');
    testing[363] = char(0xF3);
    QByteArray readError(512, '\0');
    readError[363] = char(0x70);
    report(smart::ataSelftest(testing).status == QLatin1String("inprogress") && smart::ataSelftest(testing).percentRemaining == 30
               && smart::ataSelftest(readError).status == QLatin1String("error_read") && smart::ataSelftest(QByteArray(512, '\0')).status == QLatin1String("success"),
           QStringLiteral("ATA self-test byte: running, failed reading, done"));

    QByteArray log(512, '\0');
    log[0] = char(0x05); // spare below its threshold, reliability degraded
    qToLittleEndian<quint16>(320, log.data() + 1);
    log[3] = char(80);
    log[4] = char(10);
    log[5] = char(3);
    qToLittleEndian<quint64>(1000, log.data() + 32);
    qToLittleEndian<quint64>(42, log.data() + 128);
    qToLittleEndian<quint64>(7, log.data() + 160);
    qToLittleEndian<quint16>(310, log.data() + 200);
    qToLittleEndian<quint16>(330, log.data() + 202);
    const smart::NvmeHealth h = smart::nvmeHealth(log);
    report(h.valid && h.criticalWarnings == QStringList({QStringLiteral("spare"), QStringLiteral("degraded")}) && qRound(h.temperatureC) == 47
               && h.availableSpare == 80 && h.spareThreshold == 10 && h.percentUsed == 3 && h.bytesRead == 512000000ull && h.powerOnHours == 42
               && h.mediaErrors == 7 && h.sensorsC.size() == 2 && qRound(h.sensorsC[1]) == 57,
           QStringLiteral("NVMe health log: warnings, temperature, spare, wear, counters, sensors"));
    QByteArray identify(4096, '\0');
    qToLittleEndian<quint16>(343, identify.data() + 266);
    qToLittleEndian<quint16>(358, identify.data() + 268);
    double warning = 0, critical = 0;
    smart::nvmeTemperatureLimits(identify, &warning, &critical);
    report(qRound(warning) == 70 && qRound(critical) == 85, QStringLiteral("NVMe identify: the drive's own temperature limits"));
    const bool before = smart::nvmeCanSelftest(identify);
    qToLittleEndian<quint16>(0x0017, identify.data() + 256); // security, format, firmware, self-test
    report(!before && smart::nvmeCanSelftest(identify), QStringLiteral("NVMe identify: whether the drive does self-tests"));

    // The self-test log: one running (40 % done), then results newest first.
    QByteArray tests(564, '\0');
    tests[0] = char(0x02);
    tests[1] = char(40);
    const smart::NvmeSelftest running = smart::nvmeSelftest(tests);
    tests[0] = 0;
    tests[4] = char(0x27); // an extended test, a part of it failed
    const smart::NvmeSelftest failed = smart::nvmeSelftest(tests);
    tests[4] = char(0x11); // a short one, stopped
    const smart::NvmeSelftest stopped = smart::nvmeSelftest(tests);
    tests[4] = char(0x10);
    const smart::NvmeSelftest passed = smart::nvmeSelftest(tests);
    tests[4] = char(0x0F);
    report(running.status == QLatin1String("inprogress") && running.percentRemaining == 60 && failed.status == QLatin1String("known_seg_fail")
               && stopped.status == QLatin1String("aborted") && passed.status == QLatin1String("success")
               && smart::nvmeSelftest(tests).status.isEmpty() && smart::nvmeSelftest(QByteArray()).status.isEmpty(),
           QStringLiteral("NVMe self-test log: running, a part failed, stopped, passed, never ran"));
    report(smart::ataAttributes(QByteArray(100, '\x7f'), {}).isEmpty() && !smart::nvmeHealth(QByteArray(10, '\0')).valid,
           QStringLiteral("SMART pages too short to be real are left alone"));
}

// "What happened?": honest odds and the right first step for each kind of drive.
void triageAdvice()
{
    using triage::Symptom;
    using Odds = triage::Advice::Odds;
    triage::Drive ssd;
    ssd.known = true;
    ssd.name = QStringLiteral("Fast SSD");
    ssd.ssd = true;
    ssd.trim = triage::Trim::Continuous;
    triage::Drive weekly = ssd, off = ssd;
    weekly.trim = triage::Trim::Weekly;
    off.trim = triage::Trim::Off;
    triage::Drive hdd;
    hdd.known = true;
    hdd.name = QStringLiteral("Big HDD");
    triage::Drive card = hdd;
    card.removable = true;
    triage::Drive failing = hdd;
    failing.failing = true;
    triage::Drive system = hdd;
    system.system = true;
    report(triage::advise(Symptom::Deleted, ssd).odds == Odds::Poor && triage::advise(Symptom::Deleted, weekly).odds == Odds::Fair
               && triage::advise(Symptom::Deleted, off).odds == Odds::Good && triage::advise(Symptom::Deleted, hdd).odds == Odds::Good
               && triage::advise(Symptom::Deleted, card).odds == Odds::Good,
           QStringLiteral("what happened: deleted files' odds, by how the drive erases them (TRIM)"));
    const triage::Advice rescue = triage::advise(Symptom::Deleted, failing);
    report(rescue.tools.value(0) == QLatin1String("rescue-copy") && rescue.odds == Odds::Fair && rescue.dont.join(' ').contains(QLatin1String("Don't check, repair")),
           QStringLiteral("what happened: on a failing drive, a Rescue Copy comes first"), rescue.tools.join(','));
    report(triage::advise(Symptom::Deleted, system).odds_why.contains(QLatin1String("DiskForge Live")),
           QStringLiteral("what happened: the system's own drive is best looked at from DiskForge Live"));
    report(triage::advise(Symptom::Formatted, ssd).odds == Odds::Poor && triage::advise(Symptom::Formatted, card).odds == Odds::Good,
           QStringLiteral("what happened: a formatted SSD is mostly erased, a formatted card isn't"));
    const triage::Advice raw = triage::advise(Symptom::WontOpen, hdd);
    report(raw.dont.join(' ').contains(QLatin1String("Don't format it")) && raw.dont.join(' ').contains(QLatin1String("chkdsk"))
               && raw.tools == QStringList{QStringLiteral("health"), QStringLiteral("lost-files")},
           QStringLiteral("what happened: a drive that won't open isn't formatted or repaired"));
    report(triage::advise(Symptom::PartitionGone, hdd).tools.value(0) == QLatin1String("recover-partitions")
               && triage::advise(Symptom::WindowsWontStart, triage::Drive()).tools.value(0) == QLatin1String("rescue-usb")
               && triage::advise(Symptom::Ransomware, hdd).dont.value(0).contains(QLatin1String("pay"))
               && triage::advise(Symptom::Deleted, triage::Drive()).odds == Odds::Unknown,
           QStringLiteral("what happened: each one's first tool, and no odds without a drive"));
    Disk spinning;
    spinning.rotationRate = 7200;
    Disk noTrim;
    noTrim.rotationRate = 0;
    noTrim.discard = false;
    report(triage::trimOf(spinning) == triage::Trim::NotSsd && triage::trimOf(noTrim) == triage::Trim::Unsupported,
           QStringLiteral("what happened: TRIM only matters on an SSD that does it"));
}

// Beyond the health record: eMMC wear, alignment, and what's known about some models.
void driveFacts()
{
    QTemporaryDir sys;
    QDir().mkpath(sys.filePath(QStringLiteral("mmcblk0/device")));
    const auto write = [&sys](const QString &name, const QByteArray &text) {
        QFile f(sys.filePath(QStringLiteral("mmcblk0/device/") + name));
        return f.open(QIODevice::WriteOnly) && f.write(text) == text.size();
    };
    write(QStringLiteral("life_time"), "0x02 0x04\n");
    write(QStringLiteral("pre_eol_info"), "0x02\n");
    const drivefacts::EmmcLife life = drivefacts::emmcLife(QStringLiteral("/dev/mmcblk0"), sys.path());
    report(life.known && life.usedFrom == 30 && life.usedTo == 40 && life.reserve == 2 && drivefacts::describe(life).contains(QLatin1String("80% used"))
               && !drivefacts::emmcLife(QStringLiteral("/dev/sda"), sys.path()).known,
           QStringLiteral("drive facts: an eMMC drive's wear, the worse of its two estimates, and its spare blocks"), drivefacts::describe(life));
    Disk disk;
    Volume good, bad, extended;
    good.number = 1;
    good.offset = 1024 * 1024;
    bad.number = 2;
    bad.offset = 63 * 512;
    extended.number = 3;
    extended.offset = 1000;
    extended.isContainer = true;
    disk.volumes = {good, bad, extended};
    const QStringList off = drivefacts::misaligned(disk);
    report(off.size() == 1 && off[0].startsWith(QLatin1String("Partition 2")), QStringLiteral("drive facts: a partition off a 4 KiB boundary is found"),
           off.join(QLatin1Char(',')));
    const auto kinds = [](const QString &model, const QString &firmware) {
        QStringList out;
        for (const drivefacts::Fact &f : drivefacts::aboutModel(model, firmware))
            out << QString::number(int(f.kind));
        return out.join(QLatin1Char(','));
    };
    report(kinds(QStringLiteral("WDC WD40EFAX-68JH4N1"), QStringLiteral("83.00A83")) == QLatin1String("0")
               && kinds(QStringLiteral("WDC WD40EFRX-68N32N0"), QStringLiteral("82.00A82")).isEmpty()
               && kinds(QStringLiteral("ST8000DM004-2CX188"), QStringLiteral("0001")) == QLatin1String("0"),
           QStringLiteral("drive facts: SMR models from the makers' lists, and their CMR siblings not"));
    report(kinds(QStringLiteral("Samsung SSD 980 PRO 1TB"), QStringLiteral("3B2QGXA7")) == QLatin1String("1")
               && kinds(QStringLiteral("Samsung SSD 980 PRO 1TB"), QStringLiteral("5B2QGXA7")).isEmpty()
               && kinds(QStringLiteral("Samsung SSD 990 PRO 2TB"), QStringLiteral("1B2QJXD7")) == QLatin1String("1")
               && kinds(QStringLiteral("Samsung SSD 990 PRO 2TB"), QStringLiteral("4B2QJXD7")).isEmpty()
               && kinds(QStringLiteral("ST3000DM001-1ER166"), QStringLiteral("CC26")) == QLatin1String("2"),
           QStringLiteral("drive facts: firmware with a known bug is pointed out until it's updated"));
    const QVector<drivefacts::Fact> tey = drivefacts::aboutModel(QStringLiteral("TOSHIBA MG08ACA16TEY"), QStringLiteral("0102"));
    report(tey.size() == 1 && tey[0].text.contains(QLatin1String("2.37")) && tey[0].text.contains(QLatin1String("Backblaze"))
               && kinds(QStringLiteral("WDC  WUH721414ALE6L4"), QStringLiteral("LDGNW07G")) == QLatin1String("2")
               && kinds(QStringLiteral("HGST HTS545050A7E380"), QStringLiteral("GG2OACA0")).isEmpty(),
           QStringLiteral("drive facts: Backblaze's failure rate for models in their records, the closest name winning"), tey.value(0).text);
}

// Seagate's FARM record against SMART's: a wiped SMART history shows up.
void farmHistory()
{
    const farm::Result reset = farm::parse(R"({"power_on_time":{"hours":3},"seagate_farm_log":{"page_1_drive_information":
        {"poh":24267,"spoh":24300,"head_flight_hours":24100,"power_cycle_count":61}}})");
    report(reset.read && reset.smartHours == 3 && reset.farmHours == 24267 && reset.reset() && farm::describe(reset).contains(QLatin1String("wiped")),
           QStringLiteral("FARM: SMART says 3 hours, FARM 24,267: the SMART record was wiped, and it says so"), farm::describe(reset));
    const farm::Result honest = farm::parse(R"({"ata_smart_attributes":{"table":[{"id":9,"raw":{"value":1500}}]},
        "seagate_farm_log":{"page_1_drive_information":{"poh":1502}}})");
    report(honest.read && honest.smartHours == 1500 && !honest.reset() && farm::describe(honest).contains(QLatin1String("agree")),
           QStringLiteral("FARM: when they agree (SMART's hours from attribute 9), it says so"));
    const farm::Result hgst = farm::parse(R"({"power_on_time":{"hours":1986},"seagate_farm_log":{"supported":false}})");
    const farm::Result denied = farm::parse(R"({"smartctl":{"messages":[{"string":"Smartctl open device: /dev/sda failed: Permission denied","severity":"error"}]}})");
    report(!hgst.read && hgst.problem.contains(QLatin1String("doesn't keep")) && !denied.read && denied.problem.contains(QLatin1String("Permission denied"))
               && !farm::parse("not json").read,
           QStringLiteral("FARM: a drive without one, and smartctl without the rights, each say why"), hgst.problem + QStringLiteral(" | ") + denied.problem);
    report(farm::keeps(QStringLiteral("ST16000NM001G-2KK103")) && farm::keeps(QStringLiteral("Seagate BarraCuda")) && !farm::keeps(QStringLiteral("HGST HTS545050A7E380"))
               && !farm::keeps(QStringLiteral("STORAGE DEVICE")),
           QStringLiteral("FARM: asked of Seagate's drives only"));
}

// SSD refresh: which reads were slow for the drive, and writing them back unchanged.
void slowRefresh()
{
    BlockMapData map(64 * 1024 * 1024, false);
    QVector<ReadSample> samples;
    for (quint64 off = 0; off < 64ull * 1024 * 1024; off += 1024 * 1024)
        samples.append({off, 1024 * 1024, quint32(off == 5 * 1024 * 1024 || off == 6 * 1024 * 1024 || off == 40 * 1024 * 1024 ? 300 : 2), true});
    samples.append({50ull * 1024 * 1024, 1024 * 1024, 400, false}); // unreadable: Repair's, not this
    map.add(samples);
    const auto ranges = slowRanges(samples, map);
    report(ranges.size() == 2 && ranges[0] == qMakePair(quint64(5 * 1024 * 1024), quint64(2 * 1024 * 1024)) && ranges[1].first == 40 * 1024 * 1024,
           QStringLiteral("SSD refresh: slow reads are found, neighbours joined, unreadable ones left out"), QString::number(ranges.size()));

    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ssd.img"));
    QByteArray data(8 * 1024 * 1024, '\0');
    for (qsizetype i = 0; i < data.size(); ++i)
        data[i] = char((i * 131 + i / 4096) & 0xFF);
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(data);
        f.close();
    }
    const int fd = blockio::openFile(path, blockio::FileMode::Update);
    RefreshResult result;
    {
        // A range past the end can't be read: it's counted and left alone.
        SlowRefresh refresh(fd, {{1024 * 1024, 3 * 1024 * 1024}, {7 * 1024 * 1024, 1024 * 1024}, {20 * 1024 * 1024, 1024 * 1024}});
        QObject::connect(&refresh, &SlowRefresh::finished, [&result](const RefreshResult &r) { result = r; });
        refresh.run();
    }
    QByteArray after;
    if (f.open(QIODevice::ReadOnly)) {
        after = f.readAll();
        f.close();
    }
    report(result.error.isEmpty() && result.bytes == 4 * 1024 * 1024 && result.unreadable == 1 && after == data,
           QStringLiteral("SSD refresh: each slow piece is written back unchanged, and what can't be read is left alone"),
           QStringLiteral("%1 bytes, %2 unreadable, same: %3 %4").arg(result.bytes).arg(result.unreadable).arg(after == data).arg(result.error));
}

// Drives that belong together but aren't opened: RAID members, Windows dynamic disks.
void closedSets()
{
    const auto volume = [](const QString &device, const QString &fs, const QString &uuid, const QString &label, const QString &memberOf,
                           const QString &partType = QString()) {
        Volume v;
        v.device = device;
        v.fsType = fs;
        v.uuid = uuid;
        v.label = label;
        v.raidMemberOf = memberOf;
        v.partType = partType;
        return v;
    };
    Disk a, b, c, e;
    a.device = QStringLiteral("/dev/sdb");
    a.volumes = {volume(QStringLiteral("/dev/sdb1"), QStringLiteral("linux_raid_member"), QStringLiteral("u-sys"), QStringLiteral("DiskStation:0"), QStringLiteral("md")),
                 volume(QStringLiteral("/dev/sdb5"), QStringLiteral("linux_raid_member"), QStringLiteral("u-data"), QStringLiteral("DiskStation:2"), QString())};
    b.device = QStringLiteral("/dev/sdc");
    b.volumes = {volume(QStringLiteral("/dev/sdc5"), QStringLiteral("linux_raid_member"), QStringLiteral("u-data"), QStringLiteral("DiskStation:2"), QString()),
                 volume(QStringLiteral("/dev/sdc6"), QStringLiteral("linux_raid_member"), QStringLiteral("u-run"), QStringLiteral("box:1"), QStringLiteral("md127"))};
    c.device = QStringLiteral("/dev/sdd");
    c.volumes = {volume(QStringLiteral("/dev/sdd1"), QString(), QString(), QString(), QString(), QStringLiteral("5808C8AA-7E8F-42E0-85D2-E1E90434CFB3")),
                 volume(QStringLiteral("/dev/sdd2"), QString(), QString(), QString(), QString(), QStringLiteral("af9b60a0-1431-4f62-bc68-3311714a69ad"))};
    e.device = QStringLiteral("/dev/sde");
    e.volumes = {volume(QStringLiteral("/dev/sde1"), QString(), QString(), QString(), QString(), QStringLiteral("0x42"))};
    const QVector<closedsets::Set> sets = closedsets::find({a, b, c, e}, false);
    QStringList said;
    for (const auto &s : sets)
        said << closedsets::describe(s);
    report(sets.size() == 3 && sets[0].name == QLatin1String("DiskStation:0") && sets[1].members == QStringList{QStringLiteral("/dev/sdb5"), QStringLiteral("/dev/sdc5")}
               && sets[2].kind == closedsets::Set::Kind::Dynamic && sets[2].members == QStringList{QStringLiteral("/dev/sdd"), QStringLiteral("/dev/sde")},
           QStringLiteral("closed sets: a NAS's arrays by their members (a running one left out), and Windows dynamic disks"), said.join(QStringLiteral(" | ")));
    report(closedsets::find({c}, true).isEmpty(), QStringLiteral("closed sets: dynamic disks whose volumes are open already aren't offered again"));
    const QString script = closedsets::openScript(sets);
    report(script.contains(QLatin1String("mdadm --assemble --scan --readonly --run")) && script.contains(QLatin1String("vgchange -ay"))
               && script.contains(QLatin1String("ldmtool create all")) && !closedsets::openScript({sets[2]}).contains(QLatin1String("mdadm")),
           QStringLiteral("closed sets: opened read-only, each program only for what needs it"), script);
    report(closedsets::missing(sets, [](const QString &p) { return p == QLatin1String("mdadm"); }) == QStringList{QStringLiteral("ldmtool")},
           QStringLiteral("closed sets: a program that isn't here is named"));
}

void healthTests()
{
    hiddenArea();
    triageAdvice();
    driveFacts();
    farmHistory();
    slowRefresh();
    closedSets();
    rawPages();
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(config.path()));

    // This PC's HGST, as UDisks reported it: 16 replaced sectors, 4 command timeouts (packed).
    health::AtaInput hgst;
    hgst.attributes = {attr(5, 16), attr(10, 0), attr(184, 0), attr(187, 0), attr(188, 4), attr(196, 2), attr(197, 0),
                       attr(198, 0), attr(199, 0)};
    hgst.temperatureC = 26;
    health::Verdict v = health::ata(hgst);
    report(v.state == Health::State::Warning && codes(v) == QStringList{QStringLiteral("reallocated?"), QStringLiteral("timeouts.")}
               && v.summary.contains(QLatin1String("16")),
           QStringLiteral("the HGST: replaced sectors warn, old timeouts are only a note"), codes(v).join(QLatin1Char(' ')) + QStringLiteral(" | ") + v.summary);

    health::AtaInput clean;
    clean.attributes = {attr(5, 0), attr(197, 0), attr(198, 0), attr(187, 0), attr(199, 0)};
    v = health::ata(clean);
    report(v.state == Health::State::Healthy && v.reasons.isEmpty(), QStringLiteral("a clean drive is healthy"), codes(v).join(QLatin1Char(' ')));

    health::AtaInput uncorrectable = clean;
    uncorrectable.attributes[3] = attr(187, 3);
    v = health::ata(uncorrectable);
    report(v.state == Health::State::Warning && codes(v).contains(QLatin1String("uncorrectable?")),
           QStringLiteral("errors it couldn't fix (187) warn"), codes(v).join(QLatin1Char(' ')));

    health::AtaInput cable = clean;
    cable.attributes[4] = attr(199, 12);
    v = health::ata(cable);
    report(v.state == Health::State::Healthy && codes(v) == QStringList{QStringLiteral("crc.")},
           QStringLiteral("old connection errors are only a note"), codes(v).join(QLatin1Char(' ')));
    cable.seen.insert(199, 10);
    v = health::ata(cable);
    report(v.state == Health::State::Warning && codes(v) == QStringList{QStringLiteral("crc?")} && v.reasons[0].text.contains(QLatin1String("cable")),
           QStringLiteral("connection errors that went up say to check the cable"), v.reasons.value(0).text);

    health::AtaInput failing = clean;
    failing.attributes << attr(1, 0, 40, 62);
    failing.failingNow = 1;
    v = health::ata(failing);
    report(v.state == Health::State::Failing && v.summary.contains(QLatin1String("back up")),
           QStringLiteral("a number past its maker's limit is failing"), v.summary);
    health::AtaInput says = clean;
    says.driveSaysFailing = true;
    report(health::ata(says).state == Health::State::Failing, QStringLiteral("the drive saying it's failing is failing"));

    health::AtaInput hot = clean;
    hot.temperatureC = 58;
    report(codes(health::ata(hot)) == QStringList{QStringLiteral("hot?")}, QStringLiteral("a hard drive at 58 °C runs hot"));
    hot.ssd = true;
    report(health::ata(hot).reasons.isEmpty(), QStringLiteral("an SSD at 58 °C is fine"));

    health::AtaInput ssd = clean;
    ssd.ssd = true;
    ssd.attributes << attr(231, 0, 87);
    report(health::ata(ssd).lifeLeft == 87, QStringLiteral("SSD life left is read from its wear attribute"));

    // NVMe
    health::NvmeInput nvme;
    nvme.availableSpare = 100;
    nvme.spareThreshold = 10;
    nvme.percentUsed = 3;
    nvme.temperatureC = 41;
    nvme.warningTempC = 82;
    report(health::nvme(nvme).state == Health::State::Healthy, QStringLiteral("the Samsung 990 PRO is healthy"));
    health::NvmeInput worn = nvme;
    worn.percentUsed = 104;
    report(codes(health::nvme(worn)) == QStringList{QStringLiteral("worn?")}, QStringLiteral("an NVMe past its rated life warns"));
    health::NvmeInput low = nvme;
    low.availableSpare = 15;
    report(codes(health::nvme(low)) == QStringList{QStringLiteral("spare-low?")}, QStringLiteral("spare close to its threshold warns"));
    low.availableSpare = 5;
    report(health::nvme(low).state == Health::State::Failing, QStringLiteral("spare under its threshold is failing"));
    health::NvmeInput hotNvme = nvme;
    hotNvme.temperatureC = 83;
    report(codes(health::nvme(hotNvme)) == QStringList{QStringLiteral("hot?")}, QStringLiteral("an NVMe past its own warning temperature warns"));
    health::NvmeInput critical = nvme;
    critical.criticalWarnings = {QStringLiteral("readonly")};
    report(health::nvme(critical).state == Health::State::Failing, QStringLiteral("an NVMe that went read-only is failing"));
    health::NvmeInput errors = nvme;
    errors.mediaErrors = 2;
    report(codes(health::nvme(errors)) == QStringList{QStringLiteral("media-errors?")}, QStringLiteral("media errors warn"));

    // Remembered counters: first values stick until acknowledged.
    const QString key = QStringLiteral("Test_Drive_123");
    health::remember(key, {attr(199, 3), attr(188, 1)});
    health::remember(key, {attr(199, 9), attr(188, 1)});
    report(health::seen(key).value(199) == 3, QStringLiteral("the first value seen is kept"));
    health::acknowledge(key, {attr(199, 9), attr(188, 1)});
    report(health::seen(key).value(199) == 9, QStringLiteral("dismissing a warning takes the current values"));

    // The banner: dismissed until something new or a bigger number.
    Health shown;
    shown.reasons = {{HealthReason::Level::Warning, QStringLiteral("reallocated"), 16, QString()},
                     {HealthReason::Level::Note, QStringLiteral("timeouts"), 4, QString()}};
    const QStringList dismissed = health::signature(shown);
    report(dismissed == QStringList{QStringLiteral("reallocated=16")} && !health::worseThan(shown, dismissed),
           QStringLiteral("a dismissed warning stays dismissed"), dismissed.join(QLatin1Char(' ')));
    Health more = shown;
    more.reasons[0].number = 24;
    Health other = shown;
    other.reasons << HealthReason{HealthReason::Level::Warning, QStringLiteral("pending"), 2, QString()};
    Health better = shown;
    better.reasons[0].number = 8;
    report(health::worseThan(more, dismissed) && health::worseThan(other, dismissed) && !health::worseThan(better, dismissed)
               && health::worseThan(shown, {}),
           QStringLiteral("it comes back when a number goes up or something new appears, not when it gets better"));

    // fwupd, from what it said on this PC (serials shortened).
    auto device = [](const char *name, const char *version, const char *serial, quint64 flags) {
        return QVariantMap{{QStringLiteral("DeviceId"), QStringLiteral("id-") + QLatin1String(name)},
                           {QStringLiteral("Name"), QLatin1String(name)},
                           {QStringLiteral("Version"), QLatin1String(version)},
                           {QStringLiteral("Serial"), QLatin1String(serial)},
                           {QStringLiteral("Plugin"), QStringLiteral("nvme")},
                           {QStringLiteral("Flags"), flags}};
    };
    const quint64 updatable = 4644337652597003ULL; // internal, updatable, needs AC, needs a reboot
    const QVector<firmware::Device> fw = firmware::devices({device("SSD 990 PRO 1TB", "8B2QJXD7", "S7LAN", updatable),
                                                            device("HTS545050A7E380", "GG2OACA0", "TE851", 4503600164241675ULL),
                                                            device("Cruzer Glide", "1.26", "", 4503599627370496ULL)});
    report(fw.size() == 3 && fw[0].updatable() && fw[1].updatable() && !fw[2].updatable(), QStringLiteral("fwupd's devices are read, with the updatable flag"));
    report(firmware::match(fw, QStringLiteral("TE851"), QString(), QString()) == 1
               && firmware::match(fw, QString(), QStringLiteral("Samsung SSD 990 PRO 1TB"), QStringLiteral("8B2QJXD7")) == 0
               && firmware::match(fw, QStringLiteral("OTHER"), QStringLiteral("Samsung SSD 990 PRO 1TB"), QStringLiteral("8B2QJXD7")) == -1
               && firmware::match(fw, QString(), QStringLiteral("Samsung SSD 990 PRO 1TB"), QStringLiteral("8B2QJXD8")) == -1
               && firmware::match(fw, QString(), QStringLiteral("SanDisk Cruzer Glide"), QStringLiteral("1.26")) == 2,
           QStringLiteral("a drive is matched by serial, or by model and firmware version"));
    const QVector<firmware::Device> twins = firmware::devices({device("Twin", "1.0", "", updatable), device("Twin", "1.0", "", updatable)});
    report(firmware::match(twins, QString(), QStringLiteral("Twin"), QStringLiteral("1.0")) == -1, QStringLiteral("two alike without serials aren't guessed"));

    const QVariantMap never{{QStringLiteral("Enabled"), true}, {QStringLiteral("Type"), 1u}, {QStringLiteral("ModificationTime"), quint64(-1)}};
    const QVariantMap fresh{{QStringLiteral("Enabled"), true}, {QStringLiteral("Type"), 1u}, {QStringLiteral("ModificationTime"), quint64(1790000000)}};
    const QVariantMap folder{{QStringLiteral("Enabled"), true}, {QStringLiteral("Type"), 3u}, {QStringLiteral("ModificationTime"), quint64(1791327398)}};
    using FwState = firmware::Result::State;
    const firmware::Result noList = firmware::outcome(fw[0], {}, QStringLiteral("org.freedesktop.fwupd.NotSupported"),
                                                      QStringLiteral("no components in silo"), {folder, never});
    const firmware::Result upToDate = firmware::outcome(fw[0], {}, QStringLiteral("org.freedesktop.fwupd.NothingToDo"),
                                                        QStringLiteral("No upgrades for SSD 990 PRO 1TB"), {folder, fresh});
    const firmware::Result available = firmware::outcome(fw[1], {{{QStringLiteral("Version"), QStringLiteral("GG2OACA1")}}}, {}, {}, {fresh});
    const firmware::Result stick = firmware::outcome(fw[2], {}, QStringLiteral("org.freedesktop.fwupd.NotSupported"), {}, {fresh});
    const firmware::Result broken = firmware::outcome(fw[0], {}, QStringLiteral("org.freedesktop.fwupd.Internal"), QStringLiteral("oops"), {fresh});
    report(noList.state == FwState::NoList && firmware::describe(noList).contains(QLatin1String("fwupdmgr refresh")),
           QStringLiteral("no list downloaded says how to get one"), firmware::describe(noList));
    report(upToDate.state == FwState::UpToDate && upToDate.listDate.isValid(), QStringLiteral("nothing newer, with a list, is up to date"),
           firmware::describe(upToDate));
    report(available.state == FwState::Available && available.newVersion == QLatin1String("GG2OACA1")
               && firmware::describe(available).contains(QLatin1String("GG2OACA1")),
           QStringLiteral("a newer version is shown, and DiskForge doesn't install it"), firmware::describe(available));
    report(stick.state == FwState::NotUpdatable && broken.state == FwState::Error && broken.error == QLatin1String("oops"),
           QStringLiteral("a drive fwupd can't update, and other errors, say so"));

    // Btrfs error counts, from a made-up /sys/fs/btrfs: one clean file system, and one on two
    // drives with errors, mounted as a clone (temp_fsid).
    QTemporaryDir sys;
    auto put = [&sys](const QString &path, const QByteArray &data) {
        QDir().mkpath(QFileInfo(sys.filePath(path)).path());
        QFile f(sys.filePath(path));
        if (f.open(QIODevice::WriteOnly))
            f.write(data);
    };
    const QByteArray zero = "write_errs 0\nread_errs 0\nflush_errs 0\ncorruption_errs 0\ngeneration_errs 0\n";
    put(QStringLiteral("3abccb79/devices/nvme0n1p2"), {});
    put(QStringLiteral("3abccb79/devinfo/1/error_stats"), zero);
    put(QStringLiteral("3abccb79/temp_fsid"), "0\n");
    put(QStringLiteral("dbd34a09/devices/sda1"), {});
    put(QStringLiteral("dbd34a09/devices/sdb1"), {});
    put(QStringLiteral("dbd34a09/devinfo/1/error_stats"), "write_errs 0\nread_errs 2\nflush_errs 0\ncorruption_errs 1\ngeneration_errs 0\n");
    put(QStringLiteral("dbd34a09/devinfo/2/error_stats"), "write_errs 0\nread_errs 1\nflush_errs 0\ncorruption_errs 0\ngeneration_errs 0\n");
    put(QStringLiteral("dbd34a09/temp_fsid"), "1\n");
    const btrfscheck::Counts cleanFs = btrfscheck::read(QStringLiteral("nvme0n1p2"), sys.path());
    const btrfscheck::Counts twoDrives = btrfscheck::read(QStringLiteral("sdb1"), sys.path());
    report(cleanFs.known && cleanFs.total() == 0 && cleanFs.devices == 1 && btrfscheck::describe(cleanFs) == QLatin1String("No errors recorded."),
           QStringLiteral("a clean Btrfs has no errors recorded"), btrfscheck::describe(cleanFs));
    report(twoDrives.read == 3 && twoDrives.corruption == 1 && twoDrives.devices == 2 && twoDrives.tempFsid
               && btrfscheck::describe(twoDrives).contains(QLatin1String("3 read error")) && btrfscheck::describe(twoDrives).contains(QLatin1String("all 2 drives")),
           QStringLiteral("errors on a Btrfs across two drives are added up, and a clone is noted"), btrfscheck::describe(twoDrives));
    report(!btrfscheck::read(QStringLiteral("sdc1"), sys.path()).known && !btrfscheck::read(QStringLiteral("../dbd34a09/devices/sda1"), sys.path()).known
               && !btrfscheck::read(QString(), sys.path()).known,
           QStringLiteral("an unknown or odd device name finds nothing"));
    const btrfscheck::Counts damaged = btrfscheck::parse("read_errs -5\nwrite_errs 99999999999999999999\ncorruption_errs 7 8\nflush_errs x\n");
    const btrfscheck::Counts huge = btrfscheck::parse("read_errs 9223372036854775807\nwrite_errs 9223372036854775807\ngeneration_errs 9223372036854775807\n");
    report(!damaged.known && damaged.total() == 0 && huge.total() > 0 && huge.total() <= 3000000000000LL,
           QStringLiteral("damaged or huge counts don't count or overflow"));

    // Scrub units: named like systemd-escape --path names them.
    const QStringList paths = {QStringLiteral("/"), QStringLiteral("/home"), QStringLiteral("/run/media/d34droot/SATA500"),
                               QStringLiteral("/mnt/my disk"), QStringLiteral("/mnt/a-b"), QStringLiteral("/.dotdir"),
                               QStringLiteral("/mnt/.hidden"), QStringLiteral("/mnt/\u00fcber"), QStringLiteral("//mnt//x/"),
                               QStringLiteral("/mnt/a\\b"), QStringLiteral("/mnt/a:b_c.d"), QStringLiteral("/mnt/100%")};
    QStringList wrong;
    const bool haveTool = !QStandardPaths::findExecutable(QStringLiteral("systemd-escape")).isEmpty();
    for (const QString &path : haveTool ? paths : QStringList()) {
        QProcess p;
        p.start(QStringLiteral("systemd-escape"), {QStringLiteral("--path"), path});
        p.waitForFinished();
        const QString expected = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
        if (Systemd::escapePath(path) != expected)
            wrong << QStringLiteral("%1 -> %2 (systemd: %3)").arg(path, Systemd::escapePath(path), expected);
    }
    report(wrong.isEmpty(), haveTool ? QStringLiteral("unit names match systemd-escape --path") : QStringLiteral("unit names (skipped: no systemd-escape)"),
           wrong.join(QStringLiteral(" | ")));
    report(btrfscheck::scrubUnit(QStringLiteral("/")) == QLatin1String("btrfs-scrub@-.service")
               && btrfscheck::scrubTimer(QStringLiteral("/run/media/d34droot/SATA500")) == QLatin1String("btrfs-scrub@run-media-d34droot-SATA500.timer")
               && btrfscheck::scrubMountPoint({QStringLiteral("/home"), QStringLiteral("/"), QStringLiteral("/var/log")}) == QLatin1String("/")
               && btrfscheck::scrubMountPoint({QStringLiteral("/run/media/x/Data"), QStringLiteral("/mnt/d")}) == QLatin1String("/mnt/d"),
           QStringLiteral("one scrub per file system, through / when it's there"));
    report(btrfscheck::scrubOutcome(true, 0, QStringLiteral("success"), 0, 0).contains(QLatin1String("checked out"))
               && btrfscheck::scrubOutcome(true, 0, QStringLiteral("success"), 1, 4).contains(QLatin1String("repaired"))
               && btrfscheck::scrubOutcome(true, 3, QStringLiteral("exit-code"), 0, 2).contains(QLatin1String("couldn't fix"))
               && btrfscheck::scrubOutcome(false, 0, QStringLiteral("success"), 0, 0).contains(QLatin1String("nothing was checked"))
               && btrfscheck::scrubOutcome(true, 1, QStringLiteral("exit-code"), 0, 0).contains(QLatin1String("didn't finish")),
           QStringLiteral("a finished scrub says what it found"));

    // Reasons from outside SMART keep the order and the verdict.
    Health withNote;
    withNote.state = Health::State::Healthy;
    withNote.summary = QStringLiteral("Healthy");
    withNote.reasons = {{HealthReason::Level::Note, QStringLiteral("timeouts"), 4, QStringLiteral("4 command timeouts.")}};
    health::add(withNote, {HealthReason::Level::Warning, QStringLiteral("btrfs-sda1"), 3, QStringLiteral("Btrfs on sda1 has seen errors. More.")});
    Health failingAlready;
    failingAlready.state = Health::State::Failing;
    failingAlready.summary = QStringLiteral("Failing, back up now");
    health::add(failingAlready, {HealthReason::Level::Warning, QStringLiteral("btrfs-sda1"), 3, QStringLiteral("x")});
    report(withNote.state == Health::State::Warning && withNote.reasons[0].code == QLatin1String("btrfs-sda1")
               && withNote.summary == QLatin1String("Btrfs on sda1 has seen errors") && failingAlready.state == Health::State::Failing
               && failingAlready.summary.startsWith(QLatin1String("Failing")),
           QStringLiteral("Btrfs errors raise a healthy drive to a warning, and never lower a failing one"), withNote.summary);

    // Hard drive power settings.
    report(drivepower::standbyText(0) == QLatin1String("never") && drivepower::standbyText(60).contains(QLatin1String("5 minute"))
               && drivepower::standbyText(241).contains(QLatin1String("30 minute")) && drivepower::standbyText(242).contains(QLatin1String("1 hour"))
               && drivepower::standbyText(244).contains(QLatin1String("2 hour")) && drivepower::standbyText(12).contains(QLatin1String("1 minute")),
           QStringLiteral("spin-down times read hdparm's way (5-second steps, then half hours)"));
    bool choicesFit = true;
    for (const drivepower::Choice &c : drivepower::standbyChoices())
        choicesFit = choicesFit && c.value >= 0 && c.value <= 251 && !c.label.isEmpty();
    for (const drivepower::Choice &c : drivepower::apmChoices())
        choicesFit = choicesFit && c.value >= 1 && c.value <= 254 && !c.label.isEmpty();
    report(choicesFit, QStringLiteral("every offered spin-down time and power-saving level is a valid value"));
    using drivepower::kDriveDefault;
    using drivepower::kKeep;
    const QVariantMap first = drivepower::configuration({}, 120, kKeep, kKeep);
    const QVariantMap kept{{QStringLiteral("ata-pm-standby"), 120}, {QStringLiteral("ata-apm-level"), 128}, {QStringLiteral("other-key"), true}};
    const QVariantMap changed = drivepower::configuration(kept, kDriveDefault, 254, 0);
    const QVariantMap odd = drivepower::configuration(kept, 300, 0, 7);
    report(first == QVariantMap{{QStringLiteral("ata-pm-standby"), 120}}
               && changed == QVariantMap{{QStringLiteral("ata-apm-level"), 254}, {QStringLiteral("ata-write-cache-enabled"), false}, {QStringLiteral("other-key"), true}}
               && odd == kept,
           QStringLiteral("power settings change only what was picked, and keep keys DiskForge doesn't know"));
    report(drivepower::stateText(0x00).startsWith(QLatin1String("Asleep")) && drivepower::stateText(0xff) == QLatin1String("Spinning")
               && drivepower::stateText(0x80) == QLatin1String("Idle") && drivepower::stateText(7) == QLatin1String("Unknown"),
           QStringLiteral("power states in plain words"));
    Disk hdd;
    hdd.rotationRate = 5400;
    hdd.ataPm = true;
    Disk solid = hdd;
    solid.rotationRate = 0;
    Disk image = hdd;
    image.isLoop = true;
    Disk flash = hdd;
    flash.nvmeNamespace = true;
    Disk bare = hdd;
    bare.ataPm = false;
    Disk solidCache = solid;
    solidCache.ataWriteCache = true;
    report(PowerBox::applies(hdd) && !PowerBox::applies(solid) && PowerBox::applies(solidCache) && !PowerBox::applies(image) && !PowerBox::applies(flash)
               && !PowerBox::applies(bare),
           QStringLiteral("drive settings show for drives that have them (an SSD's write cache too), not for an SSD's spin-down, images or NVMe"));

    int described = 0;
    for (const int id : {1, 3, 4, 5, 7, 9, 10, 12, 177, 184, 187, 188, 190, 194, 196, 197, 198, 199, 231, 241})
        described += health::describe(id).name.isEmpty() ? 0 : 1;
    report(described == 20 && health::describe(5).counts && health::describe(199).counts && !health::describe(9).counts,
           QStringLiteral("the common attributes have plain names, and the ones that count are marked"));
}
