// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "windowsusb.h"

#include "blockio.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QProcess>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTimeZone>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

constexpr quint64 kFat32Limit = 4ULL * 1024 * 1024 * 1024;
constexpr qint64 kMaxXml = 16 * 1024 * 1024;

QString tr(const char *text)
{
    return QCoreApplication::translate("windowsusb", text);
}

quint64 le64(const uchar *p, int bytes)
{
    quint64 v = 0;
    for (int i = 0; i < bytes; ++i)
        v |= quint64(p[i]) << (8 * i);
    return v;
}

// One component of the answer file, with the attributes Setup wants on every one.
void component(QXmlStreamWriter &x, const QString &name, const QString &arch)
{
    x.writeStartElement(QStringLiteral("component"));
    x.writeAttribute(QStringLiteral("name"), name);
    x.writeAttribute(QStringLiteral("processorArchitecture"), arch);
    x.writeAttribute(QStringLiteral("publicKeyToken"), QStringLiteral("31bf3856ad364e35"));
    x.writeAttribute(QStringLiteral("language"), QStringLiteral("neutral"));
    x.writeAttribute(QStringLiteral("versionScope"), QStringLiteral("nonSxS"));
}

void command(QXmlStreamWriter &x, const QString &element, const QString &field, int order, const QString &text)
{
    x.writeStartElement(element);
    x.writeAttribute(QStringLiteral("wcm:action"), QStringLiteral("add"));
    x.writeTextElement(QStringLiteral("Order"), QString::number(order));
    x.writeTextElement(field, text);
    x.writeEndElement();
}

} // namespace

quint64 windowsusb::splitFrom = kFat32Limit;

bool windowsusb::Info::needsSplit() const
{
    return installSize >= splitFrom;
}

bool windowsusb::Options::any() const
{
    return skipChecks || noOnlineAccount || !localUser.isEmpty() || skipPrivacy || sameRegion || noBitLocker;
}

windowsusb::WimInfo windowsusb::parseWimXml(const QByteArray &raw)
{
    WimInfo info;
    // WIMs keep it in UTF-16 with a byte order mark.
    QString text;
    if (raw.startsWith("\xFF\xFE"))
        text = QString::fromUtf16(reinterpret_cast<const char16_t *>(raw.constData() + 2), (raw.size() - 2) / 2);
    else
        text = QString::fromUtf8(raw);
    QXmlStreamReader xml(text);
    int depth = 0, image = 0;
    QString path; // the elements we're in, for the few we want
    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType t = xml.readNext();
        if (t == QXmlStreamReader::StartElement) {
            ++depth;
            path += QLatin1Char('/') + xml.name().toString();
            if (path == QLatin1String("/WIM/IMAGE"))
                ++image;
        } else if (t == QXmlStreamReader::EndElement) {
            --depth;
            path.truncate(path.lastIndexOf(QLatin1Char('/')));
        } else if (t == QXmlStreamReader::Characters && image == 1) {
            const QString value = xml.text().toString().trimmed();
            if (path == QLatin1String("/WIM/IMAGE/NAME"))
                info.name = value.left(100);
            else if (path == QLatin1String("/WIM/IMAGE/WINDOWS/VERSION/BUILD"))
                info.build = value.toInt();
            else if (path == QLatin1String("/WIM/IMAGE/WINDOWS/ARCH"))
                info.arch = value == QLatin1String("9") ? QStringLiteral("amd64") : value == QLatin1String("12") ? QStringLiteral("arm64")
                          : value == QLatin1String("0")                ? QStringLiteral("x86")
                                                                       : QString();
        }
        if (depth > 64)
            break;
    }
    info.images = image;
    if (xml.hasError() || image == 0)
        info.error = tr("Couldn't read what's in it.");
    return info;
}

