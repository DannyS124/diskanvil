// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Moving the GPT backup to the end of a bigger disk, on plain files (no root needed).

#include "testkit.h"

#include "../src/common/format.h"
#include "../src/drives/gpt.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr qint64 MiB = 1024 * 1024;

// A 64 MiB disk image with three partitions, made by sfdisk.
bool makeDisk(const QString &path, int sectorSize)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || !f.resize(64 * MiB))
        return false;
    f.close();
    QFile script(path + QStringLiteral(".sfdisk"));
    if (!script.open(QIODevice::WriteOnly))
        return false;
    script.write("label: gpt\nsize=8MiB, type=linux\nsize=16MiB, type=linux\nsize=20MiB, type=linux\n");
    script.close();
    int code = -1;
    sh(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("sfdisk --quiet --sector-size %1 '%2' < '%3'").arg(sectorSize).arg(path, script.fileName())}, &code);
    return code == 0;
}

// sfdisk exits 0 even when it finds problems; it reports them as text on both streams.
bool sfdiskVerify(const QString &path, int sectorSize, QString *output)
{
    *output = sh(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("sfdisk --verify --sector-size %1 '%2' 2>&1").arg(sectorSize).arg(path)});
    return output->contains(QLatin1String("No errors detected")) && !output->contains(QLatin1String("mismatch"))
        && !output->contains(QLatin1String("not on the end"));
}

QString partitions(const QString &path, int sectorSize)
{
    // start and size of each partition, plus its unique ID
    return sh(QStringLiteral("sfdisk"), {QStringLiteral("--dump"), QStringLiteral("--sector-size"), QString::number(sectorSize), path})
        .section(QStringLiteral("\n\n"), 1);
}

QString lastLba(const QString &path, int sectorSize)
{
    return sh(QStringLiteral("sh"), {QStringLiteral("-c"),
                                     QStringLiteral("sfdisk --dump --sector-size %1 '%2' | sed -n 's/^last-lba: //p'").arg(sectorSize).arg(path)});
}

gpt::Result relocate(const QString &path, int sectorSize, bool newGuids)
{
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return {false, QStringLiteral("open failed")};
    const gpt::Result r = gpt::relocateBackup(fd, sectorSize, newGuids);
    ::close(fd);
    return r;
}

void grow(const QString &path, qint64 size)
{
    QFile f(path);
    if (f.open(QIODevice::ReadWrite))
        f.resize(size);
}

void sectorSizeTests(int sectorSize)
{
    const QString tag = sectorSize == 512 ? QString() : QStringLiteral(" (%1-byte sectors)").arg(sectorSize);
    QTemporaryDir dir;
    const QString disk = dir.filePath(QStringLiteral("disk.img"));
    if (!makeDisk(disk, sectorSize))
        return report(false, QStringLiteral("make a GPT test disk") + tag);
    const QString before = partitions(disk, sectorSize);

    grow(disk, 96 * MiB);
    QString verify;
    report(!sfdiskVerify(disk, sectorSize, &verify), QStringLiteral("a disk copied onto a bigger one has its backup in the wrong place") + tag);

    gpt::Result r = relocate(disk, sectorSize, false);
    report(r.ok, QStringLiteral("move the backup to the end") + tag, r.error);
    const bool clean = sfdiskVerify(disk, sectorSize, &verify);
    report(clean, QStringLiteral("sfdisk finds no problems afterwards") + tag, clean ? QString() : verify);
    report(partitions(disk, sectorSize) == before, QStringLiteral("partitions and their IDs are unchanged") + tag);
    report(lastLba(disk, sectorSize).toLongLong() > (64 * MiB) / sectorSize, QStringLiteral("the new space can be used") + tag, lastLba(disk, sectorSize));

    // Second run on an already-correct disk changes nothing.
    const QString hashBefore = sha256File(disk);
    r = relocate(disk, sectorSize, false);
    report(r.ok && sha256File(disk) == hashBefore, QStringLiteral("running it again changes nothing") + tag, r.error);

    // New IDs for a clone that stays next to its original.
    grow(disk, 128 * MiB);
    r = relocate(disk, sectorSize, true);
    const QString after = partitions(disk, sectorSize);
    report(r.ok && sfdiskVerify(disk, sectorSize, &verify), QStringLiteral("new IDs: still a valid table") + tag, r.error);
    auto starts = [](const QString &dump) {
        QStringList out;
        for (const QString &line : dump.split(QLatin1Char('\n')))
            out << line.section(QStringLiteral(", uuid="), 0, 0);
        return out;
    };
    report(after != before && starts(after) == starts(before), QStringLiteral("new IDs: every partition gets a new ID, nothing else moves") + tag);
}

} // namespace

// The inspector, on images sfdisk made: an intact table, then each kind of damage.
gpt::Report inspectFile(const QString &path, int sectorSize)
{
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return {};
    gpt::Report r = gpt::inspect(fd, sectorSize);
    ::close(fd);
    return r;
}

