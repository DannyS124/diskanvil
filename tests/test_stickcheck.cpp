// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Check a USB Stick without a stick: one in memory that can be genuine, wrap around past its
// real size (most fake sticks), or drop what's written past it, and have spots that fail to
// write, fail to read, or flip bits.

#include "testkit.h"

#include "../src/drives/writeprotect.h"
#include "../src/usb/stickcheck.h"

#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>

#include <algorithm>
#include <cstring>

namespace {

constexpr quint64 kMiB = 1024 * 1024;

class MemStick : public stickcheck::Target
{
public:
    enum class Past { Wraps, Drops };
    struct Bad {
        quint64 offset;
        quint64 length;
        enum Kind { WriteFails, ReadFails, Flips } kind;
    };

    MemStick(quint64 claimed, quint64 real, Past past = Past::Wraps)
        : m_claimed(claimed)
        , m_past(past)
        , m_data(qsizetype(real), '\0')
    {
    }
    QVector<Bad> bad;

    quint64 size() const override { return m_claimed; }
    bool write(quint64 offset, const char *data, qsizetype length) override
    {
        if (hits(offset, length, Bad::WriteFails))
            return false;
        if (offset + quint64(length) <= quint64(m_data.size())) {
            std::memcpy(m_data.data() + offset, data, size_t(length));
            return true;
        }
        for (qsizetype i = 0; i < length; ++i) {
            const quint64 at = offset + quint64(i);
            if (at < quint64(m_data.size()))
                m_data[qsizetype(at)] = data[i];
            else if (m_past == Past::Wraps)
                m_data[qsizetype(at % quint64(m_data.size()))] = data[i];
        }
        return true;
    }
    bool read(quint64 offset, char *data, qsizetype length) override
    {
        if (hits(offset, length, Bad::ReadFails))
            return false;
        if (offset + quint64(length) <= quint64(m_data.size())) {
            std::memcpy(data, m_data.constData() + offset, size_t(length));
        } else {
            for (qsizetype i = 0; i < length; ++i) {
                const quint64 at = offset + quint64(i);
                if (at < quint64(m_data.size()))
                    data[i] = m_data[qsizetype(at)];
                else
                    data[i] = m_past == Past::Wraps ? m_data[qsizetype(at % quint64(m_data.size()))] : '\0';
            }
        }
        if (hits(offset, length, Bad::Flips))
            data[length / 2] = char(data[length / 2] ^ 0x10);
        return true;
    }
    bool flush() override { return true; }

private:
    bool hits(quint64 offset, qsizetype length, Bad::Kind kind) const
    {
        return std::any_of(bad.cbegin(), bad.cend(), [&](const Bad &b) {
            return b.kind == kind && b.offset < offset + quint64(length) && offset < b.offset + b.length;
        });
    }
    quint64 m_claimed;
    Past m_past;
    QByteArray m_data;
};

stickcheck::Result check(std::unique_ptr<MemStick> stick, stickcheck::Mode mode, bool stopHalfway = false)
{
    stickcheck::Checker checker(std::move(stick), mode);
    stickcheck::Result result;
    QObject::connect(&checker, &stickcheck::Checker::finished, [&](const stickcheck::Result &r) { result = r; });
    if (stopHalfway) {
        QObject::connect(&checker, &stickcheck::Checker::progress, [&](const QString &, quint64 done, quint64 total) {
            if (done * 2 >= total)
                checker.cancel();
        });
    }
    checker.run();
    return result;
}

QString describe(const stickcheck::Result &r)
{
    return QStringLiteral("fake %1, real %2 MiB, write %3, read %4, wrong %5")
        .arg(r.fake)
        .arg(r.real / kMiB)
        .arg(r.writeErrors)
        .arg(r.readErrors)
        .arg(r.wrongData);
}

const char *name(stickcheck::Mode mode)
{
    return mode == stickcheck::Mode::Quick ? "quick" : mode == stickcheck::Mode::Full ? "full" : "full, twice";
}

} // namespace

