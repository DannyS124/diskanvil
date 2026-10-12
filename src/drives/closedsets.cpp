// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "closedsets.h"

#include <QCoreApplication>
#include <QMap>

namespace closedsets {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("closedsets", text);
}

} // namespace

bool isDynamicType(const QString &type)
{
    const QString t = type.toLower();
    return t == QLatin1String("5808c8aa-7e8f-42e0-85d2-e1e90434cfb3")    // LDM metadata
        || t == QLatin1String("af9b60a0-1431-4f62-bc68-3311714a69ad")    // LDM data
        || t == QLatin1String("0x42");
}

QVector<Set> find(const QVector<Disk> &disks, bool dynamicOpen)
{
    QVector<Set> out;
    QMap<QString, int> arrays; // UUID -> index in out
    Set dynamic;
    dynamic.kind = Set::Kind::Dynamic;
    for (const Disk &d : disks) {
        if (d.isRaid) // images of a NAS's drives count too
            continue;
        bool dynamicDisk = false;
        for (const Volume &v : d.volumes) {
            // A RAID member whose array isn't running: UDisks knows the array ("md" with no
            // device of its own yet) or doesn't at all.
            if (v.fsType == QLatin1String("linux_raid_member") && (v.raidMemberOf.isEmpty() || v.raidMemberOf == QLatin1String("md"))) {
                const QString id = v.uuid.isEmpty() ? v.device : v.uuid;
                if (!arrays.contains(id)) {
                    arrays.insert(id, int(out.size()));
                    Set s;
                    s.name = v.label;
                    s.id = v.uuid;
                    out << s;
                }
                out[arrays.value(id)].members << v.device;
            }
            dynamicDisk = dynamicDisk || isDynamicType(v.partType);
        }
        if (dynamicDisk)
            dynamic.members << d.device;
    }
    if (!dynamic.members.isEmpty() && !dynamicOpen)
        out << dynamic;
    return out;
}

QString describe(const Set &set)
{
    const QString on = set.members.join(QStringLiteral(", "));
    if (set.kind == Set::Kind::Dynamic)
        return tr("Windows dynamic disks (%1)").arg(on);
    return set.name.isEmpty() ? tr("drives of a RAID array (%1)").arg(on) : tr("drives of the RAID array %1 (%2)").arg(set.name, on);
}

QString openScript(const QVector<Set> &sets)
{
    bool raid = false, dynamic = false;
    for (const Set &s : sets) {
        raid = raid || s.kind == Set::Kind::Raid;
        dynamic = dynamic || s.kind == Set::Kind::Dynamic;
    }
    QStringList script;
    // Read-only: nothing is written to the drives, not even a resync of an array left unclean.
    if (raid)
        script << QStringLiteral("mdadm --assemble --scan --readonly --run");
    // A NAS puts LVM on its arrays (Synology's SHR): its volumes are switched on, which reads
    // them and writes nothing.
    if (raid)
        script << QStringLiteral("if command -v vgchange >/dev/null; then vgchange -ay; fi");
    if (dynamic)
        script << QStringLiteral("ldmtool create all");
    // Whatever opened, the rest is said by what shows up.
    return script.join(QStringLiteral("; ")) + QStringLiteral("; udevadm settle; true");
}

QStringList missing(const QVector<Set> &sets, const std::function<bool(const QString &)> &has)
{
    QStringList out;
    for (const Set &s : sets) {
        const QString program = s.kind == Set::Kind::Raid ? QStringLiteral("mdadm") : QStringLiteral("ldmtool");
        if (!has(program) && !out.contains(program))
            out << program;
    }
    return out;
}

} // namespace closedsets