windowsusb::WimInfo windowsusb::readWim(const QString &path)
{
    WimInfo info;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        info.error = f.errorString();
        return info;
    }
    // The header: "MSWIM\0\0\0", then (at 72) where the XML is: its size in 7 bytes and a flag
    // byte, its offset, its size again.
    const QByteArray header = f.read(208);
    if (header.size() < 208 || !header.startsWith(QByteArray("MSWIM\0\0\0", 8))) {
        info.error = tr("It isn't a WIM file.");
        return info;
    }
    const auto *h = reinterpret_cast<const uchar *>(header.constData());
    const quint64 size = le64(h + 72, 7);
    const quint64 offset = le64(h + 80, 8);
    if (size == 0 || qint64(size) > kMaxXml || offset + size > quint64(f.size()) || !f.seek(qint64(offset))) {
        info.error = tr("Couldn't read what's in it.");
        return info;
    }
    return parseWimXml(f.read(qint64(size)));
}

namespace {

// A WIM's lookup table: 50 bytes for each resource, its size as stored (7 bytes), flags, where
// it is, its size unpacked, the part it's in (2 bytes), how often it's used (4) and its SHA-1.
constexpr int kWimHeader = 208;
constexpr int kEntry = 50;
constexpr uchar kMetadata = 0x02;
constexpr uchar kCompressed = 0x04;
constexpr uchar kSolid = 0x10;
constexpr quint64 kPiece = 4 * 1024 * 1024;
constexpr quint64 kMaxTable = 512 * 1024 * 1024;

struct Resource {
    QByteArray hash;
    quint64 offset = 0;
    quint64 size = 0;
    uchar flags = 0;
};

struct Wim {
    int fd = -1;
    quint64 size = 0;
    QByteArray guid;
    int part = 0;
    int parts = 0;
    bool solid = false;
    QVector<Resource> resources; // the ones in this part

    ~Wim()
    {
        if (fd >= 0)
            blockio::close(fd);
    }
};

// Header and lookup table; empty when it isn't a WIM that can be read.
QString readTable(const QString &path, blockio::FileMode mode, Wim &wim)
{
    wim.fd = blockio::openFile(path, mode);
    if (wim.fd < 0)
        return tr("Couldn't open %1.").arg(QFileInfo(path).fileName());
    wim.size = blockio::deviceSize(wim.fd);
    blockio::Buffer buf = blockio::alignedBuffer(kPiece);
    if (!buf || !blockio::readAt(wim.fd, buf.get(), kWimHeader, 0) || !QByteArrayView(buf.get(), 8).startsWith(QByteArrayView("MSWIM\0\0\0", 8)))
        return tr("%1 isn't a WIM file.").arg(QFileInfo(path).fileName());
    const auto *h = reinterpret_cast<const uchar *>(buf.get());
    wim.solid = qFromLittleEndian<quint32>(h + 12) == 0xE00;
    wim.guid = QByteArray(buf.get() + 24, 16);
    wim.part = qFromLittleEndian<quint16>(h + 40);
    wim.parts = qFromLittleEndian<quint16>(h + 42);
    const quint64 tableSize = le64(h + 48, 7);
    const uchar tableFlags = h[55];
    const quint64 tableAt = le64(h + 56, 8);
    if ((tableFlags & kCompressed) || tableSize % kEntry || tableSize > kMaxTable || tableAt + tableSize > wim.size)
        return tr("Couldn't read what's in %1.").arg(QFileInfo(path).fileName());
    for (quint64 done = 0; done < tableSize;) {
        const quint64 n = std::min(tableSize - done, kPiece / kEntry * kEntry);
        if (!blockio::readAt(wim.fd, buf.get(), n, tableAt + done))
            return tr("Couldn't read what's in %1.").arg(QFileInfo(path).fileName());
        for (quint64 at = 0; at < n; at += kEntry) {
            const auto *e = reinterpret_cast<const uchar *>(buf.get() + at);
            Resource r;
            r.size = le64(e, 7);
            r.flags = e[7];
            r.offset = le64(e + 8, 8);
            r.hash = QByteArray(reinterpret_cast<const char *>(e + 30), 20);
            if (r.flags & kSolid)
                wim.solid = true;
            // A part's table can list other parts' resources too; only its own are in it.
            if (r.size == 0 || qFromLittleEndian<quint16>(e + 24) != wim.part)
                continue;
            if (r.offset + r.size > wim.size)
                return tr("%1 is cut short.").arg(QFileInfo(path).fileName());
            wim.resources.push_back(r);
        }
        done += n;
    }
    return {};
}

} // namespace

