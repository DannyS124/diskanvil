// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Scan for Viruses' third layer, the boot: Authenticode checked on boot programs this test
// signs itself (a throwaway certificate authority made here with OpenSSL), Secure Boot's lists
// and boot entries from a folder of made-up UEFI variables, and the EFI partition check.
//
// DISKFORGE_REAL_EFI=<folder>: also a real EFI partition, mounted there (read-only).

#include "testkit.h"

#include "../src/authenticode.h"
#include "../src/bootcheck.h"
#include "../src/efivars.h"
#include "testsign.h"

#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>

using namespace testsign;

namespace {

void putFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(data);
}

void realEfi()
{
    const QString esp = qEnvironmentVariable("DISKFORGE_REAL_EFI");
    if (esp.isEmpty()) {
        out << "SKIP  boot: no real EFI partition (DISKFORGE_REAL_EFI)" << Qt::endl;
        return;
    }
    // Against this PC's own Secure Boot lists, when it has them.
    const boot::Report r = boot::checkEfiPartition(esp, QString(), efi::trust());
    for (const QString &note : r.notes)
        out << "      " << note << Qt::endl;
    for (const boot::Finding &f : r.findings)
        out << "      " << f.severity << ' ' << f.where << ": " << f.verdict << ' ' << f.reasons.join(QStringLiteral(" / ")) << Qt::endl;
    bool windows = false;
    for (const boot::Finding &f : r.findings)
        windows = windows || (f.name.compare(QLatin1String("bootmgfw.efi"), Qt::CaseInsensitive) == 0 && f.severity == 0 && f.verdict.contains(QLatin1String("Microsoft")));
    report(windows, QStringLiteral("boot: a real Windows boot manager is signed by Microsoft and unchanged"));
}

} // namespace

