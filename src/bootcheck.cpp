// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bootcheck.h"

#include "authenticode.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace boot {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("boot", text);
}

constexpr qint64 kLargestBootFile = 64 << 20;
constexpr int kMostBootFiles = 300;

QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (f.size() > kLargestBootFile || !f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

// A path under `root`, any case (an NTFS drive on Linux keeps the case).
QString caseless(const QString &root, const QString &relative)
{
    QString at = root;
    for (const QString &part : QString(relative).replace(QLatin1Char('\\'), QLatin1Char('/')).split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        QString next = at + QLatin1Char('/') + part;
        if (!QFileInfo::exists(next)) {
            next.clear();
            for (const QString &name : QDir(at).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
                if (name.compare(part, Qt::CaseInsensitive) == 0) {
                    next = at + QLatin1Char('/') + name;
                    break;
                }
            }
            if (next.isEmpty())
                return {};
        }
        at = next;
    }
    return at;
}

bool banned(const authenticode::Result &r, const efi::Trust &trust)
{
    if (trust.banned.sha256.contains(r.sha256))
        return true;
    for (const QByteArray &cert : r.certificates) {
        if (trust.banned.certificates.contains(cert))
            return true;
    }
    return false;
}

bool byMicrosoft(const authenticode::Result &r)
{
    return r.state == authenticode::Result::State::Signed && r.signer.contains(QLatin1String("Microsoft"), Qt::CaseInsensitive);
}

} // namespace

void Finding::flag(int level, const QString &reason)
{
    if (!reasons.contains(reason))
        reasons << reason;
    severity = std::max(severity, level);
}

