// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "partrecover.h"

#include "blockio.h"
#include "format.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace {

constexpr int kKept = 10;

bool isGuid(const QString &s)
{
    static const QRegularExpression form(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    return form.match(s).hasMatch();
}

bool isMbrType(const QString &s)
{
    static const QRegularExpression form(QStringLiteral("^0x[0-9a-f]{2}$"));
    return form.match(s).hasMatch();
}

QString fileFor(const QString &driveKey)
{
    // The key is already plain (letters, digits, - _ .); never a path of its own.
    QString name = driveKey;
    if (name.isEmpty() || name.startsWith(QLatin1Char('.')) || name.contains(QLatin1Char('/')))
        name.prepend(QStringLiteral("drive-"));
    name.replace(QLatin1Char('/'), QLatin1Char('_'));
    return recover::folder() + QLatin1Char('/') + name + QStringLiteral(".json");
}

} // namespace

QString recover::Layout::fingerprint() const
{
    QVector<Part> sorted = parts;
    std::sort(sorted.begin(), sorted.end(), [](const Part &a, const Part &b) { return a.start < b.start; });
    QStringList out = {table, diskId};
    for (const Part &p : std::as_const(sorted)) {
        out << QStringLiteral("%1:%2:%3:%4:%5:%6:%7:%8:%9")
                   .arg(p.number)
                   .arg(p.start)
                   .arg(p.size)
                   .arg(p.type, p.guid, p.name)
                   .arg(p.flags)
                   .arg(int(p.container))
                   .arg(int(p.logical));
    }
    return out.join(QLatin1Char('|'));
}

QString recover::diskId(const QString &device)
{
    QFile dev(QStringLiteral("/sys/class/block/%1/dev").arg(QFileInfo(device).fileName()));
    if (!dev.open(QIODevice::ReadOnly))
        return {};
    const QString majorMinor = QString::fromLatin1(dev.readAll()).trimmed();
    QFile data(QStringLiteral("/run/udev/data/b") + majorMinor);
    if (!data.open(QIODevice::ReadOnly))
        return {};
    for (const QByteArray &line : data.readAll().split('\n')) {
        if (line.startsWith("E:ID_PART_TABLE_UUID=")) {
            const QString id = QString::fromLatin1(line.mid(21)).trimmed().toLower();
            static const QRegularExpression signature(QStringLiteral("^[0-9a-f]{8}$"));
            return isGuid(id) || signature.match(id).hasMatch() ? id : QString();
        }
    }
    return {};
}

recover::Layout recover::fromDisk(const Disk &disk)
{
    Layout l;
    l.saved = QDateTime::currentDateTimeUtc();
    l.table = disk.tableType;
    l.diskSize = disk.size;
    l.diskId = diskId(disk.device);
    for (const Volume &v : disk.volumes) {
        if (v.number <= 0)
            continue; // a file system on the whole drive, no table
        Part p;
        p.number = v.number;
        p.start = v.offset;
        p.size = v.size;
        p.type = v.partType.toLower();
        p.guid = disk.tableType == QLatin1String("gpt") ? v.partUuid.toLower() : QString();
        p.name = v.partName;
        p.flags = v.partFlags;
        p.container = v.isContainer;
        p.logical = v.isContained;
        p.fsType = v.effectiveFsType();
        p.label = v.label;
        l.parts.push_back(p);
    }
    return l;
}

recover::Layout recover::fromReport(const gpt::Report &report, bool backup)
{
    const gpt::Header &h = backup ? report.backup : report.primary;
    const quint64 sector = quint64(std::max(report.sectorSize, 512));
    Layout l;
    l.saved = QDateTime::currentDateTimeUtc();
    l.table = QStringLiteral("gpt");
    l.diskSize = (report.lastLba + 1) * sector;
    l.diskId = h.diskGuid;
    for (const gpt::Entry &e : h.entries) {
        Part p;
        p.number = e.index;
        p.start = e.firstLba * sector;
        p.size = e.lastLba >= e.firstLba ? (e.lastLba - e.firstLba + 1) * sector : 0;
        p.type = e.type;
        p.guid = e.guid;
        p.name = e.name;
        p.flags = e.attributes;
        l.parts.push_back(p);
    }
    return l;
}

