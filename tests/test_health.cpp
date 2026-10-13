// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// The health verdict, from recorded SMART values: what counts, what's only a note, and
// counters that never go down only warning when they grow.

#include "testkit.h"

#include "../src/health/burnin.h"
#include "../src/health/writecliff.h"
#include "../src/health/hardware.h"

#include <unistd.h>

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
#include "../src/viruses/productkey.h"
#include "../src/viruses/peinfo.h"
#include "../src/viruses/efiboot.h"
#include "../src/tools/linuxboot.h"
#include "../src/tools/winre.h"
#include "../src/tools/testboot.h"
#include "../src/drives/usbinfo.h"
#include "../src/drives/blockio.h"
#include "../src/health/surfacescan.h"
#include "../src/tools/systemd.h"

#include <QDir>
#include <QFile>
#include <QThread>
#include <QRandomGenerator>
#include <QElapsedTimer>
#include <QCryptographicHash>
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

// The raw SMART and NVMe pages DiskAnvil reads itself on Windows: decoded the way UDisks decodes
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
    report(triage::advise(Symptom::Deleted, system).odds_why.contains(QLatin1String("DiskAnvil Live")),
           QStringLiteral("what happened: the system's own drive is best looked at from DiskAnvil Live"));
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
    Disk a, b, c, e, f, g;
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
    // An MBR dynamic disk as Linux shows it: the kernel's own volume, which the table doesn't have.
    f.device = QStringLiteral("/dev/sdf");
    f.tableType = QStringLiteral("dos");
    f.volumes = {volume(QStringLiteral("/dev/sdf1"), QStringLiteral("ntfs"), QStringLiteral("AEEA5DF0EA5DB575"), QStringLiteral("DYNVOL"), QString())};
    f.volumes[0].number = 1;
    // An ordinary MBR disk.
    g.device = QStringLiteral("/dev/sdg");
    g.tableType = QStringLiteral("dos");
    g.volumes = {volume(QStringLiteral("/dev/sdg1"), QStringLiteral("ntfs"), QStringLiteral("1234"), QStringLiteral("DATA"), QString(), QStringLiteral("0x07"))};
    g.volumes[0].number = 1;
    const QVector<closedsets::Set> sets = closedsets::find({a, b, c, e, f, g}, false);
    QStringList said;
    for (const auto &s : sets)
        said << closedsets::describe(s);
    report(sets.size() == 3 && sets[0].name == QLatin1String("DiskStation:0") && sets[1].members == QStringList{QStringLiteral("/dev/sdb5"), QStringLiteral("/dev/sdc5")}
               && sets[2].kind == closedsets::Set::Kind::Dynamic && sets[2].members == QStringList{QStringLiteral("/dev/sdd"), QStringLiteral("/dev/sde"), QStringLiteral("/dev/sdf")},
           QStringLiteral("closed sets: a NAS's arrays by their members (a running one left out), and Windows dynamic disks"), said.join(QStringLiteral(" | ")));
    report(closedsets::find({c}, true).isEmpty(), QStringLiteral("closed sets: dynamic disks whose volumes are open already aren't offered again"));
    const QString script = closedsets::openScript(sets);
    report(script.contains(QLatin1String("mdadm --assemble --scan --readonly --run")) && script.contains(QLatin1String("vgchange -ay"))
               && script.contains(QLatin1String("ldmtool create all")) && !closedsets::openScript({sets[2]}).contains(QLatin1String("mdadm")),
           QStringLiteral("closed sets: opened read-only, each program only for what needs it"), script);
    report(closedsets::missing(sets, [](const QString &p) { return p == QLatin1String("mdadm"); }) == QStringList{QStringLiteral("ldmtool")},
           QStringLiteral("closed sets: a program that isn't here is named"));
}