QString windowsusb::checkParts(const QString &original, const QStringList &parts,
                               const std::function<bool(quint64, quint64)> &progress)
{
    Wim whole;
    if (const QString error = readTable(original, blockio::FileMode::Read, whole); !error.isEmpty())
        return error;
    QHash<QByteArray, qsizetype> byHash;
    for (qsizetype i = 0; i < whole.resources.size(); ++i)
        byHash.insert(whole.resources[i].hash, i);

    std::vector<std::unique_ptr<Wim>> pieces;
    quint64 total = 0;
    QSet<int> numbers;
    for (const QString &path : parts) {
        auto wim = std::make_unique<Wim>();
        if (const QString error = readTable(path, blockio::FileMode::ReadBack, *wim); !error.isEmpty())
            return error;
        if (wim->parts != int(parts.size()) || wim->part < 1 || wim->part > wim->parts || numbers.contains(wim->part)
            || (!pieces.empty() && wim->guid != pieces.front()->guid))
            return tr("install.wim's parts don't belong together.");
        numbers.insert(wim->part);
        // Read front to back, as the stick has them.
        std::sort(wim->resources.begin(), wim->resources.end(), [](const Resource &a, const Resource &b) { return a.offset < b.offset; });
        for (const Resource &r : wim->resources)
            total += whole.solid ? 0 : r.size;
        if (whole.solid)
            total += wim->size;
        pieces.push_back(std::move(wim));
    }

    blockio::Buffer mine = blockio::alignedBuffer(2 * kPiece);
    blockio::Buffer theirs = blockio::alignedBuffer(kPiece);
    if (!mine || !theirs)
        return tr("Not enough memory to check install.wim's parts.");
    quint64 done = 0;
    if (whole.solid) {
        // Packed together (an ESD's way), the resources can't be told apart: every byte of the
        // parts read back is the check then.
        for (const auto &wim : pieces) {
            for (quint64 at = 0; at < wim->size;) {
                const quint64 n = std::min(kPiece, wim->size - at);
                if (!blockio::readAt(wim->fd, mine.get(), (n + blockio::kAlign - 1) / blockio::kAlign * blockio::kAlign, at)
                    && !blockio::readAt(wim->fd, mine.get(), n, at))
                    return tr("Couldn't read install.wim's parts back from the stick. It may be failing; try another one.");
                at += n;
                done += n;
                if (progress && !progress(done, total))
                    return tr("Stopped.");
            }
        }
        return {};
    }

    QSet<QByteArray> seen;
    for (const auto &wim : pieces) {
        // What's been read of the part: [from, to) in `mine`, in pieces at the sector edge.
        quint64 from = 0, to = 0;
        for (const Resource &r : wim->resources) {
            const qsizetype i = byHash.value(r.hash, -1);
            if (i < 0 || whole.resources[i].size != r.size || (whole.resources[i].flags & (kCompressed | kMetadata)) != (r.flags & (kCompressed | kMetadata)))
                return tr("install.wim's parts don't match it.");
            seen.insert(r.hash);
            for (quint64 at = 0; at < r.size;) {
                const quint64 n = std::min(kPiece, r.size - at);
                const quint64 offset = r.offset + at;
                if (offset < from || offset + n > to) {
                    from = offset / blockio::kAlign * blockio::kAlign;
                    const quint64 want = std::min(2 * kPiece, (wim->size - from + blockio::kAlign - 1) / blockio::kAlign * blockio::kAlign);
                    const qint64 got = blockio::readSome(wim->fd, mine.get(), want, from);
                    if (got < 0 || from + quint64(got) < offset + n)
                        return tr("Couldn't read install.wim's parts back from the stick. It may be failing; try another one.");
                    to = from + quint64(got);
                }
                if (!blockio::readAt(whole.fd, theirs.get(), n, whole.resources[i].offset + at))
                    return tr("Couldn't read %1.").arg(QFileInfo(original).fileName());
                if (memcmp(mine.get() + (offset - from), theirs.get(), n) != 0)
                    return tr("The stick gave back something different from what was written to install.wim's parts. "
                              "It may be failing; try another one.");
                at += n;
                done += n;
                if (progress && !progress(done, total))
                    return tr("Stopped.");
            }
        }
    }
    for (const Resource &r : whole.resources) {
        if (!seen.contains(r.hash))
            return tr("Some of install.wim is missing from its parts.");
    }
    return {};
}