QJsonObject recover::toJson(const Layout &l)
{
    QJsonArray parts;
    for (const Part &p : l.parts) {
        QJsonObject o{{QStringLiteral("number"), p.number},
                      {QStringLiteral("start"), QString::number(p.start)}, // as text: JSON numbers lose precision past 2^53
                      {QStringLiteral("size"), QString::number(p.size)},
                      {QStringLiteral("type"), p.type},
                      {QStringLiteral("flags"), QString::number(p.flags)}};
        if (!p.guid.isEmpty())
            o.insert(QStringLiteral("guid"), p.guid);
        if (!p.name.isEmpty())
            o.insert(QStringLiteral("name"), p.name);
        if (p.container)
            o.insert(QStringLiteral("container"), true);
        if (p.logical)
            o.insert(QStringLiteral("logical"), true);
        if (!p.fsType.isEmpty())
            o.insert(QStringLiteral("fs"), p.fsType);
        if (!p.label.isEmpty())
            o.insert(QStringLiteral("label"), p.label);
        parts.append(o);
    }
    return {{QStringLiteral("saved"), l.saved.toString(Qt::ISODate)},
            {QStringLiteral("table"), l.table},
            {QStringLiteral("disk_size"), QString::number(l.diskSize)},
            {QStringLiteral("disk_id"), l.diskId},
            {QStringLiteral("partitions"), parts}};
}

bool recover::fromJson(const QJsonObject &o, Layout *l)
{
    Layout out;
    auto number = [](const QJsonValue &v, quint64 *n) {
        bool ok = false;
        *n = v.toString().toULongLong(&ok);
        return ok;
    };
    out.saved = QDateTime::fromString(o.value(QStringLiteral("saved")).toString(), Qt::ISODate);
    out.table = o.value(QStringLiteral("table")).toString();
    out.diskId = o.value(QStringLiteral("disk_id")).toString();
    const QJsonArray parts = o.value(QStringLiteral("partitions")).toArray();
    if (!out.saved.isValid() || (out.table != QLatin1String("gpt") && out.table != QLatin1String("dos"))
        || !number(o.value(QStringLiteral("disk_size")), &out.diskSize) || parts.isEmpty() || parts.size() > 128 || out.diskId.size() > 36
        || hasHiddenCharacters(out.diskId))
        return false;
    for (const QJsonValue &value : parts) {
        const QJsonObject po = value.toObject();
        Part p;
        p.number = po.value(QStringLiteral("number")).toInt(-1);
        p.type = po.value(QStringLiteral("type")).toString();
        p.guid = po.value(QStringLiteral("guid")).toString();
        p.name = cleanName(po.value(QStringLiteral("name")).toString()).left(36);
        p.container = po.value(QStringLiteral("container")).toBool();
        p.logical = po.value(QStringLiteral("logical")).toBool();
        p.fsType = cleanName(po.value(QStringLiteral("fs")).toString()).left(32);
        p.label = cleanName(po.value(QStringLiteral("label")).toString()).left(64);
        const bool typeOk = out.table == QLatin1String("gpt") ? isGuid(p.type) : isMbrType(p.type);
        if (p.number < 1 || p.number > 128 || !number(po.value(QStringLiteral("start")), &p.start) || !number(po.value(QStringLiteral("size")), &p.size)
            || !number(po.value(QStringLiteral("flags")), &p.flags) || !typeOk || (!p.guid.isEmpty() && !isGuid(p.guid)))
            return false;
        out.parts.push_back(p);
    }
    *l = out;
    return true;
}

QString recover::folder()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/diskforge/layouts");
}

QVector<recover::Layout> recover::saved(const QString &driveKey)
{
    QVector<Layout> out;
    QFile f(fileFor(driveKey));
    if (!f.open(QIODevice::ReadOnly) || f.size() > 4 * 1024 * 1024)
        return out;
    const QJsonArray list = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("layouts")).toArray();
    for (const QJsonValue &v : list) {
        Layout l;
        if (out.size() < kKept && fromJson(v.toObject(), &l))
            out.push_back(l);
    }
    return out;
}

bool recover::remember(const QString &driveKey, const Layout &layout)
{
    if (layout.parts.isEmpty() || driveKey.isEmpty())
        return false;
    static QHash<QString, QString> newest; // what's on file, so a refresh doesn't read it every time
    const QString print = layout.fingerprint();
    const QString file = fileFor(driveKey);
    if (newest.value(file) == print)
        return false;
    QVector<Layout> list = saved(driveKey);
    if (!list.isEmpty() && list.first().fingerprint() == print) {
        newest.insert(file, print);
        return false;
    }
    list.prepend(layout);
    QJsonArray out;
    for (int i = 0; i < std::min<int>(int(list.size()), kKept); ++i)
        out.append(toJson(list[i]));
    QDir().mkpath(folder());
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(QJsonObject{{QStringLiteral("layouts"), out}}).toJson());
    if (!f.commit())
        return false;
    newest.insert(file, print);
    return true;
}