namespace {

void writeProtectTests()
{
    using namespace writeprotect;
    const auto has = [](const QVector<Finding> &found, Fix fix) {
        return std::any_of(found.cbegin(), found.cend(), [fix](const Finding &f) { return f.fix == fix; });
    };
    State stick;
    stick.device = QStringLiteral("/dev/sdz");
    stick.name = QStringLiteral("Test Stick");
    Part p1;
    p1.device = QStringLiteral("/dev/sdz1");
    p1.name = QStringLiteral("STICK");
    p1.fsType = QStringLiteral("vfat");
    stick.parts << p1;

    report(diagnose(stick).isEmpty() && !nothingFound(stick).isEmpty(), QStringLiteral("write-protected: a writable stick has nothing found, and says what else it can be"));

    State worn = stick;
    worn.drivesays = 1;
    worn.kernelReadOnly = true;
    worn.parts[0].mountedReadOnly = true;
    QVector<Finding> found = diagnose(worn);
    report(found.size() == 1 && found[0].fix == Fix::None && found[0].text.contains(QLatin1String("wearing out")),
           QStringLiteral("write-protected: a stick that says so itself: only that, no fix offered, said honestly"), found.value(0).title);

    State card = worn;
    card.sdCard = true;
    found = diagnose(card);
    report(found.size() == 1 && found[0].title.contains(QLatin1String("lock switch")) && found[0].text.contains(QLatin1String("slide")),
           QStringLiteral("write-protected: a card: its lock switch"));
    State slot = stick;
    slot.sdCard = slot.cardSlot = slot.kernelReadOnly = true;
    report(diagnose(slot).value(0).title.contains(QLatin1String("lock switch")), QStringLiteral("write-protected: a card in the PC's own slot, read-only: its switch"));

    State flagged = stick;
    flagged.kernelReadOnly = true;
    flagged.drivesays = 0;
    found = diagnose(flagged);
    report(found.size() == 1 && found[0].fix == Fix::ClearKernelFlag, QStringLiteral("write-protected: Linux's own read-only flag: cleared as root"));
    const QStringList script = clearKernelFlagScript(flagged);
    report(script.value(2) == QLatin1String("blockdev --setrw '/dev/sdz' '/dev/sdz1'"), QStringLiteral("write-protected: the flag cleared on the drive and its partitions"), script.value(2));
    State odd = flagged;
    odd.device = QStringLiteral("/dev/sd'z; reboot");
    report(clearKernelFlagScript(odd).value(2).startsWith(QLatin1String("blockdev --setrw '/dev/sd'\\''z; reboot'")), QStringLiteral("write-protected: a device name can't run anything"));

    State stayed = flagged;
    stayed.drivesays = -1;
    stayed.flagClearedBefore = true;
    found = diagnose(stayed);
    report(found.size() == 1 && found[0].fix == Fix::None && found[0].text.contains(QLatin1String("wearing out")),
           QStringLiteral("write-protected: Linux's flag back after being cleared: it's the drive, said so"));
    State replug = stick;
    replug.windows = replug.policyClearedBefore = true;
    replug.drivesays = 1;
    report(diagnose(replug).value(0).title.contains(QLatin1String("Unplug")), QStringLiteral("write-protected: Windows' policy just switched off: unplug and plug in, not \"worn out\""));
    report(askDevice(QStringLiteral("/etc/passwd")) == -1 && askDevice(QStringLiteral("/dev/sda; x")) == -1, QStringLiteral("write-protected: only a /dev/sdX is ever opened to ask"));

    State blocked = flagged;
    blocked.writeBlock = true;
    found = diagnose(blocked);
    report(found.size() == 1 && found[0].fix == Fix::AllowWriteBlock, QStringLiteral("write-protected: DiskAnvil Live's write protection: let it through, nothing else said"));

    State mounted = stick;
    mounted.parts[0].mountedReadOnly = true;
    found = diagnose(mounted);
    report(found.size() == 1 && found[0].fix == Fix::RepairAndReopen && found[0].part == 0, QStringLiteral("write-protected: opened read-only: repaired and opened again"));
    mounted.parts[0].fsType = QStringLiteral("ntfs");
    report(diagnose(mounted).value(0).fix == Fix::Reopen && diagnose(mounted).value(0).text.contains(QLatin1String("Shift")),
           QStringLiteral("write-protected: NTFS open read-only: Fast Startup explained"));
    mounted.parts[0].fsType = QStringLiteral("iso9660");
    report(diagnose(mounted).value(0).fix == Fix::None, QStringLiteral("write-protected: a disc image's file system: nothing to fix, said why"));

    State windows = stick;
    windows.windows = true;
    windows.policyWriteProtect = windows.diskAttributeReadOnly = true;
    windows.policyDenyWrite << QStringLiteral("Removable Disks: Deny write access");
    windows.parts[0].attributeReadOnly = true;
    windows.parts[0].mountedReadOnly = true;
    found = diagnose(windows);
    report(found.size() == 4 && has(found, Fix::ClearPolicy) && has(found, Fix::ClearDiskAttribute) && has(found, Fix::ClearVolumeAttribute)
               && std::any_of(found.cbegin(), found.cend(), [](const Finding &f) { return f.fix == Fix::None && f.text.contains(QLatin1String("gpedit")); }),
           QStringLiteral("write-protected: Windows: its policy, an administrator's, the disk's and the volume's marks"), QString::number(found.size()));

    // Reading Linux's flags and mounts: from a /sys and a mount table made here.
    QTemporaryDir dir;
    QDir(dir.path()).mkpath(QStringLiteral("class/block/sdz"));
    QDir(dir.path()).mkpath(QStringLiteral("class/block/sdz1"));
    const auto write = [&dir](const QString &name, const QByteArray &text) {
        QFile f(dir.path() + QLatin1Char('/') + name);
        return f.open(QIODevice::WriteOnly) && f.write(text) == text.size();
    };
    write(QStringLiteral("class/block/sdz/ro"), "0\n");
    write(QStringLiteral("class/block/sdz1/ro"), "1\n");
    write(QStringLiteral("mountinfo"),
          "22 1 8:1 / / rw,relatime shared:1 - ext4 /dev/sda1 rw\n"
          "90 22 8:17 / /run/media/me/MY\\040STICK ro,nosuid,nodev,relatime shared:50 - vfat /dev/sdz1 rw,fmask=0022\n");
    State read = stick;
    readLinux(read, dir.path(), dir.path() + QStringLiteral("/mountinfo"));
    report(!read.kernelReadOnly && read.parts[0].kernelReadOnly && read.parts[0].mountedReadOnly && read.parts[0].mountPoint == QLatin1String("/run/media/me/MY STICK"),
           QStringLiteral("write-protected: Linux's flags and read-only mounts read"), read.parts[0].mountPoint);
    report(askDrive(-1) == -1, QStringLiteral("write-protected: no drive to ask: not known, not guessed"));
}

} // namespace

