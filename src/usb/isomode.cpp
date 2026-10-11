// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "isomode.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QSet>

namespace {

constexpr quint64 kFat32Limit = 4ULL * 1024 * 1024 * 1024; // 4 GiB, minus a byte

QString tr(const char *text)
{
    return QCoreApplication::translate("isomode", text);
}

// The first word of a boot menu line, lower case.
QByteArray firstWord(const QByteArray &line)
{
    const QByteArray t = line.trimmed();
    qsizetype end = 0;
    while (end < t.size() && !QChar::isSpace(uchar(t[end])))
        ++end;
    return t.left(end).toLower();
}

// The lines that look for the stick by its label, or start the kernel.
bool searchesLabel(const QByteArray &word)
{
    static const QSet<QByteArray> words = {"linux", "linuxefi", "linux16", "$linux", "append", "options", "search", "set", "kernel", "for"};
    return words.contains(word);
}

bool startsKernel(const QByteArray &word)
{
    static const QSet<QByteArray> words = {"linux", "linuxefi", "linux16", "$linux", "append", "options", "kernel"};
    return words.contains(word);
}

// Whether `word` (like "boot=live") is in `line` as a whole word.
qsizetype findWord(const QByteArray &line, const QByteArray &word)
{
    qsizetype at = 0;
    while ((at = line.indexOf(word, at)) >= 0) {
        const bool startOk = at == 0 || QChar::isSpace(uchar(line[at - 1]));
        const qsizetype end = at + word.size();
        const bool endOk = end >= line.size() || QChar::isSpace(uchar(line[end]));
        if (startOk && endOk)
            return at;
        at = end;
    }
    return -1;
}

QByteArray addPersistence(QByteArray line, isomode::Persistence persistence)
{
    if (persistence == isomode::Persistence::Casper) {
        if (findWord(line, "persistent") >= 0)
            return line;
        const qsizetype boot = findWord(line, "boot=casper");
        if (boot >= 0)
            return line.insert(boot + 11, " persistent");
        // Ubuntu's own menus have no boot=casper: the kernel path says it.
        const qsizetype kernel = line.indexOf("/casper/vmlinuz");
        if (kernel >= 0) {
            qsizetype end = kernel;
            while (end < line.size() && !QChar::isSpace(uchar(line[end])))
                ++end;
            return line.insert(end, " persistent");
        }
    } else if (persistence == isomode::Persistence::LiveBoot) {
        if (findWord(line, "persistence") >= 0)
            return line;
        const qsizetype boot = findWord(line, "boot=live");
        if (boot >= 0)
            return line.insert(boot + 9, " persistence");
    }
    return line;
}

} // namespace

QString isomode::Analysis::whyNot() const
{
    if (windows)
        return tr("This is a Windows ISO: Make a Windows USB (in the File menu) does those.");
    if (!efi)
        return tr("This ISO can't start a UEFI PC from its files, only written as it is.");
    if (!writeAsIs.isEmpty())
        return writeAsIs;
    if (!bigFile.isEmpty())
        return tr("%1 is over 4 GB, and FAT32 can't hold a file that big.").arg(bigFile);
    return {};
}

