// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "whycrash.h"

#include "peinfo.h"
#include "reghive.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTimeZone>

#include <algorithm>

namespace whycrash {

namespace {

QString tr(const char *text, const char *disambiguation = nullptr, int n = -1)
{
    return QCoreApplication::translate("whycrash", text, disambiguation, n);
}

// Stop codes, from Microsoft's bug check reference, in plain words.
#define N(text) QT_TRANSLATE_NOOP("whycrash", text)
const StopCode kCodes[] = {
    {0x0a, "IRQL_NOT_LESS_OR_EQUAL", N("Code in Windows' core touched memory it wasn't allowed to at that moment."), N("Usually a driver; sometimes faulty memory (RAM)."), 4, false},
    {0x19, "BAD_POOL_HEADER", N("Windows found its own memory bookkeeping damaged."), N("A driver that wrote where it shouldn't, or faulty RAM."), 0, false},
    {0x1a, "MEMORY_MANAGEMENT", N("Windows' memory manager found its own records broken."), N("Very often faulty RAM; sometimes a driver."), 0, false},
    {0x1e, "KMODE_EXCEPTION_NOT_HANDLED", N("Code in Windows' core crashed and nothing caught it."), N("Usually a driver."), 2, false},
    {0x24, "NTFS_FILE_SYSTEM", N("The NTFS file system hit something it couldn't handle."), N("A damaged file system or failing disk; sometimes antivirus or backup software."), 0, false},
    {0x3b, "SYSTEM_SERVICE_EXCEPTION", N("A request a program made of Windows' core crashed inside it."), N("Usually a driver: graphics, antivirus and anti-cheat ones most often."), 2, false},
    {0x3d, "INTERRUPT_EXCEPTION_NOT_HANDLED", N("Something went wrong while a device was being answered."), N("A driver or the device."), 0, false},
    {0x44, "MULTIPLE_IRP_COMPLETE_REQUESTS", N("A driver finished the same job twice."), N("A driver bug."), 0, false},
    {0x4e, "PFN_LIST_CORRUPT", N("Windows' list of the memory in the PC was damaged."), N("Faulty RAM, or a driver."), 0, false},
    {0x50, "PAGE_FAULT_IN_NONPAGED_AREA", N("Windows' core reached for memory that isn't there."), N("A driver (one already removed too), faulty RAM, or antivirus."), 3, false},
    {0x74, "BAD_SYSTEM_CONFIG_INFO", N("Windows' settings (its registry) are damaged."), N("A damaged registry, often after a disk problem or a bad shutdown."), 0, false},
    {0x77, "KERNEL_STACK_INPAGE_ERROR", N("Part of Windows couldn't be read back from the disk."), N("The disk, its cable or controller; sometimes RAM."), 0, false},
    {0x7a, "KERNEL_DATA_INPAGE_ERROR", N("Windows couldn't read part of itself back from the disk."), N("The disk, its cable or controller; sometimes RAM."), 0, false},
    {0x7b, "INACCESSIBLE_BOOT_DEVICE", N("Windows couldn't reach its own drive while starting."), N("A changed SATA mode in the firmware settings, a storage driver, or a failing drive."), 0, false},
    {0x7e, "SYSTEM_THREAD_EXCEPTION_NOT_HANDLED", N("A part of Windows working in the background crashed."), N("Usually a driver."), 2, false},
    {0x7f, "UNEXPECTED_KERNEL_MODE_TRAP", N("The processor hit a problem Windows can't survive."), N("Hardware (overclocking, the processor, RAM), or drivers using too much of a stack."), 0, true},
    {0x80, "NMI_HARDWARE_FAILURE", N("The hardware signalled a fatal error."), N("Hardware."), 0, true},
    {0x8e, "KERNEL_MODE_EXCEPTION_NOT_HANDLED", N("Code in Windows' core crashed and nothing caught it."), N("Usually a driver; sometimes RAM."), 2, false},
    {0x9c, "MACHINE_CHECK_EXCEPTION", N("The processor reported a fatal hardware error."), N("The processor, RAM, motherboard or overclocking."), 0, true},
    {0x9f, "DRIVER_POWER_STATE_FAILURE", N("A device's driver didn't finish going to sleep or waking up in time."), N("A device driver: Wi-Fi, USB, graphics or storage most often."), 0, false},
    {0xa0, "INTERNAL_POWER_ERROR", N("Windows' power management hit a fatal error, often around hibernation."), N("The hibernation file, disk space, or drivers."), 0, false},
    {0xa5, "ACPI_BIOS_ERROR", N("The PC's firmware describes the hardware wrongly."), N("The firmware (BIOS/UEFI) needs an update."), 0, false},
    {0xbe, "ATTEMPTED_WRITE_TO_READONLY_MEMORY", N("A driver tried to write to memory that's read-only."), N("A driver."), 0, false},
    {0xc1, "SPECIAL_POOL_DETECTED_MEMORY_CORRUPTION", N("Driver Verifier caught a driver damaging memory."), N("A driver (Driver Verifier was on)."), 0, false},
    {0xc2, "BAD_POOL_CALLER", N("A driver used Windows' memory functions wrongly."), N("A driver."), 0, false},
    {0xc4, "DRIVER_VERIFIER_DETECTED_VIOLATION", N("Driver Verifier caught a driver breaking the rules."), N("A driver (Driver Verifier was on)."), 0, false},
    {0xc5, "DRIVER_CORRUPTED_EXPOOL", N("A driver damaged Windows' memory."), N("A driver."), 0, false},
    {0xce, "DRIVER_UNLOADED_WITHOUT_CANCELLING_PENDING_OPERATIONS", N("A driver was removed while it still had work going."), N("A driver."), 1, false},
    {0xd1, "DRIVER_IRQL_NOT_LESS_OR_EQUAL", N("A driver touched memory it wasn't allowed to at that moment."), N("A driver: network, USB, graphics and antivirus ones most often."), 4, false},
    {0xd5, "DRIVER_PAGE_FAULT_IN_FREED_SPECIAL_POOL", N("A driver used memory after giving it back."), N("A driver."), 3, false},
    {0xea, "THREAD_STUCK_IN_DEVICE_DRIVER", N("The graphics driver got stuck."), N("The graphics driver or the graphics card."), 0, false},
    {0xed, "UNMOUNTABLE_BOOT_VOLUME", N("Windows couldn't open its own drive."), N("A damaged file system, a failing drive or a loose cable."), 0, false},
    {0xef, "CRITICAL_PROCESS_DIED", N("A program Windows can't run without stopped."), N("Damaged system files, disk errors, malware, or it was ended on purpose."), 0, false},
    {0xf4, "CRITICAL_OBJECT_TERMINATION", N("A program Windows can't run without stopped."), N("Very often the disk; damaged system files."), 0, false},
    {0xf7, "DRIVER_OVERRAN_STACK_BUFFER", N("A driver wrote past the end of its space."), N("A driver, or malware."), 0, false},
    {0xfc, "ATTEMPTED_EXECUTE_OF_NOEXECUTE_MEMORY", N("Windows' core tried to run something that isn't code."), N("A driver bug, or malware."), 0, false},
    {0xfe, "BUGCODE_USB_DRIVER", N("A USB driver hit an error."), N("A USB device, its driver or the USB controller."), 0, false},
    {0x101, "CLOCK_WATCHDOG_TIMEOUT", N("One of the processor's cores stopped answering."), N("The processor, overclocking or its voltage, or the firmware."), 0, true},
    {0x109, "CRITICAL_STRUCTURE_CORRUPTION", N("Windows found its own core changed."), N("Faulty RAM, or software that patches Windows (cheats, some malware)."), 0, false},
    {0x10d, "WDF_VIOLATION", N("A driver broke the rules of Microsoft's driver framework."), N("A driver."), 0, false},
    {0x113, "VIDEO_DXGKRNL_FATAL_ERROR", N("Windows' graphics core hit a fatal error."), N("The graphics driver."), 0, false},
    {0x116, "VIDEO_TDR_FAILURE", N("The graphics driver froze and couldn't be restarted."), N("The graphics driver, or the card's overclock, heat or power."), 2, false},
    {0x117, "VIDEO_TDR_TIMEOUT_DETECTED", N("The graphics driver froze."), N("The graphics driver or card."), 2, false},
    {0x119, "VIDEO_SCHEDULER_INTERNAL_ERROR", N("The graphics scheduler hit a fatal error."), N("The graphics driver."), 0, false},
    {0x124, "WHEA_UNCORRECTABLE_ERROR", N("The hardware itself reported an error it couldn't fix."), N("Hardware: the processor, RAM, a PCI Express card or drive, overclocking, the power supply or heat."), 0, true},
    {0x12b, "FAULTY_HARDWARE_CORRUPTED_PAGE", N("A piece of memory changed by itself."), N("Faulty RAM."), 0, true},
    {0x133, "DPC_WATCHDOG_VIOLATION", N("A driver kept the processor busy for too long."), N("A driver: storage (or an SSD's firmware), network, graphics or sound."), 0, false},
    {0x139, "KERNEL_SECURITY_CHECK_FAILURE", N("Windows found its own data damaged."), N("A driver, or faulty RAM."), 0, false},
    {0x13a, "KERNEL_MODE_HEAP_CORRUPTION", N("Windows' memory was damaged."), N("A driver."), 0, false},
    {0x144, "BUGCODE_USB3_DRIVER", N("A USB 3 driver hit an error."), N("A USB device or its driver."), 0, false},
    {0x154, "UNEXPECTED_STORE_EXCEPTION", N("Windows' compressed memory hit an error."), N("The disk (page file) or RAM."), 0, false},
    {0x1c8, "MANUALLY_INITIATED_POWER_BUTTON_HOLD", N("The power button was held, and Windows wrote down what it was doing."), N("It was frozen, and someone held the power button."), 0, false},
    {0xe2, "MANUALLY_INITIATED_CRASH", N("Someone made it crash from the keyboard, on purpose."), N("Done on purpose (for testing)."), 0, false},
    {0xc000021a, "WINLOGON_FATAL_ERROR", N("A core part of Windows (sign-in or its subsystem) died."), N("Damaged system files or a bad update."), 0, false},
};
#undef N

// The WHEA, disk, file system and storage events worth putting next to a crash.
QString errorWords(const evtx::Event &e)
{
    const QString p = e.provider;
    if (p.contains(QLatin1String("WHEA"))) {
        if (e.id == 18 || e.id == 20)
            return tr("a fatal hardware error was reported");
        if (e.id == 17)
            return tr("a PCI Express device reported an error it corrected");
        if (e.id == 19)
            return tr("the processor reported an error it corrected");
        if (e.id == 47)
            return tr("memory reported an error it corrected");
        return tr("a hardware error was reported (WHEA %1)").arg(e.id);
    }
    if (p == QLatin1String("disk")) {
        if (e.id == 7)
            return tr("a bad block on %1").arg(e.at(0));
        if (e.id == 11)
            return tr("a controller error on %1").arg(e.at(0));
        if (e.id == 51)
            return tr("an error while Windows was paging on %1").arg(e.at(0));
        if (e.id == 153 || e.id == 154)
            return tr("a disk read or write had to be retried");
        return {};
    }
    if ((p == QLatin1String("storahci") || p == QLatin1String("stornvme") || p.startsWith(QLatin1String("iaStor"))) && e.id == 129)
        return tr("the storage controller was reset (it stopped answering)");
    if (p.contains(QLatin1String("Ntfs")) && e.id == 55)
        return tr("the file system was found damaged");
    if (p.contains(QLatin1String("Ntfs")) && e.id == 98 && e.value(QStringLiteral("CorruptionState")).toUInt() != 0)
        return tr("a volume needs a full check (chkdsk)");
    if (p == QLatin1String("Display") && e.id == 4101)
        return tr("the graphics driver (%1) stopped responding and was restarted").arg(e.at(0));
    if (p == QLatin1String("volmgr") && (e.id == 46 || e.id == 161 || e.id == 45))
        return tr("a crash dump couldn't be written");
    return {};
}

bool hardwareEvent(const evtx::Event &e)
{
    return e.provider.contains(QLatin1String("WHEA")) && (e.id == 18 || e.id == 20);
}

bool diskEvent(const evtx::Event &e)
{
    return e.provider == QLatin1String("disk") || e.provider == QLatin1String("storahci") || e.provider == QLatin1String("stornvme")
        || (e.provider.contains(QLatin1String("Ntfs")) && e.id == 55);
}

quint64 number(QString s)
{
    s = s.trimmed();
    bool ok = false;
    const quint64 v = s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive) ? s.mid(2).toULongLong(&ok, 16) : s.toULongLong(&ok);
    return ok ? v : 0;
}

QString codeText(quint32 code)
{
    return QStringLiteral("0x%1").arg(code, code > 0xffff ? 8 : 2, 16, QLatin1Char('0')).toUpper().replace(QLatin1String("0X"), QLatin1String("0x"));
}

bool windowsDriver(const QString &name)
{
    static const QSet<QString> core = {QStringLiteral("ntoskrnl.exe"), QStringLiteral("hal.dll"), QStringLiteral("ntfs.sys"), QStringLiteral("fltmgr.sys"),
                                       QStringLiteral("storport.sys"), QStringLiteral("wdf01000.sys"), QStringLiteral("ndis.sys"), QStringLiteral("tcpip.sys"),
                                       QStringLiteral("dxgkrnl.sys"), QStringLiteral("dxgmms2.sys"), QStringLiteral("acpi.sys"), QStringLiteral("pci.sys"),
                                       QStringLiteral("usbxhci.sys"), QStringLiteral("ucx01000.sys"), QStringLiteral("classpnp.sys"), QStringLiteral("disk.sys"),
                                       QStringLiteral("volmgr.sys"), QStringLiteral("netio.sys"), QStringLiteral("ksecdd.sys"), QStringLiteral("cng.sys"),
                                       QStringLiteral("win32k.sys"), QStringLiteral("win32kfull.sys"), QStringLiteral("win32kbase.sys"), QStringLiteral("stornvme.sys"),
                                       QStringLiteral("storahci.sys"), QStringLiteral("ataport.sys"), QStringLiteral("partmgr.sys"), QStringLiteral("volsnap.sys")};
    return core.contains(name.toLower());
}

// A blue screen's driver from its dump: the stop code's address, else the saved instruction.
void blameFromDump(const crashdump::Dump &dump, const StopCode *sc, Incident &inc, const std::function<QString(const QString &)> &describe)
{
    if (sc && sc->hardware) {
        inc.blame = tr("A hardware error: no driver is to blame.");
        return;
    }
    QString at;
    bool unloaded = false;
    if (sc && sc->addressParameter > 0)
        at = crashdump::where(dump, dump.parameters[size_t(sc->addressParameter - 1)], &unloaded);
    if (at.isEmpty() && dump.instruction)
        at = crashdump::where(dump, dump.instruction, &unloaded);
    const QString name = at.section(QLatin1Char('+'), 0, 0);
    const auto describeName = [&](const QString &n) {
        QString path;
        for (const crashdump::Driver &d : dump.drivers)
            if (d.name.compare(n, Qt::CaseInsensitive) == 0)
                path = d.path;
        const QString what = describe && !path.isEmpty() ? describe(path) : QString();
        return what.isEmpty() ? n : QStringLiteral("%1 (%2)").arg(n, what);
    };
    if (!name.isEmpty() && !windowsDriver(name)) {
        inc.driver = name;
        inc.blame = unloaded ? tr("It crashed in the code of %1, a driver that had already been removed.").arg(name)
                             : tr("It crashed in the code of %1.").arg(describeName(name));
        return;
    }
    // Windows' own code was running: the driver that handed it bad data is often further up the stack.
    QStringList others;
    for (const QString &n : dump.onStack)
        if (!windowsDriver(n))
            others << n;
    if (!name.isEmpty())
        inc.blame = tr("It crashed in Windows' own code (%1); the real cause is usually a driver that gave it bad data.").arg(name);
    if (!others.isEmpty()) {
        inc.driver = others.first();
        inc.blame += (inc.blame.isEmpty() ? QString() : QStringLiteral(" "))
                   + tr("Drivers seen on the crashing part's stack: %1 (a guess: an old leftover can show up there too).").arg(others.mid(0, 3).join(QStringLiteral(", ")));
    }
}

QString onDisk(const QString &root, const QString &windowsPath);

} // namespace