void zeroSector(const QString &path, int sectorSize, qint64 lba)
{
    QFile f(path);
    if (f.open(QIODevice::ReadWrite) && f.seek(lba * sectorSize))
        f.write(QByteArray(sectorSize, '\0'));
}

void inspectorTests(int sectorSize)
{
    const QString tag = QStringLiteral(" (%1-byte sectors)").arg(sectorSize);
    QTemporaryDir dir;
    const QString disk = dir.filePath(QStringLiteral("inspect.img"));
    if (!makeDisk(disk, sectorSize))
        return report(false, QStringLiteral("make a GPT test disk") + tag);
    gpt::Report r = inspectFile(disk, sectorSize);
    const qint64 last = QFileInfo(disk).size() / sectorSize - 1;
    report(r.problems.isEmpty() && r.primary.valid() && r.backup.valid() && r.backupMatches && r.mbr.protective() && r.primary.entries.size() == 3
               && r.primary.entries[0].firstLba == quint64(1024 * 1024 / sectorSize) && r.backup.lba == quint64(last)
               && r.primary.entries[0].type == QLatin1String("0fc63daf-8483-4772-8e79-3d69d8477de4"),
           QStringLiteral("an intact table reads back: both headers, three partitions, protective MBR") + tag, r.problems.join(QStringLiteral(" | ")));

    const QString copy = dir.filePath(QStringLiteral("copy.img"));
    auto fresh = [&] {
        QFile::remove(copy);
        QFile::copy(disk, copy);
    };
    fresh();
    zeroSector(copy, sectorSize, 1);
    r = inspectFile(copy, sectorSize);
    report(!r.primary.present && r.backup.valid() && r.problems.join(QLatin1Char(' ')).contains(QLatin1String("main GPT header is missing")),
           QStringLiteral("a wiped main header is found, and the backup is still good") + tag, r.problems.join(QStringLiteral(" | ")));
    fresh();
    {
        QFile f(copy);
        if (f.open(QIODevice::ReadWrite) && f.seek(2 * sectorSize + 40))
            f.write("\x07", 1); // a partition's end, without fixing the checksum
    }
    r = inspectFile(copy, sectorSize);
    report(r.primary.headerCrcOk && !r.primary.entriesCrcOk && r.problems.join(QLatin1Char(' ')).contains(QLatin1String("partition list is damaged")),
           QStringLiteral("a changed partition list fails its checksum") + tag, r.problems.join(QStringLiteral(" | ")));
    fresh();
    zeroSector(copy, sectorSize, last);
    r = inspectFile(copy, sectorSize);
    report(r.primary.valid() && !r.backup.present && r.problems.join(QLatin1Char(' ')).contains(QLatin1String("backup GPT header")),
           QStringLiteral("a missing backup header is found") + tag, r.problems.join(QStringLiteral(" | ")));
    fresh();
    zeroSector(copy, sectorSize, 0);
    r = inspectFile(copy, sectorSize);
    report(r.primary.valid() && r.problems.join(QLatin1Char(' ')).contains(QLatin1String("protective MBR")),
           QStringLiteral("a missing protective MBR is noticed") + tag, r.problems.join(QStringLiteral(" | ")));
    fresh();
    grow(copy, 96 * MiB);
    r = inspectFile(copy, sectorSize);
    report(r.primary.valid() && r.backupElsewhere && r.backup.valid() && r.problems.join(QLatin1Char(' ')).contains(QLatin1String("isn't at the end")),
           QStringLiteral("a backup left in the middle (copied to a bigger drive) is found") + tag, r.problems.join(QStringLiteral(" | ")));
}

void mbrInspectorTests()
{
    QTemporaryDir dir;
    const QString disk = dir.filePath(QStringLiteral("mbr.img"));
    {
        QFile f(disk);
        if (!f.open(QIODevice::WriteOnly) || !f.resize(32 * MiB))
            return report(false, QStringLiteral("make an MBR test disk"));
    }
    sh(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("printf 'label: dos\\nsize=8MiB, type=83, bootable\\ntype=c\\n' | sfdisk --quiet '%1'").arg(disk)});
    const gpt::Report r = inspectFile(disk, 512);
    report(r.problems.isEmpty() && r.mbr.signature && r.mbr.entries.size() == 2 && r.mbr.entries[0].bootable && r.mbr.entries[0].type == 0x83
               && r.mbr.entries[1].type == 0x0c && !r.primary.present,
           QStringLiteral("an MBR table reads back, with its boot flag"), r.problems.join(QStringLiteral(" | ")));
    const QString dump = gpt::hexDump(QByteArray("EFI PART\x00\x00\x01\x00\x5c\x00\x00\x00 tail", 21), 512);
    report(dump.startsWith(QLatin1String("00000200  45 46 49 20 50 41 52 54  00 00 01 00 5c 00 00 00  |EFI PART....\\...|"))
               && dump.count(QLatin1Char('\n')) == 2,
           QStringLiteral("the hex view shows offsets, bytes and text"), dump.left(80));
}