QString recover::problem(const Layout &l, quint64 diskSize, int sectorSize)
{
    const quint64 sector = quint64(std::max(sectorSize, 512));
    if (l.table != QLatin1String("gpt") && l.table != QLatin1String("dos"))
        return QObject::tr("Only GPT and MBR tables can be written back.");
    if (l.parts.isEmpty())
        return QObject::tr("There are no partitions in it.");
    const quint64 listSectors = (128 * 128 + sector - 1) / sector;
    const quint64 lastLba = diskSize / sector - 1;
    const quint64 firstUsable = 2 + listSectors, lastUsable = lastLba > listSectors + 1 ? lastLba - listSectors - 1 : 0;
    int primary = 0;
    for (const Part &p : l.parts) {
        if (p.size == 0 || p.start % sector || p.size % sector)
            return QObject::tr("Partition %1 doesn't start or end on a sector of this drive.").arg(p.number);
        if (p.start + p.size > diskSize || p.start + p.size < p.start)
            return QObject::tr("Partition %1 goes past the end of this drive: the drive is smaller than the one this was saved from.").arg(p.number);
        if (l.table == QLatin1String("gpt") && (p.start / sector < firstUsable || (p.start + p.size) / sector - 1 > lastUsable))
            return QObject::tr("Partition %1 is where the table itself has to go.").arg(p.number);
        primary += l.table == QLatin1String("dos") && !p.logical ? 1 : 0;
        for (const Part &q : l.parts) {
            if (&q == &p || (p.container && q.logical) || (q.container && p.logical))
                continue;
            if (p.start < q.start + q.size && q.start < p.start + p.size)
                return QObject::tr("Partitions %1 and %2 overlap.").arg(p.number).arg(q.number);
        }
    }
    if (primary > 4)
        return QObject::tr("An MBR holds four primary partitions.");
    return {};
}

gpt::Result recover::write(int fd, const Layout &l, int givenSectorSize)
{
    const int sectorSize = givenSectorSize > 0 ? givenSectorSize : blockio::logicalSize(fd);
    const quint64 sector = quint64(sectorSize);
    const QString why = problem(l, blockio::deviceSize(fd), sectorSize);
    if (!why.isEmpty()) {
        gpt::Result r;
        r.error = why;
        return r;
    }
    if (l.table == QLatin1String("gpt")) {
        QVector<gpt::Entry> entries;
        for (const Part &p : l.parts) {
            gpt::Entry e;
            e.index = p.number;
            e.type = p.type;
            e.guid = p.guid;
            e.firstLba = p.start / sector;
            e.lastLba = (p.start + p.size) / sector - 1;
            e.attributes = p.flags;
            e.name = p.name;
            entries.push_back(e);
        }
        return gpt::writeTable(fd, sectorSize, l.diskId, entries);
    }
    QVector<gpt::MbrPart> parts;
    for (const Part &p : l.parts) {
        gpt::MbrPart m;
        m.slot = p.logical ? 0 : p.number;
        m.type = quint8(p.type.mid(2).toUInt(nullptr, 16));
        m.bootable = p.flags & 0x80;
        m.logical = p.logical;
        m.firstLba = p.start / sector;
        m.sectors = p.size / sector;
        parts.push_back(m);
    }
    return gpt::writeMbr(fd, sectorSize, quint32(l.diskId.left(8).toUInt(nullptr, 16)), parts);
}

Disk recover::preview(const Disk &disk, const Layout &l)
{
    Disk d = disk;
    d.tableType = l.table;
    d.volumes.clear();
    const bool digitEnd = !disk.device.isEmpty() && disk.device.back().isDigit(); // nvme0n1 -> nvme0n1p1
    for (const Part &p : l.parts) {
        Volume v;
        v.objectPath = QStringLiteral("/preview/%1").arg(p.number);
        v.device = disk.device + (digitEnd ? QStringLiteral("p") : QString()) + QString::number(p.number);
        v.number = p.number;
        v.offset = p.start;
        v.size = p.size;
        v.partType = p.type;
        v.partName = p.name;
        v.label = p.label;
        v.fsType = p.fsType;
        v.isContainer = p.container;
        v.isContained = p.logical;
        d.volumes.push_back(v);
    }
    std::sort(d.volumes.begin(), d.volumes.end(), [](const Volume &a, const Volume &b) { return a.offset < b.offset; });
    return d;
}