windowsusb::Info windowsusb::inspect(const QString &root)
{
    Info info;
    QDir dir(root);
    // FAT and UDF don't care about case, but the folder listing does.
    auto find = [&dir](const QString &path) -> QString {
        QString found = QStringLiteral(".");
        for (const QString &part : path.split(QLatin1Char('/'))) {
            const QStringList names = QDir(dir.filePath(found)).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
            QString match;
            for (const QString &n : names) {
                if (n.compare(part, Qt::CaseInsensitive) == 0)
                    match = n;
            }
            if (match.isEmpty())
                return {};
            found = found == QLatin1String(".") ? match : found + QLatin1Char('/') + match;
        }
        return found;
    };
    for (const QString &candidate : {QStringLiteral("sources/install.wim"), QStringLiteral("sources/install.esd")}) {
        const QString found = find(candidate);
        if (!found.isEmpty()) {
            info.install = found;
            info.installSize = quint64(QFileInfo(dir.filePath(found)).size());
            break;
        }
    }
    const bool boots = !find(QStringLiteral("efi/boot/bootx64.efi")).isEmpty() || !find(QStringLiteral("efi/boot/bootaa64.efi")).isEmpty();
    info.windows = !info.install.isEmpty() && boots && !find(QStringLiteral("bootmgr.efi")).isEmpty();
    if (info.windows)
        info.wim = readWim(dir.filePath(info.install));
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        info.bytes += quint64(it.fileInfo().size());
    }
    return info;
}

QString windowsusb::userName(const QString &typed, QString *error)
{
    QString name;
    for (const QChar c : typed.trimmed()) {
        if (c.unicode() < 0x20 || QStringLiteral("\"/\\[]:;|=,+*?<>@%&.").contains(c))
            name += QLatin1Char('_');
        else
            name += c;
    }
    name.truncate(20); // Windows' limit
    static const QSet<QString> reserved = {QStringLiteral("administrator"), QStringLiteral("guest"), QStringLiteral("defaultaccount"),
                                           QStringLiteral("wdagutilityaccount"), QStringLiteral("helpassistant"), QStringLiteral("krbtgt"),
                                           QStringLiteral("local"), QStringLiteral("none"), QStringLiteral("system")};
    if (name.isEmpty() || name.count(QLatin1Char('_')) == name.size()) {
        if (error)
            *error = tr("Type a name for the account.");
        return {};
    }
    if (reserved.contains(name.toLower())) {
        if (error)
            *error = tr("Windows keeps the name \"%1\" for itself; pick another.").arg(name);
        return {};
    }
    return name;
}