isomode::Analysis isomode::analyse(const QVector<filecopy::Entry> &entries, const QString &isoLabel)
{
    Analysis a;
    a.isoLabel = isoLabel;
    a.fatLabel = fatLabel(isoLabel);
    bool installWim = false, bootmgr = false, casper = false, live = false;
    for (const filecopy::Entry &e : entries) {
        const QString path = e.path.toLower();
        if (!e.isDir) {
            a.bytes += e.size;
            if (e.size >= kFat32Limit && a.bigFile.isEmpty())
                a.bigFile = e.path;
        }
        if (path == QLatin1String("efi/boot/bootx64.efi"))
            a.efi = true;
        else if (path == QLatin1String("sources/install.wim") || path == QLatin1String("sources/install.esd"))
            installWim = true;
        else if (path == QLatin1String("bootmgr") || path == QLatin1String("bootmgr.efi"))
            bootmgr = true;
        else if (path.startsWith(QLatin1String("casper/")))
            casper = true;
        else if (path == QLatin1String("live/filesystem.squashfs"))
            live = true;
        // The ones Rufus has found only start written as they are.
        if (path == QLatin1String("proxmox") || path.startsWith(QLatin1String("proxmox/")))
            a.writeAsIs = tr("Proxmox only starts when it's written as it is.");
        else if (path == QLatin1String(".miso"))
            a.writeAsIs = tr("Manjaro only starts when it's written as it is.");
    }
    // Microsoft's ISOs keep their files in UDF; the ISO 9660 side only has a README saying so.
    const bool onlyReadme = entries.size() == 1 && entries.first().path.compare(QLatin1String("README.TXT"), Qt::CaseInsensitive) == 0;
    a.windows = (installWim && bootmgr) || onlyReadme;
    if (isoLabel.startsWith(QLatin1String("Install-SUSE")) || isoLabel.startsWith(QLatin1String("Install-LEAP"))
        || isoLabel.startsWith(QLatin1String("openSUSE-Tumbleweed")))
        a.writeAsIs = tr("openSUSE only starts when it's written as it is.");
    else if (casper && isoLabel.contains(QLatin1String("Pop_OS"), Qt::CaseInsensitive))
        a.writeAsIs = tr("Pop!_OS only starts when it's written as it is.");
    a.persistence = casper ? Persistence::Casper : live ? Persistence::LiveBoot : Persistence::None;
    return a;
}

QString isomode::fatLabel(const QString &isoLabel)
{
    QString label;
    for (const QChar c : isoLabel.toUpper()) {
        if (c.unicode() < 0x20 || c.unicode() > 0x7e || QStringLiteral("*?,;:/\\|+=<>[]\"").contains(c))
            continue;
        // Dots and spaces are allowed, but boot lines would need them escaped: keep it simple.
        label += (c == QLatin1Char('.') || c == QLatin1Char(' ')) ? QLatin1Char('_') : c;
    }
    label.truncate(11);
    return label.isEmpty() ? QStringLiteral("LIVEUSB") : label;
}

bool isomode::isBootConfig(const QString &path)
{
    const QString lower = path.toLower();
    return lower.endsWith(QLatin1String(".cfg")) || (lower.startsWith(QLatin1String("loader/entries/")) && lower.endsWith(QLatin1String(".conf")));
}

QByteArray isomode::patchConfig(const QByteArray &text, const QString &isoLabel, const QString &fatLabel, Persistence persistence)
{
    const QByteArray from = isoLabel.toUtf8();
    const QByteArray fromEscaped = QByteArray(from).replace(' ', "\\x20"); // how GRUB writes spaces
    const QByteArray to = fatLabel.toUtf8();
    const bool swap = !from.isEmpty() && from != to;
    QByteArray out;
    out.reserve(text.size() + 64);
    qsizetype start = 0;
    while (start < text.size()) {
        qsizetype end = text.indexOf('\n', start);
        end = end < 0 ? text.size() : end + 1;
        QByteArray line = text.mid(start, end - start);
        const QByteArray word = firstWord(line);
        if (swap && searchesLabel(word)) {
            line.replace(fromEscaped, to);
            line.replace(from, to);
        }
        if (persistence != Persistence::None && startsKernel(word)) {
            // Persistence goes before the line ending.
            qsizetype body = line.size();
            while (body > 0 && (line[body - 1] == '\n' || line[body - 1] == '\r'))
                --body;
            line = addPersistence(line.left(body), persistence) + line.mid(body);
        }
        out += line;
        start = end;
    }
    return out;
}

QString isomode::persistenceLabel(Persistence persistence)
{
    return persistence == Persistence::Casper ? QStringLiteral("casper-rw") : QStringLiteral("persistence");
}

QByteArray isomode::persistenceConf()
{
    // The line end matters: Debian live gives up on the file without it.
    return "/ union\n";
}