// Windows product keys: the firmware's MSDM table, and the registry's encoded one.
void productKeys()
{
    QByteArray msdm(56, '\0');
    msdm.replace(0, 4, "MSDM");
    msdm += "ABCDE-FGH2J-KLMN3-PQRS4-TUVW5";
    report(productkey::fromMsdm(msdm) == QLatin1String("ABCDE-FGH2J-KLMN3-PQRS4-TUVW5") && productkey::fromMsdm(QByteArray("MSDM")).isEmpty()
               && productkey::fromMsdm(QByteArray(90, 'x')).isEmpty(),
           QStringLiteral("product key: the firmware's (MSDM) is read where the table keeps it"));
    // Encoded the way Windows 8 and later keep it: base 24, the "N" taken out and its place in
    // the first character, bit 3 of the last byte set.
    const auto encode = [](const QString &key) {
        static const QString chars = QStringLiteral("BCDFGHJKMPQRTVWXY2346789");
        QString plain = key;
        plain.remove(QLatin1Char('-'));
        const int n = int(plain.indexOf(QLatin1Char('N')));
        plain.remove(n, 1);
        plain.prepend(chars.at(n));
        QByteArray bytes(15, '\0');
        for (const QChar c : plain) { // the number times 24, plus the digit
            int carry = int(chars.indexOf(c));
            for (int i = 0; i < 15; ++i) {
                const int v = quint8(bytes[i]) * 24 + carry;
                bytes[i] = char(v & 0xFF);
                carry = v >> 8;
            }
        }
        bytes[14] = char(quint8(bytes[14]) | 0x08);
        QByteArray value(164, '\0');
        value.replace(52, 15, bytes);
        return value;
    };
    bool all = true;
    QString wrong;
    for (const QString &key : {QStringLiteral("VK7JG-NPHTM-C97JM-9MPGT-3V66T"), QStringLiteral("TX9XD-98N7V-6WMQ6-BX7FG-H8Q99"),
                               QStringLiteral("NBCDF-GHJKM-PQRTV-WXY23-46789"), QStringLiteral("99999-99999-99999-9999N-99999")}) { // the N can be 0 to 23 in
        const QString back = productkey::decode(encode(key));
        if (back != key) {
            all = false;
            wrong += key + QStringLiteral(" -> ") + back + QLatin1Char(' ');
        }
    }
    report(all && productkey::isGeneric(QStringLiteral("vk7jg-nphtm-c97jm-9mpgt-3v66t")) && !productkey::isGeneric(QStringLiteral("ABCDE-FGH2J-KLMN3-PQRS4-TUVW5"))
               && productkey::decode(QByteArray(20, '\0')).isEmpty(),
           QStringLiteral("product key: the registry's key is decoded, the N wherever it is, and generic ones are told apart"), wrong);
    // The test SOFTWARE hive (made in Windows with our own keys) has no key: it says so.
    const productkey::Installed none = productkey::fromHive(QStringLiteral(SOURCE_DIR "/tests/data/registry/software.hiv"));
    report(none.key.isEmpty() && none.problem.contains(QLatin1String("no key")), QStringLiteral("product key: a registry without one says so"),
           none.problem);
}

// A Windows program's version strings, from our own tiny EXE (tests/data/pe/make.sh).
void peVersions()
{
    QFile exe(QStringLiteral(SOURCE_DIR "/tests/data/pe/versioned.exe"));
    const QByteArray data = exe.open(QIODevice::ReadOnly) ? exe.readAll() : QByteArray();
    const QHash<QString, QString> v = peinfo::versionStrings(data);
    report(v.value(QStringLiteral("CompanyName")) == QLatin1String("DiskAnvil Test Company")
               && v.value(QStringLiteral("FileDescription")) == QStringLiteral("Grüße driver for tests") && v.value(QStringLiteral("FileVersion")) == QLatin1String("3.1.4.1"),
           QStringLiteral("PE: the company and description in a program's version resource"),
           QStringList(v.keys()).join(QLatin1Char(',')) + QStringLiteral(" ") + v.value(QStringLiteral("FileDescription")));
    // Cut short or scribbled on, it's read without going out of bounds (ASan watches this);
    // something that isn't a program gives nothing.
    peinfo::versionStrings(data.left(data.size() / 2));
    QByteArray broken = data;
    for (qsizetype i = 0x200; i < broken.size(); i += 37)
        broken[i] = char(0xFF);
    peinfo::versionStrings(broken);
    report(peinfo::versionStrings(QByteArray("MZ")).isEmpty() && peinfo::versionStrings(QByteArray(4096, '\0')).isEmpty(),
           QStringLiteral("PE: damaged files are read within bounds, and what isn't a program gives nothing"));
}

// The boot menu: entries built the firmware's way, read back the same; and this PC's own
// entries, rebuilt from what was read of them, byte for byte up to the end of their device path.
void bootMenu()
{
    const QByteArray made = efiboot::loadOption(QStringLiteral("Debian"), 1, 2048, 1048576, QStringLiteral("0f5c3a1e-8b2d-4c6e-9a7f-1234567890ab"),
                                                QStringLiteral("/EFI/debian/shimx64.efi"));
    const std::optional<efi::BootEntry> back = efi::parseLoadOption(7, made);
    report(back && back->description == QLatin1String("Debian") && back->active && back->partition == QLatin1String("0f5c3a1e-8b2d-4c6e-9a7f-1234567890ab")
               && back->path == QLatin1String("\\EFI\\debian\\shimx64.efi") && back->partitionNumber == 1 && back->partitionStart == 2048
               && back->partitionSize == 1048576,
           QStringLiteral("boot menu: an entry made the firmware's way reads back the same"), back ? back->partition + QLatin1Char(' ') + back->path : QString());
    report(efiboot::orderValue({3, 1, 0x1A}) == QByteArray("\x03\x00\x01\x00\x1a\x00", 6)
               && efiboot::orderCommand({3, 1, 0x1A}).join(QLatin1Char(' ')) == QLatin1String("efibootmgr -o 0003,0001,001A")
               && efiboot::createCommand(QStringLiteral("/dev/nvme0n1"), 1, QStringLiteral("/EFI/debian/shimx64.efi"), QStringLiteral("Debian")).join(QLatin1Char(' '))
                      == QLatin1String("efibootmgr -c -d /dev/nvme0n1 -p 1 -l \\EFI\\debian\\shimx64.efi -L Debian")
               && efiboot::removeCommand(2).join(QLatin1Char(' ')) == QLatin1String("efibootmgr -b 0002 -B"),
           QStringLiteral("boot menu: the order as the firmware keeps it, and efibootmgr's commands"));
    // This PC's own, when it started with UEFI: rebuilt from what was read, they're the same bytes.
    int checked = 0, same = 0;
    QStringList differ;
    for (const efi::BootEntry &e : efi::bootEntries()) {
        const std::optional<QByteArray> raw = efi::variable(QStringLiteral("Boot") + efiboot::hex4(e.number));
        if (!raw || e.partition.isEmpty() || e.path.isEmpty())
            continue;
        ++checked;
        const QByteArray rebuilt = efiboot::loadOption(e.description, e.partitionNumber, e.partitionStart, e.partitionSize, e.partition, e.path, e.active);
        // Their attributes can have more bits (hidden, a category): only the active one is ours.
        if (raw->mid(4, rebuilt.size() - 4) == rebuilt.mid(4))
            ++same;
        else
            differ << e.description;
    }
    report(same == checked, QStringLiteral("boot menu: this PC's %1 disk entries, rebuilt, are the firmware's own bytes").arg(checked), differ.join(QStringLiteral(", ")));
}