QString windowsusb::inputLocaleFor(const QString &xkbLayout)
{
    // The usual ones; anything else keeps what goes with the language.
    static const QHash<QString, QString> layouts = {
        {QStringLiteral("us"), QStringLiteral("0409:00000409")}, {QStringLiteral("gb"), QStringLiteral("0809:00000809")},
        {QStringLiteral("de"), QStringLiteral("0407:00000407")}, {QStringLiteral("at"), QStringLiteral("0c07:00000407")},
        {QStringLiteral("ch"), QStringLiteral("0807:00000807")}, {QStringLiteral("fr"), QStringLiteral("040c:0000040c")},
        {QStringLiteral("be"), QStringLiteral("080c:0000080c")}, {QStringLiteral("es"), QStringLiteral("0c0a:0000040a")},
        {QStringLiteral("latam"), QStringLiteral("080a:0000080a")}, {QStringLiteral("it"), QStringLiteral("0410:00000410")},
        {QStringLiteral("pt"), QStringLiteral("0816:00000816")}, {QStringLiteral("br"), QStringLiteral("0416:00000416")},
        {QStringLiteral("nl"), QStringLiteral("0413:00000413")}, {QStringLiteral("se"), QStringLiteral("041d:0000041d")},
        {QStringLiteral("no"), QStringLiteral("0414:00000414")}, {QStringLiteral("dk"), QStringLiteral("0406:00000406")},
        {QStringLiteral("fi"), QStringLiteral("040b:0000040b")}, {QStringLiteral("pl"), QStringLiteral("0415:00000415")},
        {QStringLiteral("cz"), QStringLiteral("0405:00000405")}, {QStringLiteral("hu"), QStringLiteral("040e:0000040e")},
        {QStringLiteral("ru"), QStringLiteral("0419:00000419")}, {QStringLiteral("ua"), QStringLiteral("0422:00000422")},
        {QStringLiteral("tr"), QStringLiteral("041f:0000041f")}, {QStringLiteral("gr"), QStringLiteral("0408:00000408")},
        {QStringLiteral("ca"), QStringLiteral("0c0c:00001009")}, {QStringLiteral("jp"), QStringLiteral("0411:00000411")},
        {QStringLiteral("kr"), QStringLiteral("0412:00000412")},
    };
    return layouts.value(xkbLayout.section(QLatin1Char(','), 0, 0).trimmed().toLower());
}

QString windowsusb::windowsTimeZone(const QByteArray &ianaId, const QByteArray &tzdataLinks)
{
    QByteArray id = ianaId;
    // Old names like US/Central are links to the real one (America/Chicago), which is the only
    // one Qt can translate. tzdata.zi lists them as "L <real name> <old name>".
    for (int hops = 0; hops < 3; ++hops) {
        const QByteArray windows = QTimeZone::ianaIdToWindowsId(id);
        if (!windows.isEmpty())
            return QString::fromLatin1(windows);
        QByteArray target;
        for (const QByteArray &line : tzdataLinks.split('\n')) {
            const QList<QByteArray> parts = line.simplified().split(' ');
            if (parts.size() == 3 && parts[0] == "L" && parts[2] == id)
                target = parts[1];
        }
        if (target.isEmpty())
            break;
        id = target;
    }
    return {};
}