const StopCode *stopCode(quint32 code)
{
    if ((code & 0xf0000000) == 0x10000000)
        code &= ~0x10000000u; // the "_M" twins
    for (const StopCode &c : kCodes)
        if (c.code == code)
            return &c;
    return nullptr;
}

QString kindName(Kind kind)
{
    switch (kind) {
    case Kind::BlueScreen:
        return tr("Blue screen");
    case Kind::PowerButton:
        return tr("Power button held");
    case Kind::PowerLost:
        return tr("Went off suddenly");
    case Kind::Sleep:
        return tr("Died going to sleep or waking");
    case Kind::Recovered:
        return tr("Screen froze and came back");
    }
    return {};
}

Records::Wer parseWer(const QByteArray &file)
{
    Records::Wer w;
    QString text;
    if (file.startsWith("\xff\xfe"))
        text = QString::fromUtf16(reinterpret_cast<const char16_t *>(file.constData() + 2), (file.size() - 2) / 2);
    else
        text = QString::fromUtf8(file);
    QHash<QString, QString> v;
    for (const QString &line : text.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts)) {
        const int eq = int(line.indexOf(QLatin1Char('=')));
        if (eq > 0)
            v.insert(line.left(eq).trimmed().toLower(), line.mid(eq + 1).trimmed());
    }
    w.type = v.value(QStringLiteral("eventtype"));
    const quint64 ft = v.value(QStringLiteral("eventtime")).toULongLong();
    if (ft)
        w.when = QDateTime::fromMSecsSinceEpoch(qint64(ft / 10000) - 11644473600000LL, QTimeZone::UTC);
    // The signature: the code, then the four numbers (by name when it has them, else in order).
    const auto sig = [&](int i) { return v.value(QStringLiteral("sig[%1].value").arg(i)); };
    w.code = quint32(sig(0).toULongLong(nullptr, 16));
    for (int i = 0; i < 4; ++i)
        w.parameters[size_t(i)] = sig(i + 1).toULongLong(nullptr, 16);
    return w;
}