void stickCheckTests()
{
    using stickcheck::Mode;
    const Mode modes[] = {Mode::Quick, Mode::Full, Mode::FullTwice};

    for (Mode mode : modes) {
        const stickcheck::Result r = check(std::make_unique<MemStick>(128 * kMiB, 128 * kMiB), mode);
        report(r.good() && r.real == 128 * kMiB, QStringLiteral("%1: a good stick is good").arg(QLatin1String(name(mode))), describe(r));
    }

    // Fake ones that wrap around, at a power of two and at an odd size.
    for (Mode mode : {Mode::Quick, Mode::Full}) {
        for (quint64 real : {64 * kMiB, 100 * kMiB}) {
            const stickcheck::Result r = check(std::make_unique<MemStick>(256 * kMiB, real), mode);
            report(r.completed && r.fake && r.wraps && r.real == real,
                   QStringLiteral("%1: a 256 MiB stick that wraps at %2 MiB is caught, with its real size")
                       .arg(QLatin1String(name(mode)))
                       .arg(real / kMiB),
                   describe(r));
        }
        const stickcheck::Result dropped = check(std::make_unique<MemStick>(256 * kMiB, 80 * kMiB, MemStick::Past::Drops), mode);
        report(dropped.completed && dropped.fake && !dropped.wraps && dropped.real == 80 * kMiB,
               QStringLiteral("%1: one that loses everything past 80 MiB is caught too").arg(QLatin1String(name(mode))), describe(dropped));
    }

    // A genuine stick with bad spots is failing, not fake.
    for (Mode mode : {Mode::Quick, Mode::Full}) {
        auto stick = std::make_unique<MemStick>(128 * kMiB, 128 * kMiB);
        stick->bad = {{50 * kMiB, 2 * kMiB, MemStick::Bad::Flips},
                      {70 * kMiB, kMiB, MemStick::Bad::ReadFails},
                      {90 * kMiB, kMiB, MemStick::Bad::WriteFails}};
        const stickcheck::Result r = check(std::move(stick), mode);
        const bool where = r.bad.contains(50 * kMiB) && r.bad.contains(70 * kMiB) && r.bad.contains(90 * kMiB);
        report(r.completed && !r.fake && r.wrongData > 0 && r.readErrors > 0 && r.writeErrors > 0 && where,
               QStringLiteral("%1: bad spots on a genuine stick are found and told apart").arg(QLatin1String(name(mode))), describe(r));
    }

    for (Mode mode : {Mode::Quick, Mode::Full}) {
        const stickcheck::Result r = check(std::make_unique<MemStick>(64 * kMiB, 64 * kMiB), mode, true);
        report(!r.completed && r.error == QLatin1String("Stopped."), QStringLiteral("%1: Stop stops it").arg(QLatin1String(name(mode))),
               r.error);
    }

    const QVector<quint64> spots = stickcheck::quickSpots(quint64(1) << 40);
    bool aligned = true;
    for (quint64 s : spots)
        aligned = aligned && s % 4096 == 0;
    report(spots.size() <= 65537 && spots.first() == 0 && spots.last() == (quint64(1) << 40) - 4096 && aligned,
           QStringLiteral("a 1 TB stick gets at most 65537 stamps, from the first byte to the last"), QString::number(spots.size()));
    report(stickcheck::estimate(Mode::Full, 64 * 1000 * kMiB) > stickcheck::estimate(Mode::Quick, 64 * 1000 * kMiB) * 20,
           QStringLiteral("the full check is said to take much longer than the quick one"));

    // Random fakes and bad spots: never a crash, never more than it claims, and a wrap without
    // bad spots always found to the MiB.
    QRandomGenerator rng(20261010);
    bool sane = true, exact = true;
    for (int round = 0; round < 120; ++round) {
        const quint64 claimed = (16 + rng.bounded(48)) * kMiB;
        const quint64 real = (1 + rng.bounded(quint32(claimed / kMiB))) * kMiB;
        const bool wraps = rng.bounded(2) == 0;
        auto stick = std::make_unique<MemStick>(claimed, real, wraps ? MemStick::Past::Wraps : MemStick::Past::Drops);
        const bool withBad = rng.bounded(3) == 0;
        if (withBad) {
            for (int i = 0; i < 3; ++i)
                stick->bad.push_back({rng.bounded(quint32(claimed / kMiB)) * kMiB, kMiB, MemStick::Bad::Kind(rng.bounded(3))});
        }
        const Mode mode = round % 4 == 0 ? Mode::Full : Mode::Quick;
        const stickcheck::Result r = check(std::move(stick), mode);
        sane = sane && r.completed && r.real <= claimed;
        if (!withBad && real < claimed && (wraps ? !(r.fake && r.real == real) : !(r.fake && r.real == real))) {
            exact = false;
            out << "      round " << round << ": claimed " << claimed / kMiB << " real " << real / kMiB << (wraps ? " wraps" : " drops")
                << " -> " << describe(r) << Qt::endl;
        }
    }
    report(sane, QStringLiteral("120 random fakes and bad spots: no crash, never more than it claims"));
    report(exact, QStringLiteral("and a fake without bad spots is always found, to the MiB"));
    writeProtectTests();
}