// Repair Linux Boot: its fstab read, each kind of install recognized from its files, and the
// script for each (checked by bash -n, never run here: grub-install would change this PC's own
// boot menu; the VM tests run it).
void linuxBootRepair()
{
    const QVector<linuxboot::FstabEntry> fstab = linuxboot::parseFstab(
        "# /etc/fstab\nUUID=aaaa-1111 / ext4 errors=remount-ro 0 1\n\nUUID=7B2C-1D3E  /boot/efi  vfat  umask=0077  0 1\n"
        "PARTUUID=0f5c3a1e-8b2d-4c6e-9a7f-1234567890ab /boot ext4 defaults 0 2\nLABEL=My\\040Data /data ext4 defaults 0 2\n");
    report(fstab.size() == 4 && fstab[1].where == QLatin1String("/boot/efi") && fstab[1].type == QLatin1String("vfat") && fstab[3].what == QLatin1String("LABEL=My Data"),
           QStringLiteral("linux boot: its fstab is read, comments and spaces in names too"));
    Disk disk;
    disk.device = QStringLiteral("/dev/nvme0n1");
    Volume esp, boot, root, data;
    esp.device = QStringLiteral("/dev/nvme0n1p1");
    esp.uuid = QStringLiteral("7B2C-1D3E");
    esp.number = 1;
    boot.device = QStringLiteral("/dev/nvme0n1p2");
    boot.partUuid = QStringLiteral("0F5C3A1E-8B2D-4C6E-9A7F-1234567890AB");
    root.device = QStringLiteral("/dev/nvme0n1p3");
    root.uuid = QStringLiteral("aaaa-1111");
    data.device = QStringLiteral("/dev/nvme0n1p4");
    data.label = QStringLiteral("My Data");
    disk.volumes = {esp, boot, root, data};
    const QVector<Disk> disks{disk};
    report(linuxboot::deviceFor(QStringLiteral("UUID=7B2C-1D3E"), disks) == esp.device
               && linuxboot::deviceFor(QStringLiteral("PARTUUID=0f5c3a1e-8b2d-4c6e-9a7f-1234567890ab"), disks) == boot.device
               && linuxboot::deviceFor(QStringLiteral("LABEL=My Data"), disks) == data.device && linuxboot::deviceFor(QStringLiteral("UUID=nope"), disks).isEmpty(),
           QStringLiteral("linux boot: UUID, PARTUUID and LABEL find their partitions"));

    QTemporaryDir dir;
    const auto put = [&dir](const QString &path, const QByteArray &data) {
        QDir().mkpath(QFileInfo(dir.filePath(path)).absolutePath());
        QFile f(dir.filePath(path));
        return f.open(QIODevice::WriteOnly) && f.write(data) >= 0;
    };
    const QByteArray etc = "UUID=aaaa-1111 / ext4 defaults 0 1\nUUID=7B2C-1D3E /boot/efi vfat umask=0077 0 1\n";
    put(QStringLiteral("debian/etc/os-release"), "PRETTY_NAME=\"Debian GNU/Linux 13 (trixie)\"\nID=debian\n");
    put(QStringLiteral("debian/etc/fstab"), etc);
    put(QStringLiteral("debian/usr/sbin/grub-install"), "");
    put(QStringLiteral("fedora/etc/os-release"), "PRETTY_NAME=\"Fedora Linux 43 (Workstation Edition)\"\nID=fedora\n");
    put(QStringLiteral("fedora/etc/fstab"), etc);
    put(QStringLiteral("fedora/usr/sbin/grub2-install"), "");
    put(QStringLiteral("fedora/usr/bin/bootctl"), "");
    put(QStringLiteral("arch/etc/os-release"), "PRETTY_NAME=\"Arch Linux\"\nID=arch\n");
    put(QStringLiteral("arch/etc/fstab"), "UUID=aaaa-1111 / ext4 defaults 0 1\nUUID=7B2C-1D3E /efi vfat umask=0077 0 1\n");
    put(QStringLiteral("arch/usr/bin/bootctl"), "");
    put(QStringLiteral("top/@/etc/fstab"), etc);
    const linuxboot::Install debian = linuxboot::inspect(dir.filePath(QStringLiteral("debian")), root.device, QString(), disks);
    const linuxboot::Install fedora = linuxboot::inspect(dir.filePath(QStringLiteral("fedora")), root.device, QString(), disks);
    const linuxboot::Install arch = linuxboot::inspect(dir.filePath(QStringLiteral("arch")), root.device, QStringLiteral("@"), disks);
    report(debian.loader == linuxboot::Install::Loader::Grub && debian.id == QLatin1String("debian") && debian.espDevice == esp.device
               && debian.espMount == QLatin1String("/boot/efi") && debian.disk == disk.device && debian.problems.isEmpty()
               && fedora.loader == linuxboot::Install::Loader::Grub2 && arch.loader == linuxboot::Install::Loader::SystemdBoot
               && arch.espMount == QLatin1String("/efi") && linuxboot::rootSubvolumes(dir.filePath(QStringLiteral("top"))) == QStringList{QStringLiteral("@")},
           QStringLiteral("linux boot: Debian's GRUB, Fedora's GRUB 2 and Arch's systemd-boot are told apart, with their EFI partitions"),
           QStringLiteral("%1 %2 %3 %4").arg(int(debian.loader)).arg(int(fedora.loader)).arg(int(arch.loader)).arg(debian.problems.join(QLatin1Char(';'))));

    const QString grub = linuxboot::repairScript(debian, true), grubBios = linuxboot::repairScript(debian, false);
    const QString grub2 = linuxboot::repairScript(fedora, true), systemdBoot = linuxboot::repairScript(arch, true);
    bool syntax = true;
    for (const QString &script : {grub, grubBios, grub2, systemdBoot}) {
        QProcess bash;
        bash.start(QStringLiteral("bash"), {QStringLiteral("-n"), QStringLiteral("-c"), script});
        syntax = syntax && bash.waitForFinished(10000) && bash.exitCode() == 0;
    }
    report(syntax && grub.contains(QLatin1String("grub-install --target=x86_64-efi --efi-directory='\\''/boot/efi'\\'' --bootloader-id='\\''debian'\\''"))
               && grubBios.contains(QLatin1String("grub-install --target=i386-pc '\\''/dev/nvme0n1'\\''")) && grub.contains(QLatin1String("update-grub"))
               && grub2.contains(QLatin1String("shimx64.efi ]; then efibootmgr -c -d '\\''/dev/nvme0n1'\\'' -p 1"))
               && grub2.contains(QLatin1String("grub2-mkconfig -o /boot/grub2/grub.cfg"))
               && systemdBoot.contains(QLatin1String("mount -o subvol='@' '/dev/nvme0n1p3'")) && systemdBoot.contains(QLatin1String("bootctl install --esp-path="))
               && grub.contains(QLatin1String("trap cleanup EXIT")),
           QStringLiteral("linux boot: each install's own repair (quoted twice inside the chroot), in scripts bash takes"),
           QStringLiteral("syntax %1 | ").arg(syntax) + grub.right(300));
}

