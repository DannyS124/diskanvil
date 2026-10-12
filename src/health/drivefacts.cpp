// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "drivefacts.h"

#include "format.h"

#include <QCoreApplication>
#include <QFile>
#include <QRegularExpression>

namespace drivefacts {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("drivefacts", text);
}

QString readLine(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.readAll()).trimmed() : QString();
}

// Firmware versions of the same kind compare as numbers in their middle: "3B2QGXA7" < "5B2QGXA7".
bool olderThan(const QString &have, const QString &fixed)
{
    return !have.isEmpty() && have.size() == fixed.size() && have.at(0).isDigit() && fixed.at(0).isDigit() && have.at(0) < fixed.at(0)
        && have.mid(1) == fixed.mid(1);
}

} // namespace

EmmcLife emmcLife(const QString &device, const QString &sysfs)
{
    EmmcLife life;
    const QString name = device.section(QLatin1Char('/'), -1);
    if (!name.startsWith(QLatin1String("mmcblk")))
        return life;
    // life_time: "0x01 0x02", estimates A and B, each 1 for 0-10% used up to 0x0A for 90-100%,
    // 0x0B for past its life.
    const QStringList estimates = readLine(sysfs + QLatin1Char('/') + name + QStringLiteral("/device/life_time")).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    int worst = 0;
    for (const QString &e : estimates) {
        bool ok = false;
        const int v = e.toInt(&ok, 16);
        if (ok && v >= 1 && v <= 11)
            worst = std::max(worst, v);
    }
    bool ok = false;
    const int eol = readLine(sysfs + QLatin1Char('/') + name + QStringLiteral("/device/pre_eol_info")).toInt(&ok, 16);
    if (worst == 0 && !ok)
        return life;
    life.known = true;
    if (worst > 0) {
        life.usedFrom = (worst - 1) * 10;
        life.usedTo = std::min(worst * 10, 100);
    }
    life.reserve = ok ? eol : 0;
    return life;
}

QString describe(const EmmcLife &life)
{
    if (!life.known)
        return {};
    QStringList parts;
    if (life.usedFrom >= 100)
        parts << tr("past the writing it was made for");
    else if (life.usedFrom >= 0)
        parts << tr("%1 to %2% of its life used").arg(life.usedFrom).arg(life.usedTo);
    if (life.reserve == 2)
        parts << tr("its spare blocks are 80% used: back it up");
    else if (life.reserve == 3)
        parts << tr("its spare blocks are 90% used: replace it soon");
    else if (life.reserve == 1)
        parts << tr("its spare blocks are fine");
    return parts.join(QStringLiteral("; "));
}

QStringList misaligned(const Disk &disk)
{
    QStringList out;
    for (const Volume &v : disk.volumes) {
        if (v.offset % 4096 == 0 || v.isContainer)
            continue;
        out << tr("Partition %1 starts %2 in, not on a 4 KiB boundary").arg(v.number).arg(formatSize(v.offset));
    }
    return out;
}

QVector<Fact> aboutModel(const QString &modelIn, const QString &firmware)
{
    QVector<Fact> out;
    const QString model = modelIn.simplified().toUpper();
    // SMR: shingled recording, which writes tracks overlapping like roof tiles: fine for
    // everyday use, slow for long writes, and not for RAID or NAS (it can drop out of an array).
    // These are from the makers' own lists (2020).
    static const QRegularExpression smr(QStringLiteral("\\b(WD[2-6]0EFAX|WD[12]0SPZX|WD20EZAZ|WD60EZAZ|ST2000DM008|ST4000DM004|ST8000DM004|"
                                                       "ST1000LM048|ST2000LM015|ST3000LM024|ST4000LM024|ST5000LM000|ST5000DM000|ST8000AS0002|"
                                                       "HDWD240|HDWD260|DT02ABA400|DT02ABA600)"));
    if (smr.match(model).hasMatch())
        out << Fact{Fact::Kind::Smr, tr("This model records with SMR (overlapping tracks): fine for everyday files, but slow for long "
                                        "writes, and not made for RAID or a NAS, where it can drop out of the array.")};
    // Firmware with a known bug, fixed by the maker's update.
    if (model.contains(QLatin1String("980 PRO")) && olderThan(firmware.trimmed().toUpper(), QStringLiteral("5B2QGXA7")))
        out << Fact{Fact::Kind::Firmware, tr("Samsung's firmware before 5B2QGXA7 for this model has a bug that makes its health drop fast, "
                                             "until it turns read-only. Update its firmware (Samsung Magician, or fwupd) soon.")};
    if (model.contains(QLatin1String("990 PRO")) && olderThan(firmware.trimmed().toUpper(), QStringLiteral("4B2QJXD7")))
        out << Fact{Fact::Kind::Firmware, tr("Samsung's firmware before 4B2QJXD7 for this model has a bug that wears it out much faster than "
                                             "it should. Update its firmware (Samsung Magician, or fwupd) soon.")};
    if (model.contains(QLatin1String("ST3000DM001")))
        out << Fact{Fact::Kind::Note, tr("This model failed far more often than most drives in Backblaze's published records. Keep what's "
                                         "on it backed up.")};
    return out;
}

} // namespace drivefacts