Report checkEfiPartition(const QString &esp, const QString &windows, const efi::Trust &trust)
{
    Report report;
    if (!trust.known && efi::available())
        report.notes << tr("This PC's firmware has no Secure Boot lists to check these against (Secure Boot isn't set up on it).");
    else if (!trust.known)
        report.notes << tr("This PC started the old BIOS way, so there are no Secure Boot lists to check these against.");
    else if (trust.setupMode)
        report.notes << tr("Secure Boot is in setup mode on this PC: anything can change what it trusts. Its maker's settings "
                           "screen can put the usual keys back (\"Restore factory keys\").");
    else if (!trust.secureBoot)
        report.notes << tr("Secure Boot is off on this PC, so it starts boot programs whoever signed them.");

    // Windows' own copy of its boot manager, if it checks out: what can be put back.
    QString clean;
    if (!windows.isEmpty()) {
        const QString copy = caseless(windows, QStringLiteral("Windows/Boot/EFI/bootmgfw.efi"));
        if (!copy.isEmpty()) {
            const authenticode::Result r = authenticode::check(readFile(copy), trust.allowed.certificates);
            if (byMicrosoft(r) && !banned(r, trust) && (!trust.known || r.trusted))
                clean = copy;
        }
    }

    QDirIterator it(esp, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    int count = 0;
    while (it.hasNext() && count < kMostBootFiles) {
        const QString path = it.next();
        if (!path.endsWith(QLatin1String(".efi"), Qt::CaseInsensitive))
            continue;
        ++count;
        Finding f;
        f.file = path;
        f.name = it.fileName();
        f.where = QLatin1Char('\\') + QDir(esp).relativeFilePath(path).replace(QLatin1Char('/'), QLatin1Char('\\'));
        const QString lower = f.where.toLower();
        const bool windowsFile = lower.startsWith(QLatin1String("\\efi\\microsoft\\boot\\"));
        const bool fallback = lower.startsWith(QLatin1String("\\efi\\boot\\boot"));
        f.kind = lower.endsWith(QLatin1String("\\bootmgfw.efi")) ? tr("Windows' boot manager")
               : fallback                                         ? tr("The program the PC starts when nothing else is set")
               : windowsFile                                      ? tr("A Windows boot program")
                                                                  : tr("A boot program");
        const authenticode::Result r = authenticode::check(readFile(path), trust.allowed.certificates);
        using State = authenticode::Result::State;
        switch (r.state) {
        case State::NotProgram:
            f.verdict = tr("Not a boot program");
            f.flag(windowsFile ? 2 : 1, tr("It's named like a boot program but isn't one: damaged, or something hiding as one."));
            break;
        case State::Unsigned:
            f.verdict = tr("Not signed");
            if (windowsFile)
                f.flag(2, tr("Windows' boot programs are always signed by Microsoft; this one isn't. Bootkits replace them."));
            else if (trust.secureBoot)
                f.flag(1, tr("Secure Boot won't start it, as it isn't signed. If it's yours, that's expected."));
            break;
        case State::Unreadable:
        case State::BadSignature:
            f.verdict = tr("Its signature doesn't hold up");
            f.flag(windowsFile ? 2 : 1, tr("Its signature is damaged or not genuine."));
            break;
        case State::Changed:
            f.verdict = tr("Signed by %1, but changed since").arg(r.signer);
            f.flag(2, tr("It was changed after it was signed: a bootkit, or a damaged file."));
            break;
        case State::Signed:
            if (banned(r, trust))
                f.verdict = tr("Signed by %1, but banned by Secure Boot").arg(r.signer);
            else if (r.trusted)
                f.verdict = tr("Signed by %1; this PC's Secure Boot trusts it").arg(r.signer);
            else if (trust.known)
                f.verdict = tr("Signed by %1, which this PC's Secure Boot doesn't trust by itself").arg(r.signer);
            else
                f.verdict = tr("Signed by %1 (%2)").arg(r.signer, r.issuer);
            if (windowsFile && !byMicrosoft(r))
                f.flag(2, tr("It's in Windows' boot folder but isn't signed by Microsoft."));
            break;
        }
        if (r.state == State::Signed || r.state == State::Unsigned) {
            if (banned(r, trust))
                f.flag(2, tr("Secure Boot's list of banned boot programs names it: an old one with a known hole, as used by bootkits "
                             "like BlackLotus."));
        }
        if (f.severity >= 2 && !clean.isEmpty() && (lower.endsWith(QLatin1String("\\bootmgfw.efi")) || (fallback && windowsFile) || (fallback && r.signer.contains(QLatin1String("Microsoft")))))
            f.cleanCopy = clean;
        report.findings << f;
    }
    std::stable_sort(report.findings.begin(), report.findings.end(), [](const Finding &a, const Finding &b) { return a.severity > b.severity; });
    return report;
}

QVector<Finding> checkBootEntries(const QVector<efi::BootEntry> &entries, const QString &partition, const QString &esp)
{
    QVector<Finding> out;
    for (const efi::BootEntry &e : entries) {
        if (e.partition.isEmpty() || e.partition.compare(partition, Qt::CaseInsensitive) != 0)
            continue;
        Finding f;
        f.name = QStringLiteral("Boot%1").arg(e.number, 4, 16, QLatin1Char('0')).toUpper().replace(QLatin1String("BOOT"), QLatin1String("Boot"));
        f.kind = tr("A boot entry: %1").arg(e.description);
        f.where = e.path;
        f.verdict = e.active ? tr("Starts %1").arg(e.path) : tr("Switched off; it would start %1").arg(e.path);
        if (!e.path.isEmpty() && caseless(esp, e.path).isEmpty())
            f.flag(1, tr("It starts a file that isn't on the EFI partition."));
        out << f;
    }
    return out;
}

Finding checkFirstSector(const QByteArray &sector, bool uefiDrive)
{
    Finding f;
    f.name = tr("The first sector");
    f.kind = tr("Boot code for the old BIOS way");
    f.where = tr("Sector 0");
    if (sector.size() < 512 || uchar(sector[510]) != 0x55 || uchar(sector[511]) != 0xAA) {
        f.verdict = tr("No boot sector");
        return f;
    }
    const QByteArray code = sector.left(440);
    if (code.count('\0') == code.size()) {
        f.verdict = uefiDrive ? tr("No boot code: this drive starts with UEFI") : tr("No boot code");
        return f;
    }
    if (code.contains("GRUB")) {
        f.verdict = tr("GRUB's boot code (Linux)");
        return f;
    }
    if (code.contains("Rufus")) {
        f.verdict = tr("Rufus' boot code");
        return f;
    }
    // Windows 7 to 11 write the same code, with the messages after it in Windows' language.
    // Its SHA-256 (of the code, the first 0x163 bytes) was taken from a disk Windows 11 set up.
    static const QByteArray windowsCode = QByteArray::fromHex("088995559ab317af9b3291408da689651e8353f62e0a478d92eb0b5a947063fd");
    if (QCryptographicHash::hash(code.left(0x163), QCryptographicHash::Sha256) == windowsCode) {
        f.verdict = tr("Windows' boot code");
        return f;
    }
    if (code.contains("Invalid partition table") && code.contains("Missing operating system")) {
        f.verdict = tr("Windows' messages, but not Windows' code");
        if (!uefiDrive)
            f.flag(1, tr("It has Windows' boot messages, but the code around them isn't Windows': an old Windows version, or a bootkit that "
                         "kept the messages."));
        return f;
    }
    if (code.contains("SYSLINUX") || code.contains("Missing OS")) {
        f.verdict = tr("A boot manager's boot code (SYSLINUX or similar)");
        return f;
    }
    f.verdict = tr("Boot code this check doesn't know");
    if (!uefiDrive)
        f.flag(1, tr("Not the boot code of Windows or the usual Linux boot managers: another boot manager, or a bootkit."));
    return f;
}

QVector<Finding> checkFirmware(const efi::Trust &trust, const QByteArray &fwupdReport)
{
    QVector<Finding> out;
    const QString firmware = tr("This PC's firmware");
    if (trust.known) {
        Finding f;
        f.name = tr("Secure Boot");
        f.kind = firmware;
        f.where = tr("UEFI settings");
        if (trust.setupMode) {
            f.verdict = tr("In setup mode");
            f.flag(1, tr("Secure Boot is in setup mode: any program can change what it trusts. The firmware settings can put the "
                         "usual keys back (\"Restore factory keys\")."));
        } else if (trust.secureBoot) {
            f.verdict = tr("On");
        } else {
            f.verdict = tr("Off");
            f.flag(1, tr("Secure Boot is off, so anything can start the PC, bootkits too. Switch it on in the firmware settings "
                         "unless something you use needs it off."));
        }
        out << f;
        // PKfail: makers who shipped the test key from their firmware kit, whose private key
        // is public. With it, anyone can sign a bootkit this PC trusts.
        for (const QByteArray &key : trust.platformKeys) {
            const QString name = authenticode::certificateName(key);
            if (name.contains(QLatin1String("DO NOT TRUST"), Qt::CaseInsensitive) || name.contains(QLatin1String("DO NOT SHIP"), Qt::CaseInsensitive)) {
                Finding k;
                k.name = tr("Platform key");
                k.kind = firmware;
                k.where = tr("UEFI settings");
                k.verdict = name;
                k.flag(2, tr("Secure Boot's top key is a test key whose secret is public (\"PKfail\"): anyone can sign a bootkit "
                             "this PC trusts. A firmware update from the PC's maker replaces it."));
                out << k;
            }
        }
    }
    // fwupd's checks of the firmware itself; the ones about the running system (Linux's own
    // settings) aren't the PC's.
    struct Door {
        const char *id;
        int level;
        const char *words;
    };
    static const Door doors[] = {
        {"org.fwupd.hsi.Spi.Bioswe", 2,
         QT_TRANSLATE_NOOP("boot", "The chip the firmware is kept on can be written by any program with full rights: the way firmware "
                                   "malware like LoJax gets in.")},
        {"org.fwupd.hsi.Spi.Ble", 1, QT_TRANSLATE_NOOP("boot", "The firmware chip's write lock isn't switched on.")},
        {"org.fwupd.hsi.Spi.SmmBwp", 1, QT_TRANSLATE_NOOP("boot", "Writing to the firmware chip isn't limited to the firmware itself.")},
        {"org.fwupd.hsi.Spi.Descriptor", 1, QT_TRANSLATE_NOOP("boot", "The firmware chip's layout isn't locked.")},
        {"org.fwupd.hsi.Amd.SpiWriteProtection", 2, QT_TRANSLATE_NOOP("boot", "The firmware chip isn't protected against being rewritten.")},
        {"org.fwupd.hsi.Amd.SmmLocked", 1, QT_TRANSLATE_NOOP("boot", "The firmware's most protected memory (SMM) isn't locked.")},
        {"org.fwupd.hsi.Uefi.Pk", 2, QT_TRANSLATE_NOOP("boot", "Secure Boot's top key isn't a real one: anyone may be able to sign what this PC trusts.")},
        {"org.fwupd.hsi.Uefi.BootserviceVars", 1, QT_TRANSLATE_NOOP("boot", "Some of the firmware's settings can still be changed after it hands over to the system.")},
        {"org.fwupd.hsi.PlatformDebugLocked", 1, QT_TRANSLATE_NOOP("boot", "The processor's debugging access isn't locked.")},
        {"org.fwupd.hsi.IntelBootguard.Enabled", 0, QT_TRANSLATE_NOOP("boot", "Intel Boot Guard, which checks the firmware itself at power-on, isn't on.")},
    };
    const QJsonArray attributes = QJsonDocument::fromJson(fwupdReport).object().value(QLatin1String("SecurityAttributes")).toArray();
    for (const QJsonValue &value : attributes) {
        const QJsonObject a = value.toObject();
        const QString id = a.value(QLatin1String("AppstreamId")).toString();
        const bool ok = a.value(QLatin1String("Flags")).toArray().contains(QStringLiteral("success"));
        for (const Door &door : doors) {
            if (id != QLatin1String(door.id) || ok)
                continue;
            // Secure Boot's own state is told above from the PC itself.
            Finding f;
            f.name = a.value(QLatin1String("Name")).toString();
            f.kind = firmware;
            f.where = tr("Checked by fwupd");
            f.verdict = a.value(QLatin1String("HsiResult")).toString();
            f.flag(door.level, QCoreApplication::translate("boot", door.words));
            out << f;
        }
    }
    return out;
}

bool putBack(const Finding &finding, const QString &backup, QString *message)
{
    if (finding.cleanCopy.isEmpty() || finding.file.isEmpty()) {
        *message = tr("there's no clean copy to put back");
        return false;
    }
    QDir().mkpath(backup);
    const QString kept = backup + QLatin1Char('/') + QString(finding.where).replace(QLatin1Char('\\'), QLatin1Char('_')).mid(1);
    QFile::remove(kept);
    if (!QFile::copy(finding.file, kept)) {
        *message = tr("a backup of it couldn't be made, so it isn't changed");
        return false;
    }
    const QString next = finding.file + QStringLiteral(".diskforge-new");
    QFile::remove(next);
    if (!QFile::copy(finding.cleanCopy, next)) {
        *message = tr("Windows' copy couldn't be put there (is the drive full?)");
        return false;
    }
    // Swapped in once it's whole: a PC that loses power halfway still has one or the other.
    QFile::setPermissions(finding.file, QFile::permissions(finding.file) | QFileDevice::WriteOwner);
    if (!QFile::remove(finding.file) || !QFile::rename(next, finding.file)) {
        *message = tr("it couldn't be replaced");
        return false;
    }
    *message = tr("Windows' own copy is back (the old one is in the backup)");
    return true;
}

} // namespace boot