// Make Room for Windows Recovery: reagentc's answer in any language, Microsoft's conditions for
// its fix, and its diskpart steps.
void windowsRecovery()
{
    const winre::Location english = winre::parse(QStringLiteral("Windows Recovery Environment (Windows RE) and system reset configuration\r\n"
                                                                "Information:\r\n\r\n    Windows RE status:         Enabled\r\n"
                                                                "    Windows RE location:       \\\\?\\GLOBALROOT\\device\\harddisk0\\partition4\\Recovery\\WindowsRE\r\n"));
    const winre::Location german = winre::parse(QStringLiteral("    Windows RE-Status:         Enabled\r\n"
                                                               "    Windows RE-Speicherort:    \\\\?\\GLOBALROOT\\device\\harddisk1\\partition5\\Recovery\\WindowsRE\r\n"));
    const winre::Location off = winre::parse(QStringLiteral("    Windows RE status:         Disabled\r\n    Windows RE location:\r\n"));
    report(english.disk == 0 && english.partition == 4 && german.disk == 1 && german.partition == 5 && off.disk == -1,
           QStringLiteral("windows recovery: where reagentc says it is, in any language, and when it's off"));
    constexpr quint64 MiB = 1024 * 1024;
    Disk disk;
    // As the Windows build lists it: the path in blockPath, "Disk 0" for people.
    disk.blockPath = QStringLiteral("\\\\.\\PhysicalDrive0");
    disk.device = QStringLiteral("Disk 0");
    disk.tableType = QStringLiteral("gpt");
    Volume efi, msr, os, re;
    efi.number = 1;
    efi.offset = 1 * MiB;
    efi.size = 100 * MiB;
    msr.number = 2;
    msr.offset = 101 * MiB;
    msr.size = 16 * MiB;
    os.number = 3;
    os.offset = 117 * MiB;
    os.size = 60000 * MiB;
    os.mountPoints = {QStringLiteral("C:/")};
    os.fsTotal = os.size;
    os.fsFree = 20000 * MiB;
    re.number = 4;
    re.offset = os.offset + os.size;
    re.size = 600 * MiB;
    re.fsTotal = 590 * MiB;
    re.fsFree = 80 * MiB;
    disk.volumes = {efi, msr, os, re};
    const winre::Plan good = winre::plan(english, {disk}, QStringLiteral("C:/"));
    report(good.needed && good.problem.isEmpty() && good.osPartition == 3 && good.rePartition == 4 && good.gpt,
           QStringLiteral("windows recovery: 80 MB free right after Windows' partition: it needs room, and can have it"), good.problem);
    Disk before = disk;
    before.volumes[3].offset = 1 * MiB; // a recovery partition before Windows (older installs)
    Disk roomy = disk;
    roomy.volumes[3].fsFree = 300 * MiB;
    report(!winre::plan(english, {before}, QStringLiteral("C:/")).problem.isEmpty() && !winre::plan(english, {roomy}, QStringLiteral("C:/")).needed
               && !winre::plan(off, {disk}, QStringLiteral("C:/")).problem.isEmpty(),
           QStringLiteral("windows recovery: not before Windows' partition, not when it has room, not when it's off"));
    const QString gpt = winre::diskpartScript(good);
    winre::Plan mbr = good;
    mbr.gpt = false;
    report(gpt.startsWith(QLatin1String("select disk 0\r\nselect partition 3\r\nshrink desired=250 minimum=250\r\nselect partition 4\r\ndelete partition override\r\n"))
               && gpt.contains(QLatin1String("create partition primary id=de94bba4-06d1-4d40-a16a-bfd50179d6ac\r\ngpt attributes=0x8000000000000001"))
               && gpt.contains(QLatin1String("format quick fs=ntfs label=\"Windows RE tools\"")) && winre::diskpartScript(mbr).contains(QLatin1String("set id=27")),
           QStringLiteral("windows recovery: Microsoft's diskpart steps, for GPT and MBR"));
}