#ifdef Q_OS_WIN
void windowsusb::fillRegionFromThisPc(Options &options)
{
    // Windows names its own time zone and keyboard the way the answer file wants them.
    options.userLocale = QLocale::system().name().replace(QLatin1Char('_'), QLatin1Char('-'));
    DYNAMIC_TIME_ZONE_INFORMATION zone = {};
    if (GetDynamicTimeZoneInformation(&zone) != TIME_ZONE_ID_INVALID)
        options.timeZone = QString::fromWCharArray(zone.TimeZoneKeyName);
    // The keyboard as "0809:00000809": its language, then the layout.
    wchar_t layout[KL_NAMELENGTH] = {};
    if (GetKeyboardLayoutNameW(layout)) {
        const uint language = uint(quintptr(GetKeyboardLayout(0)) & 0xFFFF);
        options.inputLocale = QStringLiteral("%1:%2").arg(language, 4, 16, QLatin1Char('0')).arg(QString::fromWCharArray(layout)).toLower();
    }
}
#else
void windowsusb::fillRegionFromThisPc(Options &options)
{
    options.userLocale = QLocale::system().name().replace(QLatin1Char('_'), QLatin1Char('-'));
    QFile links(QStringLiteral("/usr/share/zoneinfo/tzdata.zi"));
    options.timeZone = windowsTimeZone(QTimeZone::systemTimeZoneId(), links.open(QIODevice::ReadOnly) ? links.readAll() : QByteArray());
    // The keyboard: KDE's own setting first, then the system's.
    QString layout = QSettings(QDir::homePath() + QStringLiteral("/.config/kxkbrc"), QSettings::IniFormat)
                         .value(QStringLiteral("Layout/LayoutList"))
                         .toString();
    if (layout.isEmpty() && !QStandardPaths::findExecutable(QStringLiteral("localectl")).isEmpty()) {
        QProcess p;
        p.start(QStringLiteral("localectl"), {QStringLiteral("status")});
        p.waitForFinished(5000);
        for (const QString &line : QString::fromLocal8Bit(p.readAllStandardOutput()).split(QLatin1Char('\n'))) {
            if (line.trimmed().startsWith(QLatin1String("X11 Layout:")))
                layout = line.section(QLatin1Char(':'), 1).trimmed();
        }
    }
    options.inputLocale = inputLocaleFor(layout);
}
#endif

