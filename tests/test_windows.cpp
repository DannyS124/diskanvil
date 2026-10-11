// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Make a Windows USB without Windows: the WIM header and its XML (with real WIMs made by
// wimlib when it's there), the answer file for every mix of options, account names, keyboard
// layouts, and recognizing a Windows ISO from its files.

#include "testkit.h"

#include "../src/usb/windowsusb.h"

#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QXmlStreamReader>

namespace {

QByteArray utf16(const QString &text)
{
    QByteArray out("\xFF\xFE", 2);
    out.append(reinterpret_cast<const char *>(text.utf16()), text.size() * 2);
    return out;
}

const QString kXml = QStringLiteral(
    "<WIM><TOTALBYTES>7578075168</TOTALBYTES>"
    "<IMAGE INDEX=\"1\"><NAME>Windows 11 Home</NAME><WINDOWS><ARCH>9</ARCH><VERSION><MAJOR>10</MAJOR><MINOR>0</MINOR>"
    "<BUILD>26200</BUILD><SPBUILD>6584</SPBUILD></VERSION></WINDOWS></IMAGE>"
    "<IMAGE INDEX=\"2\"><NAME>Windows 11 Pro</NAME><WINDOWS><ARCH>12</ARCH><VERSION><BUILD>1</BUILD></VERSION></WINDOWS></IMAGE>"
    "</WIM>");

// Elements found in the answer file, as "pass/component/element" (and the text of a few).
QStringList outline(const QByteArray &xml, bool *wellFormed)
{
    QStringList found;
    QXmlStreamReader r(xml);
    QString pass, comp;
    while (!r.atEnd()) {
        if (r.readNext() != QXmlStreamReader::StartElement)
            continue;
        const QString name = r.name().toString();
        if (name == QLatin1String("settings"))
            pass = r.attributes().value(QStringLiteral("pass")).toString();
        else if (name == QLatin1String("component"))
            comp = r.attributes().value(QStringLiteral("name")).toString();
        else if (name == QLatin1String("Path") || name == QLatin1String("CommandLine") || name == QLatin1String("TimeZone")
                 || name == QLatin1String("Name") || name == QLatin1String("InputLocale"))
            found << pass + QLatin1Char('/') + comp + QLatin1Char('/') + name + QLatin1Char('=') + r.readElementText();
        else
            found << pass + QLatin1Char('/') + comp + QLatin1Char('/') + name;
    }
    *wellFormed = !r.hasError();
    return found;
}

bool has(const QStringList &outline, const QString &part)
{
    return std::any_of(outline.cbegin(), outline.cend(), [&](const QString &line) { return line.contains(part); });
}

void wimHeader(const QString &dir)
{
    const windowsusb::WimInfo info = windowsusb::parseWimXml(utf16(kXml));
    report(info.error.isEmpty() && info.name == QLatin1String("Windows 11 Home") && info.build == 26200 && info.arch == QLatin1String("amd64")
               && info.images == 2,
           QStringLiteral("a WIM's XML: the first image's name, build and architecture"), info.name);
    report(!windowsusb::parseWimXml("<WIM><IMAGE").error.isEmpty() && !windowsusb::parseWimXml(QByteArray()).error.isEmpty(),
           QStringLiteral("broken XML is turned down"));

    // A header like the real one: the XML's place and size at offset 72.
    const QByteArray xml = utf16(kXml);
    QByteArray file(208, '\0');
    file.replace(0, 8, QByteArray("MSWIM\0\0\0", 8));
    for (int i = 0; i < 7; ++i)
        file[72 + i] = char(quint64(xml.size()) >> (8 * i));
    for (int i = 0; i < 8; ++i)
        file[80 + i] = char(quint64(208) >> (8 * i));
    file += xml;
    QFile f(dir + QStringLiteral("/made.wim"));
    if (f.open(QIODevice::WriteOnly))
        f.write(file);
    f.close();
    report(windowsusb::readWim(f.fileName()).build == 26200, QStringLiteral("the XML is found from the WIM header"));
    QFile junk(dir + QStringLiteral("/junk.wim"));
    if (junk.open(QIODevice::WriteOnly))
        junk.write(QByteArray(4096, 'j'));
    junk.close();
    report(!windowsusb::readWim(junk.fileName()).error.isEmpty(), QStringLiteral("a file that isn't a WIM says so"));

    if (QStandardPaths::findExecutable(QStringLiteral("wimlib-imagex")).isEmpty()) {
        out << "SKIP  wimlib isn't installed, so there's no real WIM to read" << Qt::endl;
        return;
    }
    QDir().mkpath(dir + QStringLiteral("/content"));
    QFile c(dir + QStringLiteral("/content/hello.txt"));
    if (c.open(QIODevice::WriteOnly))
        c.write("hello");
    c.close();
    int code = -1;
    sh(QStringLiteral("wimlib-imagex"), {QStringLiteral("capture"), dir + QStringLiteral("/content"), dir + QStringLiteral("/real.wim"),
                                         QStringLiteral("Windows 11 Test")}, &code);
    const windowsusb::WimInfo real = windowsusb::readWim(dir + QStringLiteral("/real.wim"));
    report(code == 0 && real.error.isEmpty() && real.name == QLatin1String("Windows 11 Test"), QStringLiteral("a real WIM made by wimlib is read"), real.error);
}

// install.wim's parts checked against it, with parts split by wimlib: the same, damaged, one
// missing, another WIM's.
void splitParts(const QString &dir)
{
    if (QStandardPaths::findExecutable(QStringLiteral("wimlib-imagex")).isEmpty()) {
        out << "SKIP  wimlib isn't installed, so there's no WIM to split" << Qt::endl;
        return;
    }
    // Some files too big to squeeze, so there are several parts of a MiB.
    auto content = [&dir](const QString &name, quint32 seed) {
        QDir().mkpath(dir + QLatin1Char('/') + name + QStringLiteral("/sub"));
        QRandomGenerator random(seed);
        for (int i = 0; i < 6; ++i) {
            QByteArray data(700 * 1024, Qt::Uninitialized);
            random.fillRange(reinterpret_cast<quint32 *>(data.data()), data.size() / 4);
            QFile f(QStringLiteral("%1/%2/big%3.bin").arg(dir, name).arg(i));
            if (f.open(QIODevice::WriteOnly))
                f.write(data);
        }
        for (int i = 0; i < 40; ++i) {
            QFile f(QStringLiteral("%1/%2/sub/small%3.txt").arg(dir, name).arg(i));
            if (f.open(QIODevice::WriteOnly))
                f.write(QByteArray("line of text ").repeated(i * 20 + 1));
        }
        int code = -1;
        sh(QStringLiteral("wimlib-imagex"), {QStringLiteral("capture"), dir + QLatin1Char('/') + name, dir + QLatin1Char('/') + name + QStringLiteral(".wim"),
                                             QStringLiteral("Windows 11 Test")}, &code);
        QDir().mkpath(dir + QLatin1Char('/') + name + QStringLiteral("-parts"));
        int split = -1;
        sh(QStringLiteral("wimlib-imagex"), {QStringLiteral("split"), dir + QLatin1Char('/') + name + QStringLiteral(".wim"),
                                             dir + QLatin1Char('/') + name + QStringLiteral("-parts/install.swm"), QStringLiteral("1")}, &split);
        QStringList parts;
        for (const QString &part : QDir(dir + QLatin1Char('/') + name + QStringLiteral("-parts")).entryList({QStringLiteral("*.swm")}, QDir::Files))
            parts << dir + QLatin1Char('/') + name + QStringLiteral("-parts/") + part;
        return code == 0 && split == 0 ? parts : QStringList();
    };
    const QStringList parts = content(QStringLiteral("splitme"), 1);
    const QString original = dir + QStringLiteral("/splitme.wim");
    quint64 seen = 0, all = 0;
    const QString same = windowsusb::checkParts(original, parts, [&](quint64 done, quint64 total) {
        seen = done;
        all = total;
        return true;
    });
    report(parts.size() >= 3 && same.isEmpty() && seen == all && all > 4000000,
           QStringLiteral("install.wim's parts match it, every resource of it compared (%1 parts)").arg(parts.size()), same);

    QStringList fewer = parts;
    fewer.removeLast();
    report(!windowsusb::checkParts(original, fewer).isEmpty(), QStringLiteral("a part missing is found"));

    // One byte changed in the second part, in its first resource (right after the header).
    const QString damaged = dir + QStringLiteral("/damaged.swm");
    QFile::copy(parts.value(1), damaged);
    QFile d(damaged);
    if (d.open(QIODevice::ReadWrite) && d.seek(208 + 1000)) {
        char c = 0;
        d.peek(&c, 1);
        c = char(c ^ 0x55);
        d.write(&c, 1);
    }
    d.close();
    QStringList withDamage = parts;
    withDamage[1] = damaged;
    report(windowsusb::checkParts(original, withDamage).contains(QLatin1String("different")), QStringLiteral("a byte changed on the stick is found"));

    const QStringList others = content(QStringLiteral("another"), 2);
    report(!others.isEmpty() && !windowsusb::checkParts(original, others).isEmpty(), QStringLiteral("another WIM's parts don't match it"));
    report(!windowsusb::checkParts(original, parts, [](quint64, quint64) { return false; }).isEmpty(), QStringLiteral("the check stops when asked"));
}

void answerFile()
{
    windowsusb::Options all;
    all.localUser = QStringLiteral("Danny");
    all.sameRegion = true;
    all.userLocale = QStringLiteral("en-US");
    all.inputLocale = QStringLiteral("0409:00000409");
    all.timeZone = QStringLiteral("Central Standard Time");
    all.noBitLocker = true;
    bool ok = false;
    const QStringList everything = outline(windowsusb::unattendXml(all, QStringLiteral("amd64")), &ok);
    report(ok, QStringLiteral("the answer file is well-formed XML"));
    report(has(everything, QStringLiteral("windowsPE/Microsoft-Windows-Setup/Path=reg add HKLM\\SYSTEM\\Setup\\LabConfig /v BypassTPMCheck"))
               && has(everything, QStringLiteral("BypassSecureBootCheck")) && has(everything, QStringLiteral("BypassRAMCheck")),
           QStringLiteral("no TPM, Secure Boot or RAM check: LabConfig, before Setup checks the PC"));
    report(has(everything, QStringLiteral("specialize/Microsoft-Windows-Deployment/Path=reg add HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE /v BypassNRO")),
           QStringLiteral("no Microsoft account: BypassNRO"));
    report(has(everything, QStringLiteral("oobeSystem/Microsoft-Windows-Shell-Setup/Name=Danny"))
               && has(everything, QStringLiteral("CommandLine=net user \"Danny\" /logonpasswordchg:yes"))
               && has(everything, QStringLiteral("HideOnlineAccountScreens")),
           QStringLiteral("a local account, asked for a password at the first sign-in"));
    report(has(everything, QStringLiteral("ProtectYourPC")) && has(everything, QStringLiteral("TimeZone=Central Standard Time"))
               && has(everything, QStringLiteral("InputLocale=0409:00000409")) && !has(everything, QStringLiteral("UILanguage")),
           QStringLiteral("privacy, time zone and keyboard, but not the display language (the ISO may not have it)"));
    report(has(everything, QStringLiteral("PreventDeviceEncryption")) && has(everything, QStringLiteral("TCGSecurityActivationDisabled")),
           QStringLiteral("no automatic BitLocker"));

    windowsusb::Options none;
    none.skipChecks = none.noOnlineAccount = none.skipPrivacy = false;
    report(windowsusb::unattendXml(none, QStringLiteral("amd64")).isEmpty(), QStringLiteral("no options: no answer file at all"));
    windowsusb::Options onlyChecks = none;
    onlyChecks.skipChecks = true;
    const QStringList checks = outline(windowsusb::unattendXml(onlyChecks, QStringLiteral("arm64")), &ok);
    report(ok && has(checks, QStringLiteral("BypassTPMCheck")) && !has(checks, QStringLiteral("oobeSystem")) && !has(checks, QStringLiteral("specialize")),
           QStringLiteral("one option on: only what it needs"));
    windowsusb::Options tricky = none;
    tricky.localUser = QStringLiteral("A<b>&\"c");
    report(outline(windowsusb::unattendXml(tricky, QStringLiteral("amd64")), &ok).size() > 0 && ok,
           QStringLiteral("odd characters in a name can't break the XML"));
}

void names()
{
    QString error;
    report(windowsusb::userName(QStringLiteral("  Danny S  "), &error) == QLatin1String("Danny S"), QStringLiteral("a plain name stays as it is"));
    report(windowsusb::userName(QStringLiteral("a/b\\c:d"), &error) == QLatin1String("a_b_c_d"), QStringLiteral("characters Windows refuses become _"));
    report(windowsusb::userName(QStringLiteral("abcdefghijklmnopqrstuvwxyz"), &error).size() == 20, QStringLiteral("20 characters at most"));
    error.clear();
    report(windowsusb::userName(QStringLiteral("Administrator"), &error).isEmpty() && !error.isEmpty()
               && windowsusb::userName(QStringLiteral("guest"), &error).isEmpty() && windowsusb::userName(QStringLiteral("  "), &error).isEmpty(),
           QStringLiteral("names Windows keeps for itself, or none, are refused"), error);
    report(windowsusb::inputLocaleFor(QStringLiteral("us")) == QLatin1String("0409:00000409")
               && windowsusb::inputLocaleFor(QStringLiteral("de,us")) == QLatin1String("0407:00000407")
               && windowsusb::inputLocaleFor(QStringLiteral("klingon")).isEmpty(),
           QStringLiteral("keyboard layouts: the first one counts, unknown ones are left to Windows"));
    // The way tzdata.zi has them, a link to a link included.
    const QByteArray links = "# version 2026a\nL America/Chicago US/Central\nL Etc/UTC UTC\nL Asia/Kolkata Asia/Calcutta\n"
                             "L US/Central Made/Up\nZ America/Chicago -5:50:36 - LMT 1883 N 18 18u\n";
    const QString chicago = windowsusb::windowsTimeZone("America/Chicago", {});
    report(chicago == QLatin1String("Central Standard Time") && windowsusb::windowsTimeZone("US/Central", links) == chicago
               && windowsusb::windowsTimeZone("Made/Up", links) == chicago
               && windowsusb::windowsTimeZone("Asia/Calcutta", links) == QLatin1String("India Standard Time")
               && windowsusb::windowsTimeZone("US/Central", {}).isEmpty() == QTimeZone::ianaIdToWindowsId("US/Central").isEmpty()
               && windowsusb::windowsTimeZone("Nowhere/Special", links).isEmpty(),
           QStringLiteral("time zones: old names like US/Central go through tzdata's links"), chicago);
}

void recognizing(const QString &dir)
{
    // A tree like a mounted Windows ISO, with the capitals it really has.
    const QString root = dir + QStringLiteral("/winiso");
    QDir().mkpath(root + QStringLiteral("/Sources"));
    QDir().mkpath(root + QStringLiteral("/EFI/Boot"));
    for (const QString &file : {QStringLiteral("bootmgr.efi"), QStringLiteral("EFI/Boot/bootx64.efi"), QStringLiteral("setup.exe")}) {
        QFile f(root + QLatin1Char('/') + file);
        if (f.open(QIODevice::WriteOnly))
            f.write("x");
    }
    QFile::copy(dir + QStringLiteral("/made.wim"), root + QStringLiteral("/Sources/Install.wim"));
    const windowsusb::Info info = windowsusb::inspect(root);
    report(info.windows && info.install == QLatin1String("Sources/Install.wim") && info.wim.build == 26200 && info.windows11() && !info.needsSplit(),
           QStringLiteral("a Windows ISO is recognized from its files, whatever their case"), info.install);
    QFile::remove(root + QStringLiteral("/bootmgr.efi"));
    report(!windowsusb::inspect(root).windows, QStringLiteral("without its boot files it isn't one"));
    windowsusb::Info big;
    big.installSize = 4ULL * 1024 * 1024 * 1024;
    report(big.needsSplit() && windowsusb::kSplitMiB * 1024ULL * 1024 < 4ULL * 1024 * 1024 * 1024,
           QStringLiteral("install.wim of 4 GB or more is split, into parts FAT32 can hold"));
}

} // namespace

void windowsTests()
{
    QTemporaryDir dir;
    wimHeader(dir.path());
    splitParts(dir.path());
    answerFile();
    names();
    recognizing(dir.path());
}