void bootTests()
{
    QTemporaryDir dir;
    const Signer root = makeCertificate("DiskForge Test Root", nullptr, true);
    const Signer windows = makeCertificate("Microsoft Windows (DiskForge test)", &root, false);
    const Signer stranger = makeCertificate("Someone Else", nullptr, false);
    const QVector<QByteArray> db{derOf(root.cert)};

    // Authenticode
    const QByteArray unsigned_ = makeProgram("an unsigned boot program");
    authenticode::Result r = authenticode::check(unsigned_, db);
    report(r.state == authenticode::Result::State::Unsigned && r.sha256 == programHash(unsigned_),
           QStringLiteral("boot: an unsigned program, and its Authenticode hash"));
    const QByteArray signed_ = sign(makeProgram("a signed boot program"), windows, {windows.cert, root.cert});
    r = authenticode::check(signed_, db);
    report(r.state == authenticode::Result::State::Signed && r.trusted && r.signer == QLatin1String("Microsoft Windows (DiskForge test)")
               && r.issuer == QLatin1String("DiskForge Test Root") && r.trustedBy == QLatin1String("DiskForge Test Root") && r.certificates.size() == 2,
           QStringLiteral("boot: a signed program, trusted through the certificate that issued its signer"),
           QStringLiteral("%1 / %2 / %3").arg(int(r.state)).arg(r.signer, r.trustedBy));
    r = authenticode::check(signed_, {});
    report(r.state == authenticode::Result::State::Signed && !r.trusted, QStringLiteral("boot: signed, but nobody it knows vouches for it"));
    report(authenticode::check(signed_, {derOf(windows.cert)}).trusted, QStringLiteral("boot: trusted when the signer itself is in the list"));
    QByteArray changed = signed_;
    changed[0x250] = char(changed[0x250] ^ 1);
    report(authenticode::check(changed, db).state == authenticode::Result::State::Changed, QStringLiteral("boot: a program changed after it was signed"));
    QByteArray forged = signed_;
    const qsizetype blobEnd = signed_.size() - 1 - (signed_.size() - 0x400 - 8 - 1) % 8; // roughly the signature's last byte
    forged[blobEnd - 8] = char(forged[blobEnd - 8] ^ 0x5a);
    const authenticode::Result::State forgedState = authenticode::check(forged, db).state;
    report(forgedState == authenticode::Result::State::BadSignature || forgedState == authenticode::Result::State::Unreadable,
           QStringLiteral("boot: a signature that's been tampered with"), QString::number(int(forgedState)));
    const QByteArray strangers = sign(makeProgram("someone else's"), stranger, {stranger.cert});
    r = authenticode::check(strangers, db);
    report(r.state == authenticode::Result::State::Signed && !r.trusted && r.signer == QLatin1String("Someone Else"),
           QStringLiteral("boot: signed by a stranger"));
    report(authenticode::check("MZ, but nothing else", db).state == authenticode::Result::State::NotProgram, QStringLiteral("boot: not a program"));
    QRandomGenerator rng(1010);
    const int rounds = qEnvironmentVariableIntValue("DISKFORGE_FUZZ_ROUNDS") > 0 ? qEnvironmentVariableIntValue("DISKFORGE_FUZZ_ROUNDS") : 500;
    for (int round = 0; round < rounds; ++round) {
        QByteArray bytes = signed_;
        for (int i = 0, flips = 1 + int(rng.bounded(8)); i < flips; ++i) {
            const qsizetype at = rng.bounded(3) == 0 ? qsizetype(rng.bounded(0x400)) : 0x400 + qsizetype(rng.bounded(quint32(bytes.size() - 0x400)));
            bytes[at] = char(rng.bounded(256));
        }
        authenticode::check(bytes, db);
    }
    report(true, QStringLiteral("boot: %1 damaged signed programs checked without a crash").arg(rounds));

    // UEFI variables
    const QString vars = dir.filePath(QStringLiteral("efivars"));
    QDir().mkpath(vars);
    const QByteArray bannedProgram = sign(makeProgram("an old boot manager with a hole"), windows, {windows.cert, root.cert});
    const QString partition = QStringLiteral("0b5ea0a9-2c1f-4f6e-9d7a-3c4f5e6a7b8c");
    putVariable(vars, QStringLiteral("SecureBoot"), efi::kGlobal, QByteArray(1, '\1'));
    putVariable(vars, QStringLiteral("SetupMode"), efi::kGlobal, QByteArray(1, '\0'));
    putVariable(vars, QStringLiteral("db"), efi::kSecurity, signatureList("a5c059a1-94e4-4aa7-87b5-ab155c2bf072", {derOf(root.cert)}));
    putVariable(vars, QStringLiteral("dbx"), efi::kSecurity,
                signatureList("c1c41626-504c-4092-aca9-41f936934328", {authenticode::check(bannedProgram, db).sha256}));
    putVariable(vars, QStringLiteral("BootOrder"), efi::kGlobal, QByteArray("\x03\x00\x01\x00", 4));
    putVariable(vars, QStringLiteral("Boot0001"), efi::kGlobal, loadOption(QStringLiteral("Windows Boot Manager"), partition, QStringLiteral("\\EFI\\Microsoft\\Boot\\bootmgfw.efi")));
    putVariable(vars, QStringLiteral("Boot0003"), efi::kGlobal, loadOption(QStringLiteral("Something"), partition, QStringLiteral("\\EFI\\gone\\x.efi")));
    putVariable(vars, QStringLiteral("Boot0004"), efi::kGlobal,
                loadOption(QStringLiteral("Another drive"), QStringLiteral("11111111-2222-3333-4444-555555555555"), QStringLiteral("\\EFI\\BOOT\\BOOTX64.EFI")));
    const QString realVars = efi::varsDir;
    efi::varsDir = vars;
    const efi::Trust trust = efi::trust();
    const QVector<efi::BootEntry> entries = efi::bootEntries();
    efi::varsDir = realVars;
    report(trust.known && trust.secureBoot && !trust.setupMode && trust.allowed.certificates == db && trust.banned.sha256.size() == 1,
           QStringLiteral("boot: Secure Boot's state and lists, from the PC's variables"));
    QStringList order;
    for (const efi::BootEntry &e : entries)
        order << QStringLiteral("%1 %2 %3 %4").arg(e.number).arg(e.description, e.partition, e.path);
    report(entries.size() == 3 && entries[0].number == 3 && entries[1].number == 1 && entries[1].partition == partition
               && entries[1].path == QLatin1String("\\EFI\\Microsoft\\Boot\\bootmgfw.efi") && entries[2].number == 4,
           QStringLiteral("boot: the boot entries, in the firmware's order"), order.join(QStringLiteral(" | ")));

    // An EFI partition, and the Windows it starts.
    const QString esp = dir.filePath(QStringLiteral("esp")), win = dir.filePath(QStringLiteral("windows"));
    putFile(esp + QStringLiteral("/EFI/Microsoft/Boot/bootmgfw.efi"), unsigned_); // replaced by a bootkit
    putFile(esp + QStringLiteral("/EFI/Microsoft/Boot/memtest.efi"), changed);
    putFile(esp + QStringLiteral("/EFI/Boot/bootx64.efi"), signed_);
    putFile(esp + QStringLiteral("/EFI/arch/grubx64.efi"), makeProgram("grub, not signed"));
    putFile(esp + QStringLiteral("/EFI/old/banned.efi"), bannedProgram);
    putFile(esp + QStringLiteral("/EFI/Microsoft/Boot/BCD"), "not a program, and not checked");
    putFile(win + QStringLiteral("/Windows/Boot/EFI/bootmgfw.efi"), signed_);
    const boot::Report found = boot::checkEfiPartition(esp, win, trust);
    auto finding = [&found](const QString &name) -> const boot::Finding * {
        for (const boot::Finding &f : found.findings) {
            if (f.name == name)
                return &f;
        }
        return nullptr;
    };
    const boot::Finding *manager = finding(QStringLiteral("bootmgfw.efi"));
    const boot::Finding *memtest = finding(QStringLiteral("memtest.efi"));
    const boot::Finding *fallback = finding(QStringLiteral("bootx64.efi"));
    const boot::Finding *grub = finding(QStringLiteral("grubx64.efi"));
    const boot::Finding *old = finding(QStringLiteral("banned.efi"));
    auto describe = [](const boot::Finding *f) {
        return f ? QStringLiteral("[%1] %2 %3").arg(f->severity).arg(f->verdict, f->reasons.join(QLatin1Char(' '))) : QStringLiteral("(missing)");
    };
    report(found.findings.size() == 5 && found.notes.isEmpty(), QStringLiteral("boot: every .efi on the EFI partition is checked"),
           QString::number(found.findings.size()));
    report(manager && manager->severity == 2 && manager->cleanCopy == win + QStringLiteral("/Windows/Boot/EFI/bootmgfw.efi"),
           QStringLiteral("boot: an unsigned Windows boot manager, with Windows' own copy to put back"), describe(manager));
    report(memtest && memtest->severity == 2 && memtest->verdict.contains(QLatin1String("changed")), QStringLiteral("boot: a Windows boot program changed after signing"),
           describe(memtest));
    report(fallback && fallback->severity == 0 && fallback->verdict.contains(QLatin1String("trusts it")), QStringLiteral("boot: a trusted one is fine"),
           describe(fallback));
    report(grub && grub->severity == 1, QStringLiteral("boot: an unsigned boot loader with Secure Boot on"), describe(grub));
    report(old && old->severity == 2 && old->reasons.join(QLatin1Char(' ')).contains(QLatin1String("banned")),
           QStringLiteral("boot: one Secure Boot has banned"), describe(old));

    const QVector<boot::Finding> boots = boot::checkBootEntries(entries, partition, esp);
    report(boots.size() == 2 && boots[0].name == QLatin1String("Boot0003") && boots[0].severity == 1 && boots[1].severity == 0,
           QStringLiteral("boot: boot entries on this EFI partition, one for a file that's gone"),
           boots.isEmpty() ? QString() : boots[0].reasons.join(QLatin1Char(' ')));

    QString message;
    const QString backup = dir.filePath(QStringLiteral("backup"));
    const bool put = manager && boot::putBack(*manager, backup, &message);
    auto contents = [](const QString &path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    report(put && contents(esp + QStringLiteral("/EFI/Microsoft/Boot/bootmgfw.efi")) == signed_
               && contents(backup + QStringLiteral("/EFI_Microsoft_Boot_bootmgfw.efi")) == unsigned_,
           QStringLiteral("boot: Windows' own boot manager put back, the bad one kept"), message);

    // The first sector
    QByteArray sector(512, '\0');
    sector[510] = char(0x55);
    sector[511] = char(0xAA);
    report(boot::checkFirstSector(sector, true).severity == 0 && boot::checkFirstSector(sector, true).verdict.contains(QLatin1String("No boot code")),
           QStringLiteral("boot: a GPT drive's first sector without code"));
    QByteArray grubCode = sector;
    grubCode.replace(0x180, 4, "GRUB");
    report(boot::checkFirstSector(grubCode, false).verdict.contains(QLatin1String("GRUB")), QStringLiteral("boot: GRUB's code"));
    QByteArray unknown = sector;
    for (int i = 0; i < 300; ++i)
        unknown[i] = char(rng.bounded(256));
    report(boot::checkFirstSector(unknown, false).severity == 1 && boot::checkFirstSector(unknown, true).severity == 0,
           QStringLiteral("boot: code nobody knows matters only on a drive that starts the BIOS way"));
    QByteArray lookalike = unknown;
    lookalike.replace(0x163, 52, QByteArray("Invalid partition table\0Missing operating system\0\0\0", 52));
    report(boot::checkFirstSector(lookalike, false).severity == 1 && boot::checkFirstSector(lookalike, false).verdict.contains(QLatin1String("not Windows' code")),
           QStringLiteral("boot: Windows' messages around someone else's code"));
    // DISKFORGE_REAL_MBR=<file>: a disk (image) whose first sector Windows wrote.
    if (const QString mbr = qEnvironmentVariable("DISKFORGE_REAL_MBR"); !mbr.isEmpty()) {
        QFile f(mbr);
        const QByteArray first = f.open(QIODevice::ReadOnly) ? f.read(512) : QByteArray();
        report(boot::checkFirstSector(first, false).verdict == QLatin1String("Windows' boot code"), QStringLiteral("boot: a first sector Windows wrote"),
               boot::checkFirstSector(first, false).verdict);
    }

    // The firmware: Secure Boot off, a leaked test key as the platform key, and what fwupd
    // found (in its own words: a sample like a real PC's report).
    const Signer testKey = makeCertificate("DO NOT TRUST - AMI Test PK", nullptr, true);
    efi::Trust pc = trust;
    pc.secureBoot = false;
    pc.platformKeys = {derOf(testKey.cert)};
    const QByteArray fwupd = R"({"SecurityAttributes": [
        {"AppstreamId": "org.fwupd.hsi.Spi.Bioswe", "HsiResult": "enabled", "Name": "BIOS write enable", "Flags": ["action-contact-oem"]},
        {"AppstreamId": "org.fwupd.hsi.Iommu", "HsiResult": "enabled", "Name": "IOMMU", "Flags": ["success"]},
        {"AppstreamId": "org.fwupd.hsi.Kernel.Lockdown", "HsiResult": "not-enabled", "Name": "Linux kernel lockdown", "Flags": ["runtime-issue"]},
        {"AppstreamId": "org.fwupd.hsi.Amd.SmmLocked", "HsiResult": "locked", "Name": "SMM locked down", "Flags": ["success"]}]})";
    const QVector<boot::Finding> firmware = boot::checkFirmware(pc, fwupd);
    QStringList seen;
    for (const boot::Finding &f : firmware)
        seen << QStringLiteral("%1=%2").arg(f.name).arg(f.severity);
    report(seen == QStringList{QStringLiteral("Secure Boot=1"), QStringLiteral("Platform key=2"), QStringLiteral("BIOS write enable=2")},
           QStringLiteral("boot: the firmware's doors: Secure Boot off, a leaked test key, a writable firmware chip"), seen.join(QStringLiteral(", ")));
    report(boot::checkFirmware(efi::Trust(), "not json").isEmpty(), QStringLiteral("boot: nothing to say about firmware that can't be read"));
    release(testKey);

    for (const Signer &s : {root, windows, stranger})
        release(s);
    realEfi();
}