Report explain(const Records &r)
{
    Report out;
    const QVector<evtx::Event> &events = r.system.events;
    if (!events.isEmpty())
        out.from = events.first().time;
    QVector<int> usedDumps, usedWer;
    const auto near = [](const QDateTime &a, const QDateTime &b, qint64 seconds) { return a.isValid() && b.isValid() && std::abs(a.secsTo(b)) <= seconds; };
    const auto errorsBefore = [&](const QDateTime &when, Incident &inc) {
        if (!when.isValid())
            return;
        int shown = 0;
        for (const evtx::Event &e : events) {
            if (!e.time.isValid() || e.time > when || e.time.secsTo(when) > 30 * 60)
                continue;
            const QString words = errorWords(e);
            if (words.isEmpty() || shown >= 6)
                continue;
            ++shown;
            inc.before << tr("%1 (%2 before)").arg(words).arg(e.time.secsTo(when) < 90 ? tr("%n second(s)", nullptr, int(e.time.secsTo(when)))
                                                                                         : tr("%n minute(s)", nullptr, int(e.time.secsTo(when) / 60)));
        }
    };
    const auto addDump = [&](Incident &inc, int index) {
        const auto &[file, dump] = r.dumps[index];
        usedDumps << index;
        inc.sources << tr("crash dump %1").arg(file);
        const StopCode *sc = stopCode(dump.code);
        blameFromDump(dump, sc, inc, r.describeDriver);
        if (!dump.complete)
            inc.sources.last() += tr(" (not written to the end)");
    };
    const auto blueScreen = [&](Incident &inc, quint32 code) {
        inc.kind = Kind::BlueScreen;
        inc.code = code;
        const StopCode *sc = stopCode(code);
        inc.title = sc ? tr("Blue screen: %1 (%2)").arg(QString::fromLatin1(sc->name), codeText(code)) : tr("Blue screen: stop code %1").arg(codeText(code));
        inc.explanation = sc ? tr(sc->meaning) + QLatin1Char(' ') + tr("Usual cause: %1").arg(tr(sc->cause))
                             : tr("A stop code this doesn't know: look it up in Microsoft's bug check list.");
        if (code == 0x1c8)
            inc.kind = Kind::PowerButton;
    };

    // Each stop Windows noticed at the next start (Kernel-Power 41).
    for (int i = 0; i < events.size(); ++i) {
        const evtx::Event &e = events[i];
        if (!(e.provider == QLatin1String("Microsoft-Windows-Kernel-Power") && e.id == 41))
            continue;
        Incident inc;
        inc.sources << tr("event log: Kernel-Power 41 at %1").arg(e.time.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        // When it actually stopped: the "previous shutdown was unexpected" event (6008) has it.
        inc.when = e.time;
        for (int j = std::max(0, i - 40); j < std::min<int>(events.size(), i + 40); ++j) {
            const evtx::Event &u = events[j];
            if (u.provider == QLatin1String("EventLog") && u.id == 6008 && near(u.time, e.time, 600)) {
                // Its binary part: SYSTEMTIME local, then SYSTEMTIME UTC.
                const QByteArray b = QByteArray::fromHex(u.binary.toLatin1());
                if (b.size() >= 32) {
                    const auto w = [&b](int at) { return quint16(uchar(b[at]) | (uchar(b[at + 1]) << 8)); };
                    const QDateTime t(QDate(w(16), w(18), w(22)), QTime(w(24), w(26), w(28)), QTimeZone::UTC);
                    if (t.isValid())
                        inc.when = t;
                }
                inc.sources << tr("event log: unexpected shutdown (6008)");
            }
        }
        const quint32 code = quint32(number(e.value(QStringLiteral("BugcheckCode"))));
        const bool button = number(e.value(QStringLiteral("PowerButtonTimestamp"))) != 0
                         || e.value(QStringLiteral("LongPowerButtonPressDetected")).compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
        const QString sleeping = e.value(QStringLiteral("SleepInProgress"));
        if (code != 0) {
            blueScreen(inc, code);
            // Its dump: the one written at about that time, or the one Windows' report names.
            for (int d = 0; d < r.dumps.size(); ++d) {
                if (!usedDumps.contains(d) && r.dumps[d].second.code == code && near(r.dumps[d].second.time, inc.when, 20 * 60)) {
                    addDump(inc, d);
                    break;
                }
            }
        } else if (button) {
            inc.kind = Kind::PowerButton;
            inc.title = tr("The power button was held");
            inc.explanation = tr("Someone held the power button to turn it off. That's usually because it had frozen: a frozen PC "
                                 "is often a driver, the disk, or overheating.");
        } else if (!sleeping.isEmpty() && sleeping != QLatin1String("false") && sleeping != QLatin1String("0")) {
            inc.kind = Kind::Sleep;
            inc.title = tr("It died going to sleep or waking up");
            inc.explanation = tr("It stopped while going to sleep or waking. That's usually a device's driver (Wi-Fi, USB, graphics) or "
                                 "the firmware.");
        } else {
            inc.kind = Kind::PowerLost;
            inc.title = tr("It went off suddenly");
            inc.explanation = tr("Windows stopped without a blue screen and without shutting down: the power was cut, or it froze so "
                                 "hard nothing could be written. A power cut, a failing power supply, overheating, or a hardware hang.");
        }
        // Windows' own record of the blue screen (WER 1001), when there is one.
        for (int j = std::max(0, i - 40); j < std::min<int>(events.size(), i + 60); ++j) {
            const evtx::Event &b = events[j];
            if ((b.provider == QLatin1String("Microsoft-Windows-WER-SystemErrorReporting") || b.provider == QLatin1String("BugCheck")) && b.id == 1001
                && near(b.time, e.time, 900)) {
                inc.sources << tr("event log: blue screen report (1001)");
                const QString dumpPath = b.at(1).section(QLatin1Char('\\'), -1);
                if (!dumpPath.isEmpty() && inc.sources.filter(dumpPath).isEmpty()) {
                    for (int d = 0; d < r.dumps.size(); ++d)
                        if (!usedDumps.contains(d) && r.dumps[d].first.compare(dumpPath, Qt::CaseInsensitive) == 0)
                            addDump(inc, d);
                }
            }
        }
        errorsBefore(inc.when, inc);
        out.incidents << inc;
    }
    // Dumps the log doesn't reach back to (it's been overwritten since).
    for (int d = 0; d < r.dumps.size(); ++d) {
        if (usedDumps.contains(d) || !r.dumps[d].second.ok)
            continue;
        Incident inc;
        inc.when = r.dumps[d].second.time;
        blueScreen(inc, r.dumps[d].second.code);
        addDump(inc, d);
        errorsBefore(inc.when, inc);
        out.incidents << inc;
    }
    // Windows Error Reporting's notes: blue screens with neither, and graphics freezes it recovered from.
    for (int w = 0; w < r.wer.size(); ++w) {
        const Records::Wer &wer = r.wer[w];
        if (wer.type.compare(QLatin1String("BlueScreen"), Qt::CaseInsensitive) == 0) {
            const bool known = std::any_of(out.incidents.cbegin(), out.incidents.cend(), [&](const Incident &i) { return i.code == wer.code && near(i.when, wer.when, 3600); });
            if (known || wer.code == 0)
                continue;
            Incident inc;
            inc.when = wer.when;
            blueScreen(inc, wer.code);
            inc.sources << tr("Windows Error Reporting: %1").arg(wer.file);
            out.incidents << inc;
        } else if (wer.type.compare(QLatin1String("LiveKernelEvent"), Qt::CaseInsensitive) == 0 && (wer.code == 0x141 || wer.code == 0x117 || wer.code == 0x1a8)) {
            Incident inc;
            inc.kind = Kind::Recovered;
            inc.when = wer.when;
            inc.code = wer.code;
            inc.title = tr("The screen froze and came back");
            inc.explanation = tr("The graphics driver stopped responding, and Windows restarted it without a blue screen. Now and then is "
                                 "normal; often means the graphics driver, or the card's overclock, heat or power.");
            inc.sources << tr("Windows Error Reporting: %1").arg(wer.file);
            out.incidents << inc;
        }
    }
    std::sort(out.incidents.begin(), out.incidents.end(), [](const Incident &a, const Incident &b) { return a.when > b.when; });

    // What they have in common.
    QMap<QString, int> drivers;
    QSet<quint32> codes;
    int blue = 0, lost = 0, hardware = 0, disk = 0;
    for (const Incident &inc : out.incidents) {
        if (inc.kind == Kind::Recovered)
            continue;
        if (inc.kind == Kind::BlueScreen) {
            ++blue;
            codes.insert(inc.code & ~0x10000000u);
            if (const StopCode *sc = stopCode(inc.code); sc && sc->hardware)
                ++hardware;
        }
        if (inc.kind == Kind::PowerLost)
            ++lost;
        if (!inc.driver.isEmpty())
            ++drivers[inc.driver];
    }
    for (const evtx::Event &e : events) {
        if (hardwareEvent(e))
            ++hardware;
        if (diskEvent(e))
            ++disk;
    }
    const int stops = int(std::count_if(out.incidents.cbegin(), out.incidents.cend(), [](const Incident &i) { return i.kind != Kind::Recovered; }));
    if (stops == 0) {
        out.summary << tr("No crashes or sudden stops were found%1.").arg(out.from.isValid() ? tr(" since %1").arg(out.from.toLocalTime().toString(QStringLiteral("d MMM yyyy"))) : QString());
    } else {
        out.summary << tr("Unexpected stops: %1 (blue screens: %2; sudden power-offs or hard freezes: %3).").arg(stops).arg(blue).arg(lost);
        for (auto it = drivers.cbegin(); it != drivers.cend(); ++it)
            if (it.value() >= 2 || (it.value() == 1 && blue == 1))
                out.summary << tr("%1 was involved in %n of them: most likely that driver. Update it from its maker (or roll it back if it "
                                  "started after an update).", nullptr, it.value()).arg(it.key());
        if (hardware > 0)
            out.summary << tr("The hardware reported errors: test the RAM (Memory Test in DiskAnvil Live's boot menu), and undo any "
                              "overclocking; check the temperatures and the power supply.");
        if (blue >= 3 && codes.size() >= 3 && drivers.isEmpty())
            out.summary << tr("Different stop codes each time, with no driver in common: that usually means hardware (RAM, overclocking, "
                              "heat or the power supply) rather than one bad driver. Test the RAM first.");
        if (disk > 0)
            out.summary << tr("The disk had errors (%n logged): check it in Disk Health, and copy your files off first if it's failing.", nullptr, disk);
        if (lost >= 2)
            out.summary << tr("It went off suddenly more than once: check the power cable and supply, and that it isn't overheating "
                              "(dust in the fans).");
    }
    // Settings that hide crashes.
    if (r.crashDumpEnabled == 0)
        out.notes << tr("Windows is set not to save crash dumps (CrashDumpEnabled is 0), so the blue screens' details aren't kept. "
                        "System Properties, Advanced, Startup and Recovery, \"Write debugging information\": Small memory dump.");
    if (r.autoReboot && blue > 0)
        out.notes << tr("Windows restarts by itself after a blue screen, which is why it may never have been seen.");
    if (r.system.events.isEmpty())
        out.notes << tr("The System event log couldn't be read%1.").arg(r.system.problem.isEmpty() ? QString() : QStringLiteral(" (") + r.system.problem + QLatin1Char(')'));
    if (r.system.broken > 0)
        out.notes << tr("%n event(s) in the log were damaged and skipped.", nullptr, r.system.broken);
    return out;
}

namespace {

QString onDisk(const QString &root, const QString &windowsPath)
{
    // "C:\Windows\..." (or "\SystemRoot\...") on the mounted drive, in any case.
    QString path = windowsPath;
    path.replace(QLatin1Char('/'), QLatin1Char('\\'));
    if (path.startsWith(QLatin1String("\\SystemRoot\\"), Qt::CaseInsensitive))
        path = QStringLiteral("C:\\Windows\\") + path.mid(12);
    else if (path.startsWith(QLatin1String("\\??\\")))
        path = path.mid(4);
    if (path.size() < 3 || path[1] != QLatin1Char(':'))
        return {};
    if (root.isEmpty())
        return QDir::fromNativeSeparators(path);
    QString at = root;
    for (const QString &part : path.mid(3).split(QLatin1Char('\\'), Qt::SkipEmptyParts)) {
        QString next = at + QLatin1Char('/') + part;
        if (!QFileInfo::exists(next)) {
            next.clear();
            const QStringList names = QDir(at).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
            for (const QString &n : names)
                if (n.compare(part, Qt::CaseInsensitive) == 0)
                    next = at + QLatin1Char('/') + n;
            if (next.isEmpty())
                return {};
        }
        at = next;
    }
    return at;
}

QByteArray readFile(const QString &path, qint64 most)
{
    QFile f(path);
    if (path.isEmpty() || !f.open(QIODevice::ReadOnly))
        return {};
    return f.read(most);
}

} // namespace

Report read(const QString &root, const std::atomic<bool> *cancel)
{
    Records r;
    // The System log. The running Windows keeps it open: a copy is made with its own wevtutil.
    QByteArray log = readFile(onDisk(root, QStringLiteral("C:\\Windows\\System32\\winevt\\Logs\\System.evtx")), 256 * 1024 * 1024);
#ifdef Q_OS_WIN
    QTemporaryDir copy;
    if (log.isEmpty() && root.isEmpty() && copy.isValid()) {
        const QString to = QDir::toNativeSeparators(copy.filePath(QStringLiteral("System.evtx")));
        QProcess::execute(QStringLiteral("wevtutil.exe"), {QStringLiteral("epl"), QStringLiteral("System"), to});
        log = readFile(QDir::fromNativeSeparators(to), 256 * 1024 * 1024);
    }
#endif
    r.system = evtx::read(log, cancel);

    // The dumps: the small ones whole, MEMORY.DMP's header only.
    const QString minidumps = onDisk(root, QStringLiteral("C:\\Windows\\Minidump"));
    if (!minidumps.isEmpty()) {
        QDirIterator it(minidumps, {QStringLiteral("*.dmp"), QStringLiteral("*.DMP")}, QDir::Files);
        while (it.hasNext() && r.dumps.size() < 200 && !(cancel && *cancel)) {
            const QString path = it.next();
            crashdump::Dump d = crashdump::read(readFile(path, 64 * 1024 * 1024));
            if (d.ok)
                r.dumps.push_back({QFileInfo(path).fileName(), d});
        }
    }
    const QString memory = onDisk(root, QStringLiteral("C:\\Windows\\MEMORY.DMP"));
    if (!memory.isEmpty()) {
        crashdump::Dump d = crashdump::read(readFile(memory, 0x2000));
        if (d.ok)
            r.dumps.push_back({QStringLiteral("MEMORY.DMP"), d});
    }
    // Windows Error Reporting's notes.
    for (const QString &folder : {QStringLiteral("C:\\ProgramData\\Microsoft\\Windows\\WER\\ReportArchive"), QStringLiteral("C:\\ProgramData\\Microsoft\\Windows\\WER\\ReportQueue")}) {
        const QString at = onDisk(root, folder);
        if (at.isEmpty())
            continue;
        QDirIterator it(at, QDir::Dirs | QDir::NoDotAndDotDot);
        while (it.hasNext() && r.wer.size() < 500) {
            const QString dir = it.next();
            const QString name = QFileInfo(dir).fileName();
            if (!name.startsWith(QLatin1String("Kernel_"), Qt::CaseInsensitive) && !name.startsWith(QLatin1String("LiveKernelEvent"), Qt::CaseInsensitive)
                && !name.contains(QLatin1String("BlueScreen"), Qt::CaseInsensitive))
                continue;
            Records::Wer w = parseWer(readFile(dir + QStringLiteral("/Report.wer"), 1024 * 1024));
            w.file = name;
            if (!w.type.isEmpty())
                r.wer << w;
        }
    }
    // The crash settings, from the SYSTEM hive (not the running Windows': it's in use).
    if (!root.isEmpty()) {
        reg::Hive hive;
        QString error;
        if (hive.open(onDisk(root, QStringLiteral("C:\\Windows\\System32\\config\\SYSTEM")), &error)) {
            int current = 1;
            if (const auto v = hive.value(hive.key(QStringLiteral("Select")), QStringLiteral("Current")))
                current = int(v->number());
            const reg::Key crash = hive.key(QStringLiteral("ControlSet%1\\Control\\CrashControl").arg(current, 3, 10, QLatin1Char('0')));
            if (crash) {
                if (const auto v = hive.value(crash, QStringLiteral("CrashDumpEnabled")))
                    r.crashDumpEnabled = int(v->number());
                if (const auto v = hive.value(crash, QStringLiteral("AutoReboot")))
                    r.autoReboot = v->number() != 0;
            }
        }
    }
    r.describeDriver = [root](const QString &driverPath) {
        const QHash<QString, QString> v = peinfo::versionStrings(readFile(onDisk(root, driverPath), 64 * 1024 * 1024));
        const QString what = v.value(QStringLiteral("FileDescription")), who = v.value(QStringLiteral("CompanyName"));
        if (what.isEmpty() && who.isEmpty())
            return QString();
        return what.isEmpty() ? who : who.isEmpty() ? what : what + QStringLiteral(", ") + who;
    };
    return explain(r);
}

QString asText(const Report &report)
{
    QStringList out;
    out << tr("Why did it crash?");
    out << report.summary;
    for (const QString &n : report.notes)
        out << tr("Note: %1").arg(n);
    out << QString();
    for (const Incident &inc : report.incidents) {
        out << QStringLiteral("%1  %2").arg(inc.when.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")), inc.title);
        out << QStringLiteral("    ") + inc.explanation;
        if (!inc.blame.isEmpty())
            out << QStringLiteral("    ") + inc.blame;
        for (const QString &b : inc.before)
            out << QStringLiteral("    ") + tr("Before it: %1").arg(b);
        out << QStringLiteral("    ") + tr("From: %1").arg(inc.sources.join(QStringLiteral("; ")));
    }
    return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

} // namespace whycrash