// Try Starting It: a boot sector of our own (it writes DFBOOT to the serial port and halts) is
// started in QEMU from a file and through a drive's handle, the BIOS way; and UEFI's firmware
// starts with the arguments given. Writes never reach the image.
void testBoot()
{
    if (testboot::qemu().isEmpty()) {
        out << "SKIP  test boot: needs qemu-system-x86_64" << Qt::endl;
        return;
    }
    QTemporaryDir dir;
    // cli; xor ax,ax; mov ds,ax; mov si,msg; mov dx,0x3f8; next: lodsb; test al,al; jz done;
    // out dx,al; jmp next; done: hlt; jmp done; msg: "DFBOOT\r\n"
    QByteArray sector = QByteArray::fromHex("fa31c08ed8be167cbaf803ac84c07403eeebf8f4ebfd") + QByteArray("DFBOOT\r\n", 9);
    sector = sector.leftJustified(510, '\0') + QByteArray("\x55\xaa", 2);
    const QString image = dir.filePath(QStringLiteral("boot.img"));
    {
        QFile f(image);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(sector);
            f.resize(4 * 1024 * 1024);
        }
    }
    const QByteArray before = QCryptographicHash::hash([&] { QFile f(image); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }(),
                                                       QCryptographicHash::Sha256);
    const auto boot = [&](testboot::Options o) {
        o.window = false;
        o.serialLog = dir.filePath(QStringLiteral("serial-%1.txt").arg(QRandomGenerator::global()->generate()));
        QString error;
        QObject owner;
        QProcess *qemu = testboot::start(o, &owner, &error);
        QByteArray said;
        QElapsedTimer clock;
        clock.start();
        while (qemu && clock.elapsed() < 60000) {
            QFile log(o.serialLog);
            if (log.open(QIODevice::ReadOnly))
                said = log.readAll();
            if (said.contains("DFBOOT") || said.contains("BdsDxe"))
                break;
            QThread::msleep(200);
        }
        if (qemu) {
            qemu->kill();
            qemu->waitForFinished(5000);
        }
        return qemu ? QString::fromLatin1(said) : error;
    };
    testboot::Options file;
    file.file = image;
    file.format = testboot::formatOf(image);
    file.uefi = false;
    const QString fromFile = boot(file);
    report(file.format == QLatin1String("raw") && fromFile.contains(QLatin1String("DFBOOT")), QStringLiteral("test boot: an image starts in QEMU (BIOS)"), fromFile.left(200));
    // Like a drive: open only through its handle (its file can't be opened by name any more).
    const int fd = blockio::openFile(image, blockio::FileMode::Read);
    QFile::setPermissions(image, QFileDevice::Permissions());
    testboot::Options drive;
    drive.fd = fd;
    drive.format = QStringLiteral("raw");
    drive.uefi = false;
    const QString fromFd = boot(drive);
    ::close(fd);
    QFile::setPermissions(image, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    report(fromFd.contains(QLatin1String("DFBOOT")), QStringLiteral("test boot: a drive's read-only handle starts the same way"), fromFd.left(200));
    QString code, vars;
    if (testboot::ovmf(&code, &vars)) {
        testboot::Options uefi = file;
        uefi.uefi = true;
        // Once more when it says nothing: on a busy PC the firmware now and then stays silent the first time.
        QElapsedTimer took;
        took.start();
        QString firmware = boot(uefi);
        int tries = 1;
        if (!firmware.contains(QLatin1String("BdsDxe"))) {
            firmware = boot(uefi);
            ++tries;
        }
        report(firmware.contains(QLatin1String("BdsDxe")), QStringLiteral("test boot: UEFI's firmware starts with the arguments given"),
               QStringLiteral("%1 tries, %2 s: %3").arg(tries).arg(took.elapsed() / 1000).arg(firmware.right(160)));
    }
    const QByteArray after = QCryptographicHash::hash([&] { QFile f(image); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }(),
                                                      QCryptographicHash::Sha256);
    report(before == after, QStringLiteral("test boot: the image is just as it was"));
}