QByteArray windowsusb::unattendXml(const Options &o, const QString &archIn)
{
    if (!o.any())
        return {};
    const QString arch = archIn.isEmpty() ? QStringLiteral("amd64") : archIn;
    QByteArray out;
    QXmlStreamWriter x(&out);
    x.setAutoFormatting(true);
    x.setAutoFormattingIndent(2);
    x.writeStartDocument();
    x.writeComment(QStringLiteral(" Made by DiskAnvil's Make a Windows USB. Windows Setup reads it from the top of the stick. "));
    x.writeStartElement(QStringLiteral("unattend"));
    x.writeDefaultNamespace(QStringLiteral("urn:schemas-microsoft-com:unattend"));
    x.writeNamespace(QStringLiteral("http://schemas.microsoft.com/WMIConfig/2002/State"), QStringLiteral("wcm"));

    if (o.skipChecks) {
        // Setup reads these before it checks the PC.
        x.writeStartElement(QStringLiteral("settings"));
        x.writeAttribute(QStringLiteral("pass"), QStringLiteral("windowsPE"));
        component(x, QStringLiteral("Microsoft-Windows-Setup"), arch);
        x.writeStartElement(QStringLiteral("RunSynchronous"));
        int order = 1;
        for (const char *check : {"BypassTPMCheck", "BypassSecureBootCheck", "BypassRAMCheck"}) {
            command(x, QStringLiteral("RunSynchronousCommand"), QStringLiteral("Path"), order++,
                    QStringLiteral("reg add HKLM\\SYSTEM\\Setup\\LabConfig /v %1 /t REG_DWORD /d 1 /f").arg(QLatin1String(check)));
        }
        x.writeEndElement(); // RunSynchronous
        x.writeEndElement(); // component
        x.writeEndElement(); // settings
    }

    if (o.noOnlineAccount) {
        x.writeStartElement(QStringLiteral("settings"));
        x.writeAttribute(QStringLiteral("pass"), QStringLiteral("specialize"));
        component(x, QStringLiteral("Microsoft-Windows-Deployment"), arch);
        x.writeStartElement(QStringLiteral("RunSynchronous"));
        command(x, QStringLiteral("RunSynchronousCommand"), QStringLiteral("Path"), 1,
                QStringLiteral("reg add HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE /v BypassNRO /t REG_DWORD /d 1 /f"));
        x.writeEndElement();
        x.writeEndElement();
        x.writeEndElement();
    }

    const bool oobe = o.skipPrivacy || !o.localUser.isEmpty() || o.sameRegion || o.noBitLocker;
    if (oobe) {
        x.writeStartElement(QStringLiteral("settings"));
        x.writeAttribute(QStringLiteral("pass"), QStringLiteral("oobeSystem"));
        if (o.sameRegion && (!o.userLocale.isEmpty() || !o.inputLocale.isEmpty())) {
            // The display language stays the ISO's own: one it doesn't have would stop Setup.
            component(x, QStringLiteral("Microsoft-Windows-International-Core"), arch);
            if (!o.inputLocale.isEmpty())
                x.writeTextElement(QStringLiteral("InputLocale"), o.inputLocale);
            if (!o.userLocale.isEmpty()) {
                x.writeTextElement(QStringLiteral("SystemLocale"), o.userLocale);
                x.writeTextElement(QStringLiteral("UserLocale"), o.userLocale);
            }
            x.writeEndElement();
        }
        if (o.skipPrivacy || !o.localUser.isEmpty() || (o.sameRegion && !o.timeZone.isEmpty())) {
            component(x, QStringLiteral("Microsoft-Windows-Shell-Setup"), arch);
            if (o.skipPrivacy || !o.localUser.isEmpty()) {
                x.writeStartElement(QStringLiteral("OOBE"));
                if (o.skipPrivacy) {
                    x.writeTextElement(QStringLiteral("HideEULAPage"), QStringLiteral("true"));
                    x.writeTextElement(QStringLiteral("ProtectYourPC"), QStringLiteral("3"));
                }
                if (!o.localUser.isEmpty())
                    x.writeTextElement(QStringLiteral("HideOnlineAccountScreens"), QStringLiteral("true"));
                x.writeEndElement();
            }
            if (!o.localUser.isEmpty()) {
                x.writeStartElement(QStringLiteral("UserAccounts"));
                x.writeStartElement(QStringLiteral("LocalAccounts"));
                x.writeStartElement(QStringLiteral("LocalAccount"));
                x.writeAttribute(QStringLiteral("wcm:action"), QStringLiteral("add"));
                x.writeTextElement(QStringLiteral("Name"), o.localUser);
                x.writeTextElement(QStringLiteral("DisplayName"), o.localUser);
                x.writeTextElement(QStringLiteral("Group"), QStringLiteral("Administrators"));
                x.writeStartElement(QStringLiteral("Password"));
                x.writeTextElement(QStringLiteral("Value"), QString());
                x.writeTextElement(QStringLiteral("PlainText"), QStringLiteral("true"));
                x.writeEndElement(); // Password
                x.writeEndElement(); // LocalAccount
                x.writeEndElement(); // LocalAccounts
                x.writeEndElement(); // UserAccounts
                // No password yet: Windows asks for one at the first sign-in.
                x.writeStartElement(QStringLiteral("FirstLogonCommands"));
                command(x, QStringLiteral("SynchronousCommand"), QStringLiteral("CommandLine"), 1,
                        QStringLiteral("net user \"%1\" /logonpasswordchg:yes").arg(o.localUser));
                command(x, QStringLiteral("SynchronousCommand"), QStringLiteral("CommandLine"), 2, QStringLiteral("net accounts /maxpwage:unlimited"));
                x.writeEndElement();
            }
            if (o.sameRegion && !o.timeZone.isEmpty())
                x.writeTextElement(QStringLiteral("TimeZone"), o.timeZone);
            x.writeEndElement(); // component
        }
        if (o.noBitLocker) {
            component(x, QStringLiteral("Microsoft-Windows-SecureStartup-FilterDriver"), arch);
            x.writeTextElement(QStringLiteral("PreventDeviceEncryption"), QStringLiteral("true"));
            x.writeEndElement();
            component(x, QStringLiteral("Microsoft-Windows-EnhancedStorage-Adm"), arch);
            x.writeTextElement(QStringLiteral("TCGSecurityActivationDisabled"), QStringLiteral("1"));
            x.writeEndElement();
        }
        x.writeEndElement(); // settings
    }
    x.writeEndElement(); // unattend
    x.writeEndDocument();
    return out;
}