// Type and Flags: what's offered, what typed-in types are refused, and flags it doesn't
// show staying as they were.
void typeAndFlags()
{
    const QVector<PartitionTypeChoice> gptTypes = partitionTypeChoices(QStringLiteral("gpt"));
    const QVector<PartitionTypeChoice> dosTypes = partitionTypeChoices(QStringLiteral("dos"));
    bool named = !gptTypes.isEmpty() && !dosTypes.isEmpty();
    for (const PartitionTypeChoice &c : gptTypes + dosTypes)
        named = named && !c.name.isEmpty() && !c.name.contains(c.value) && partitionTypeProblem(c.value.startsWith(QLatin1String("0x")) ? QStringLiteral("dos") : QStringLiteral("gpt"), c.value).isEmpty();
    report(named, QStringLiteral("every offered type has a plain name and passes the checks"));
    const QString gpt = QStringLiteral("gpt"), dos = QStringLiteral("dos");
    report(partitionTypeProblem(gpt, QStringLiteral("0fc63daf-8483-4772-8e79-3d69d8477de4")).isEmpty()
               && !partitionTypeProblem(gpt, QStringLiteral("00000000-0000-0000-0000-000000000000")).isEmpty()
               && !partitionTypeProblem(gpt, QStringLiteral("0fc63daf-8483-4772-8e79-3d69d8477de")).isEmpty()
               && !partitionTypeProblem(gpt, QStringLiteral("0FC63DAF-8483-4772-8E79-3D69D8477DE4 ")).isEmpty()
               && !partitionTypeProblem(gpt, QStringLiteral("0x83")).isEmpty(),
           QStringLiteral("GPT types: a GUID, never the all-zero one that empties the entry"));
    report(partitionTypeProblem(dos, QStringLiteral("0x83")).isEmpty() && partitionTypeProblem(dos, QStringLiteral("0xa5")).isEmpty()
               && !partitionTypeProblem(dos, QStringLiteral("0x00")).isEmpty() && !partitionTypeProblem(dos, QStringLiteral("0x05")).isEmpty()
               && !partitionTypeProblem(dos, QStringLiteral("0x0f")).isEmpty() && !partitionTypeProblem(dos, QStringLiteral("0x85")).isEmpty()
               && !partitionTypeProblem(dos, QStringLiteral("83")).isEmpty() && !partitionTypeProblem(dos, QStringLiteral("0x183")).isEmpty()
               && !partitionTypeProblem(QStringLiteral("loop"), QStringLiteral("0x83")).isEmpty(),
           QStringLiteral("MBR types: two hex digits, never empty or extended"));
    const quint64 secret = quint64(1) << 48; // a type-specific bit the dialog doesn't show
    report(mergedPartitionFlags(gpt, secret | 1, quint64(1) << 63) == (secret | (quint64(1) << 63))
               && mergedPartitionFlags(dos, 0, 0x80 | 0x01) == 0x80 && partitionFlagChoices(dos).size() == 1
               && partitionFlagChoices(gpt).size() == 5,
           QStringLiteral("flags the dialog doesn't show stay as they were"));
    report(isBootPartitionType(QStringLiteral("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")) && isBootPartitionType(QStringLiteral("0xef"))
               && !isBootPartitionType(QStringLiteral("0x83")),
           QStringLiteral("EFI and BIOS boot types are recognized (changing them warns)"));
}

void gptTests()
{
    sectorSizeTests(512);
    sectorSizeTests(4096);
    inspectorTests(512);
    inspectorTests(4096);
    mbrInspectorTests();
    typeAndFlags();

    QTemporaryDir dir;
    const QString disk = dir.filePath(QStringLiteral("disk.img"));
    if (!makeDisk(disk, 512))
        return report(false, QStringLiteral("make a GPT test disk"));
    grow(disk, 96 * MiB);

    // A damaged partition list must be left alone.
    {
        QFile f(disk);
        if (f.open(QIODevice::ReadWrite) && f.seek(2 * 512 + 40)) // first entry's start LBA
            f.write("\x01", 1);
    }
    const QString hashBefore = sha256File(disk);
    gpt::Result r = relocate(disk, 512, false);
    report(!r.ok && sha256File(disk) == hashBefore, QStringLiteral("a damaged partition list is refused and nothing is written"), r.error);

    // So must a disk that isn't GPT at all.
    QFile::remove(disk);
    {
        QFile f(disk);
        if (f.open(QIODevice::WriteOnly))
            f.resize(8 * MiB);
    }
    r = relocate(disk, 512, false);
    report(!r.ok && r.error.contains(QLatin1String("GPT")), QStringLiteral("a disk without GPT is refused"), r.error);

    // And a disk too small for its partitions (an image restored onto a smaller one).
    if (makeDisk(disk, 512)) {
        grow(disk, 40 * MiB);
        r = relocate(disk, 512, false);
        report(!r.ok && r.error.contains(QLatin1String("too small")), QStringLiteral("a disk too small for its partitions is refused"), r.error);
    }
}