// How a USB drive is connected, from a pretend sysfs: a USB 3 stick in a USB 2 port says so, with
// its controller's name from pci.ids; and this PC's own USB drives, when there are any.
void usbConnection()
{
    QTemporaryDir dir;
    const QString sys = dir.filePath(QStringLiteral("sys"));
    const QString pci = sys + QStringLiteral("/devices/pci0000:00/0000:00:14.0");
    const QString hub = pci + QStringLiteral("/usb1"), stick = hub + QStringLiteral("/1-2");
    const QString block = stick + QStringLiteral("/1-2:1.0/host6/target6:0:0/6:0:0:0/block/sdz");
    QDir().mkpath(block);
    QDir().mkpath(sys + QStringLiteral("/block"));
    const auto put = [](const QString &path, const QByteArray &text) {
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(text) > 0;
    };
    put(pci + QStringLiteral("/class"), "0x0c0330\n");
    put(pci + QStringLiteral("/vendor"), "0x8086\n");
    put(pci + QStringLiteral("/device"), "0xa36d\n");
    put(hub + QStringLiteral("/speed"), "5000\n");
    put(hub + QStringLiteral("/idVendor"), "1d6b\n");
    put(stick + QStringLiteral("/speed"), "480\n");
    put(stick + QStringLiteral("/version"), " 3.20\n");
    put(stick + QStringLiteral("/idVendor"), "154b\n");
    put(stick + QStringLiteral("/idProduct"), "1006\n");
    put(stick + QStringLiteral("/manufacturer"), "PNY\n");
    put(stick + QStringLiteral("/product"), "USB 3.2.1 FD\n");
    QFile::link(block, sys + QStringLiteral("/block/sdz"));
    const QString ids = dir.filePath(QStringLiteral("pci.ids"));
    put(ids, "# test\n1022  Advanced Micro Devices, Inc. [AMD]\n\t43ee  500 Series Chipset USB 3.1 XHCI Controller\n"
             "8086  Intel Corporation\n\ta36c  Something else\n\ta36d  Cannon Lake PCH USB 3.1 xHCI Host Controller\n");
    const usbinfo::Info info = usbinfo::of(QStringLiteral("/dev/sdz"), sys, ids);
    const QString text = usbinfo::describe(info);
    report(info.known && info.speedMbps == 480 && info.portMbps == 5000 && info.port == QLatin1String("1-2") && info.maker == QLatin1String("PNY")
               && info.controller == QLatin1String("Intel Corporation Cannon Lake PCH USB 3.1 xHCI Host Controller") && text.contains(QLatin1String("ten times")),
           QStringLiteral("USB: a USB 3 stick at USB 2 speed is said, with its port and controller"), text);
    report(!usbinfo::of(QStringLiteral("/dev/sdq"), sys, ids).known, QStringLiteral("USB: a drive that isn't on USB has nothing to say"));
    // This PC's own.
    for (const QString &name : QDir(QStringLiteral("/sys/block")).entryList({QStringLiteral("sd*")}, QDir::AllEntries | QDir::System)) {
        const usbinfo::Info real = usbinfo::of(QStringLiteral("/dev/") + name);
        if (real.known)
            report(real.speedMbps > 0 && !real.port.isEmpty(), QStringLiteral("USB: this PC's %1").arg(name), usbinfo::describe(real));
    }
}

// Burn In a New Drive's verdict: counters compared from before to after (ATA by number, NVMe by
// UDisks' key), what's bad and what only points at the cable.
void burnInVerdict()
{
    auto ata = [](int id, qint64 raw) {
        SmartAttribute a;
        a.id = id;
        a.rawValue = raw;
        return a;
    };
    auto nvme = [](const char *key, qint64 raw) {
        SmartAttribute a;
        a.key = QString::fromLatin1(key);
        a.rawValue = raw;
        return a;
    };
    const QVector<SmartAttribute> before = {ata(5, 0), ata(197, 0), ata(199, 3), ata(9, 10), ata(194, 30)};
    QVector<SmartAttribute> after = {ata(5, 0), ata(197, 0), ata(199, 3), ata(9, 40), ata(194, 45)};
    burnin::Report r;
    r.completed = true;
    for (int i = 0; i < burnin::kSteps; ++i)
        r.steps << burnin::StepResult{burnin::Step(i), burnin::StepResult::Outcome::Passed, {}, {}, {}};
    r.changes = burnin::compare(before, after);
    report(r.changes.isEmpty() && burnin::verdict(r) == burnin::Verdict::Passed,
           QStringLiteral("burn-in: hours and temperature going up aren't counted, and it passes"));
    after[2] = ata(199, 9);
    r.changes = burnin::compare(before, after);
    report(r.changes.size() == 1 && burnin::verdict(r) == burnin::Verdict::Warning, QStringLiteral("burn-in: new cable errors are a warning, not a fail"));
    after[0] = ata(5, 8);
    r.changes = burnin::compare(before, after);
    report(r.changes.size() == 2 && burnin::verdict(r) == burnin::Verdict::Failed && r.changes[0].before == 0 && r.changes[0].after == 8,
           QStringLiteral("burn-in: a new drive gaining reallocated sectors fails"));
    r.changes = burnin::compare({nvme("media_errors", 0), nvme("avail_spare", 100), nvme("num_err_log_entries", 2)},
                                {nvme("media_errors", 0), nvme("avail_spare", 97), nvme("num_err_log_entries", 5)});
    report(r.changes.size() == 2 && burnin::verdict(r) == burnin::Verdict::Failed, QStringLiteral("burn-in: an NVMe drive's spare dropping fails; error log entries are only noted"));
    r.changes.clear();
    r.wrongData = 1;
    report(burnin::verdict(r) == burnin::Verdict::Failed, QStringLiteral("burn-in: data that came back wrong fails"));
    r.wrongData = 0;
    r.completed = false;
    report(burnin::verdict(r) == burnin::Verdict::NotFinished && burnin::html(r).contains(QLatin1String("Not finished")),
           QStringLiteral("burn-in: stopped part way is said to be unfinished"));
}

// Write Speed Over Time: where the speed fell (a cache filling), and not for one slow slice or a
// steady drive; then a real run into a file, straight to the drive and, where the file system
// can't do that, flushed instead, with the file gone afterwards.
void writeCliff()
{
    using writecliff::Point;
    auto run = [](const QVector<double> &speeds) {
        QVector<Point> points;
        quint64 written = 0;
        for (double s : speeds) {
            written += 1000000000;
            points << Point{written, s};
        }
        return writecliff::analyse(points);
    };
    QVector<double> cached;
    for (int i = 0; i < 100; ++i)
        cached << (i < 24 ? 3200.0 + (i % 3) * 40 : 450.0 + (i % 5) * 20);
    const writecliff::Finding f = run(cached);
    report(f.cliff && f.cacheBytes == 24000000000ull && qAbs(f.fastMbps - 3240) < 50 && qAbs(f.slowMbps - 490) < 30,
           QStringLiteral("write speed: a cache that fills after 24 GB is found, with the speed before and after"),
           QStringLiteral("%1 %2 %3 %4").arg(f.cliff).arg(f.cacheBytes).arg(f.fastMbps).arg(f.slowMbps));
    QVector<double> steady(80, 520.0);
    steady[30] = 90;
    steady[31] = 120;
    report(!run(steady).cliff, QStringLiteral("write speed: a steady drive with a couple of slow moments has no cliff"));
    QVector<double> jumpy;
    for (int i = 0; i < 60; ++i)
        jumpy << (i % 2 ? 900.0 : 300.0);
    report(!run(jumpy).cliff, QStringLiteral("write speed: one that's up and down all the way has no cliff"));
    report(writecliff::sliceFor(1000ull * 1000 * 1000 * 1000) == 5000000000ull / (8 * 1048576) * (8 * 1048576) + (5000000000ull % (8 * 1048576) ? 8 * 1048576 : 0)
               && writecliff::sliceFor(100 * 1048576) == 64 * 1048576,
           QStringLiteral("write speed: about 200 slices, never under 64 MiB"));

    for (const QString &where : {QDir::homePath() + QStringLiteral("/.cache"), QDir::tempPath()}) {
        QDir().mkpath(where);
        QTemporaryDir dir(where + QStringLiteral("/diskanvil-test-XXXXXX"));
        const QString file = dir.filePath(QStringLiteral("speed"));
        writecliff::Writer writer(file, -1, 192ull * 1048576);
        QVector<Point> points;
        bool completed = false;
        QString error;
        QObject::connect(&writer, &writecliff::Writer::point, [&points](const Point &p) { points << p; });
        QObject::connect(&writer, &writecliff::Writer::finished, [&](bool c, const QString &e) {
            completed = c;
            error = e;
        });
        writer.run();
        report(completed && points.size() == 3 && points.last().written == 192ull * 1048576 && points.first().mbps > 1 && !QFile::exists(file),
               QStringLiteral("write speed: 192 MiB written in %1 and timed in 3 slices, the file deleted").arg(where), error);
    }
}

// Hardware Check: a PC's parts read from made-up /proc and /sys files (a laptop's battery counted in
// energy, another in charge, a mouse's left out, a desktop whose name is the maker's filler), the
// processor's sensors found among the others, and what a stress run's numbers mean.
void hardwareCheck()
{
    QTemporaryDir dir;
    auto put = [&dir](const QString &path, const QByteArray &text) {
        QDir().mkpath(QFileInfo(dir.filePath(path)).absolutePath());
        QFile f(dir.filePath(path));
        if (f.open(QIODevice::WriteOnly))
            f.write(text);
    };
    put(QStringLiteral("cpuinfo"), "processor\t: 0\nmodel name\t: Test CPU 9000\nprocessor\t: 1\nmodel name\t: Test CPU 9000\n");
    put(QStringLiteral("meminfo"), "MemTotal:       16314752 kB\nMemFree: 1 kB\n");
    put(QStringLiteral("ps/BAT0/type"), "Battery\n");
    put(QStringLiteral("ps/BAT0/energy_full_design"), "57000000\n");
    put(QStringLiteral("ps/BAT0/energy_full"), "45600000\n");
    put(QStringLiteral("ps/BAT0/cycle_count"), "312\n");
    put(QStringLiteral("ps/BAT0/model_name"), "5B10W13930\n");
    put(QStringLiteral("ps/BAT1/type"), "Battery\n");
    put(QStringLiteral("ps/BAT1/charge_full_design"), "4000000\n");
    put(QStringLiteral("ps/BAT1/charge_full"), "2000000\n");
    put(QStringLiteral("ps/BAT1/voltage_min_design"), "11100000\n");
    put(QStringLiteral("ps/BAT1/cycle_count"), "0\n");
    put(QStringLiteral("ps/hidpp_battery_0/type"), "Battery\n");
    put(QStringLiteral("ps/hidpp_battery_0/scope"), "Device\n");
    put(QStringLiteral("ps/AC/type"), "Mains\n");
    put(QStringLiteral("dmi/sys_vendor"), "To Be Filled By O.E.M.\n");
    put(QStringLiteral("dmi/product_name"), "To Be Filled By O.E.M.\n");
    put(QStringLiteral("dmi/board_vendor"), "ASRock\n");
    put(QStringLiteral("dmi/board_name"), "B550M Pro4\n");
    put(QStringLiteral("dmi/bios_version"), "P3.10\n");
    put(QStringLiteral("dmi/bios_date"), "03/08/2023\n");
    const hardware::Summary s = hardware::readFrom(dir.filePath(QStringLiteral("cpuinfo")), dir.filePath(QStringLiteral("meminfo")),
                                                   dir.filePath(QStringLiteral("ps")), dir.filePath(QStringLiteral("dmi")));
    report(s.cpu == QLatin1String("Test CPU 9000") && s.threads == 2 && s.memory == 16314752ull * 1024 && s.board == QLatin1String("ASRock B550M Pro4")
               && s.firmware == QLatin1String("P3.10, from 03/08/2023"),
           QStringLiteral("hardware: processor, memory, the board when the PC's name is filler, firmware"), s.board + QLatin1Char(' ') + s.firmware);
    report(s.batteries.size() == 2 && s.batteries[0].wearPercent() == 20 && s.batteries[0].cycles == 312 && s.batteries[0].name.startsWith(QLatin1String("5B10W13930"))
               && qAbs(s.batteries[1].designWh - 44.4) < 0.01 && s.batteries[1].wearPercent() == 50 && s.batteries[1].cycles == -1,
           QStringLiteral("hardware: batteries in energy and in charge, their wear; a mouse's left out; a cycle count of 0 isn't one"),
           QStringLiteral("%1 batteries").arg(s.batteries.size()));
    put(QStringLiteral("hwmon/hwmon0/name"), "nvme\n");
    put(QStringLiteral("hwmon/hwmon0/temp1_input"), "99000\n");
    put(QStringLiteral("hwmon/hwmon1/name"), "k10temp\n");
    put(QStringLiteral("hwmon/hwmon1/temp1_input"), "61250\n");
    put(QStringLiteral("hwmon/hwmon1/temp3_input"), "58000\n");
    report(qAbs(hardware::cpuTemperatureFrom(dir.filePath(QStringLiteral("hwmon"))) - 61.25) < 0.01,
           QStringLiteral("hardware: the processor's own sensor, not the hotter SSD's"));

    QVector<hardware::Sample> cool, cooking;
    for (int i = 0; i < 60; ++i) {
        cool << hardware::Sample{double(i), 50.0 + i / 6.0, 4200};
        cooking << hardware::Sample{double(i), 70.0 + i / 2.0, i < 20 ? 3600.0 : 1800.0};
    }
    const hardware::StressResult fine = hardware::judge(cool, 40, 4500);
    report(!fine.tooHot && !fine.throttled && qAbs(fine.maxCelsius - 59.83) < 0.1, QStringLiteral("hardware: a PC that stays cool and fast is fine"));
    QVector<hardware::Sample> blind(30, hardware::Sample{1, -1, -1});
    hardware::StressResult unknown = hardware::judge(blind, -1, -1);
    unknown.completed = true;
    report(hardware::describe(unknown).startsWith(QLatin1String("This PC tells neither")) && !hardware::describe(unknown).contains(QLatin1String("cool")),
           QStringLiteral("hardware: a PC that tells neither its temperature nor its speed isn't called cool"), hardware::describe(unknown));
    const hardware::StressResult bad = hardware::judge(cooking, 45, 4000);
    report(bad.tooHot && bad.throttled && hardware::describe(bad).contains(QLatin1String("paste")),
           QStringLiteral("hardware: one that gets far too hot and slows itself down is told to clean it and see to the paste"), hardware::describe(bad));
}

void healthTests()
{
    burnInVerdict();
    writeCliff();
    hardwareCheck();
    hiddenArea();
    triageAdvice();
    driveFacts();
    farmHistory();
    slowRefresh();
    closedSets();
    productKeys();
    peVersions();
    bootMenu();
    linuxBootRepair();
    windowsRecovery();
    testBoot();
    usbConnection();
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
           QStringLiteral("a newer version is shown, and DiskAnvil doesn't install it"), firmware::describe(available));
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
           QStringLiteral("power settings change only what was picked, and keep keys DiskAnvil doesn't know"));
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
