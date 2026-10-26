// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Recover Partitions, on image files (as the user): a GPT put back from its own backup and
// from a saved layout, an MBR with logical partitions, both sector sizes, remembering
// layouts, and layouts that must be refused. "The same" means sfdisk --dump says so.

#include "testkit.h"

#include <cstring>

#include "../src/drives/gpt.h"
#include "../src/drives/partrecover.h"
#include "../src/drives/partscan.h"
#include "../src/drives/tableconvert.h"
#include "../src/drives/blockio.h"
#include "../src/drives/fsprobe.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr qint64 MiB = 1024 * 1024;

bool makeImage(const QString &path, qint64 size, const QByteArray &script, int sectorSize)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || !f.resize(size))
        return false;
    f.close();
    QFile s(path + QStringLiteral(".sfdisk"));
    if (!s.open(QIODevice::WriteOnly))
        return false;
    s.write(script);
    s.close();
    int code = -1;
    sh(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("sfdisk --quiet --sector-size %1 '%2' < '%3'").arg(sectorSize).arg(path, s.fileName())}, &code);
    return code == 0;
}

QString dump(const QString &path, int sectorSize, bool firstLba = true)
{
    // The device line names the file, which differs between copies; the rest must match.
    QString out = sh(QStringLiteral("sfdisk"), {QStringLiteral("--dump"), QStringLiteral("--sector-size"), QString::number(sectorSize), path})
                      .remove(QRegularExpression(QStringLiteral("^device: .*$"), QRegularExpression::MultilineOption))
                      .replace(path, QStringLiteral("IMG"));
    // Where the usable area starts isn't part of a saved layout (UDisks doesn't say), and
    // the writer uses the smallest; sfdisk starts it at 1 MiB. No partition moves either way.
    if (!firstLba)
        out.remove(QRegularExpression(QStringLiteral("^first-lba: .*$"), QRegularExpression::MultilineOption));
    return out;
}

void zero(const QString &path, qint64 offset, qint64 length)
{
    QFile f(path);
    if (f.open(QIODevice::ReadWrite) && f.seek(offset))
        f.write(QByteArray(length, '\0'));
}

template <typename Fn>
auto withFd(const QString &path, Fn fn)
{
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDWR | O_CLOEXEC);
    auto result = fn(fd);
    if (fd >= 0)
        ::close(fd);
    return result;
}

void gptRecovery(int sectorSize)
{
    const QString tag = QStringLiteral(" (%1-byte sectors)").arg(sectorSize);
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("gpt.img"));
    if (!makeImage(image, 64 * MiB,
                   "label: gpt\nsize=8MiB, type=uefi, name=\"EFI\"\nsize=16MiB, type=linux, name=\"root\"\nsize=20MiB, type=linux, name=\"home\"\n",
                   sectorSize))
        return report(false, QStringLiteral("make a GPT image") + tag);
    const QString original = dump(image, sectorSize);
    QFile::copy(image, image + QStringLiteral(".orig"));
    const qint64 sectors = 64 * MiB / sectorSize;

    // The main header wiped: the backup puts it back exactly.
    zero(image, sectorSize, sectorSize);
    gpt::Result r = withFd(image, [sectorSize](int fd) { return gpt::restoreFromBackup(fd, sectorSize); });
    report(r.ok && dump(image, sectorSize) == original && withFd(image, [sectorSize](int fd) { return gpt::inspect(fd, sectorSize); }).problems.isEmpty(),
           QStringLiteral("a wiped main GPT header is put back from the backup, exactly") + tag, r.error);

    // Both copies wiped: a saved layout puts it back exactly (through JSON, like it's kept).
    const recover::Layout taken = recover::fromReport(withFd(image, [sectorSize](int fd) { return gpt::inspect(fd, sectorSize); }), false);
    recover::Layout kept;
    const bool roundTrip = recover::fromJson(QJsonDocument::fromJson(QJsonDocument(recover::toJson(taken)).toJson()).object(), &kept)
        && kept.fingerprint() == taken.fingerprint();
    report(roundTrip && taken.parts.size() == 3, QStringLiteral("a layout survives being saved and read back") + tag);
    zero(image, 0, 34 * sectorSize);
    zero(image, (sectors - 33) * sectorSize, 33 * sectorSize);
    report(withFd(image, [sectorSize](int fd) { return gpt::inspect(fd, sectorSize); }).problems.join(QLatin1Char(' ')).contains(QLatin1String("no partition table")),
           QStringLiteral("with both copies wiped, there's no table left") + tag);
    r = withFd(image, [&kept, sectorSize](int fd) { return recover::write(fd, kept, sectorSize); });
    report(r.ok && dump(image, sectorSize, false) == dump(dir.filePath(QStringLiteral("gpt.img.orig")), sectorSize, false),
           QStringLiteral("a saved layout puts it all back exactly (IDs, names, types)") + tag, r.ok ? dump(image, sectorSize, false) : r.error);
}

void mbrRecovery()
{
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("mbr.img"));
    if (!makeImage(image, 64 * MiB,
                   "label: dos\nlabel-id: 0x1234abcd\nsize=8MiB, type=83, bootable\ntype=5\nsize=12MiB, type=83\nsize=16MiB, type=c\n", 512))
        return report(false, QStringLiteral("make an MBR image with logical partitions"));
    const QString original = dump(image, 512);
    // A layout made the way UDisks reports the partitions (as fromDisk would see them).
    const QJsonArray parts = QJsonDocument::fromJson(sh(QStringLiteral("sfdisk"), {QStringLiteral("--json"), image}).toUtf8())
                                 .object()
                                 .value(QStringLiteral("partitiontable"))
                                 .toObject()
                                 .value(QStringLiteral("partitions"))
                                 .toArray();
    recover::Layout layout;
    layout.saved = QDateTime::currentDateTimeUtc();
    layout.table = QStringLiteral("dos");
    layout.diskSize = quint64(64 * MiB);
    layout.diskId = QStringLiteral("1234abcd");
    for (const QJsonValue &v : parts) {
        const QJsonObject o = v.toObject();
        recover::Part p;
        p.number = o.value(QStringLiteral("node")).toString().section(QLatin1Char('g'), -1).toInt(); // ".../mbr.img5" -> 5
        p.start = quint64(o.value(QStringLiteral("start")).toInteger()) * 512;
        p.size = quint64(o.value(QStringLiteral("size")).toInteger()) * 512;
        p.type = QStringLiteral("0x%1").arg(o.value(QStringLiteral("type")).toString().toUInt(nullptr, 16), 2, 16, QLatin1Char('0'));
        p.flags = o.value(QStringLiteral("bootable")).toBool() ? 0x80 : 0;
        p.container = p.type == QLatin1String("0x05");
        p.logical = p.number >= 5;
        layout.parts.push_back(p);
    }
    report(layout.parts.size() == 4 && recover::problem(layout, quint64(64 * MiB), 512).isEmpty(), QStringLiteral("the MBR layout reads as four partitions, two of them logical"),
           recover::problem(layout, quint64(64 * MiB), 512));
    // Wipe the MBR and every EBR (the first sector of each gap before a logical partition).
    zero(image, 0, 512);
    for (const recover::Part &p : std::as_const(layout.parts)) {
        if (p.container)
            zero(image, qint64(p.start), 512);
    }
    zero(image, qint64(layout.parts.value(2).start + layout.parts.value(2).size), 512);
    report(dump(image, 512) != original, QStringLiteral("the MBR and the logical partitions' EBRs are wiped"));
    const gpt::Result r = withFd(image, [&layout](int fd) { return recover::write(fd, layout, 512); });
    report(r.ok && dump(image, 512) == original, QStringLiteral("the MBR comes back exactly, logical partitions and boot flag included"),
           r.ok ? dump(image, 512) : r.error);
}

void remembering()
{
    QTemporaryDir data;
    qputenv("XDG_DATA_HOME", QFile::encodeName(data.path()));
    recover::Layout one;
    one.saved = QDateTime::currentDateTimeUtc();
    one.table = QStringLiteral("gpt");
    one.diskSize = 100 * MiB;
    recover::Part p;
    p.number = 1;
    p.start = MiB;
    p.size = 10 * MiB;
    p.type = QStringLiteral("0fc63daf-8483-4772-8e79-3d69d8477de4");
    one.parts = {p};
    const QString key = QStringLiteral("HGST-HTS545050A7E380-TEST");
    report(recover::remember(key, one) && !recover::remember(key, one), QStringLiteral("a layout is kept once, not again when nothing changed"));
    for (int i = 2; i <= 13; ++i) {
        recover::Layout next = one;
        next.parts[0].size = quint64(i) * MiB;
        recover::remember(key, next);
    }
    const QVector<recover::Layout> list = recover::saved(key);
    report(list.size() == 10 && list.first().parts[0].size == 13 * MiB, QStringLiteral("only the last 10 different layouts are kept, newest first"));
    recover::Layout empty = one;
    empty.parts.clear();
    report(!recover::remember(key, empty), QStringLiteral("an empty table isn't kept"));
    recover::remember(QStringLiteral("../../escape"), one);
    recover::remember(QStringLiteral("."), one);
    const QStringList files = QDir(recover::folder()).entryList(QDir::Files);
    bool inside = !QFile::exists(data.path() + QStringLiteral("/escape.json")) && !QFile::exists(QDir(data.path()).filePath(QStringLiteral("../escape.json")));
    for (const QString &f : files)
        inside = inside && !f.startsWith(QLatin1Char('.'));
    report(inside && files.size() == 3, QStringLiteral("odd drive names stay inside the layouts folder"), files.join(QLatin1Char(' ')));
}

void refusals()
{
    recover::Layout l;
    l.saved = QDateTime::currentDateTimeUtc();
    l.table = QStringLiteral("gpt");
    recover::Part a;
    a.number = 1;
    a.start = MiB;
    a.size = 10 * MiB;
    a.type = QStringLiteral("0fc63daf-8483-4772-8e79-3d69d8477de4");
    recover::Part b = a;
    b.number = 2;
    b.start = 5 * MiB;
    l.parts = {a, b};
    report(recover::problem(l, 100 * MiB, 512).contains(QLatin1String("overlap")), QStringLiteral("overlapping partitions are refused"));
    l.parts = {a};
    report(recover::problem(l, 8 * MiB, 512).contains(QLatin1String("past the end")), QStringLiteral("a layout bigger than the drive is refused"));
    recover::Part odd = a;
    odd.start = MiB + 100;
    l.parts = {odd};
    report(recover::problem(l, 100 * MiB, 512).contains(QLatin1String("sector")), QStringLiteral("partitions off the sector grid are refused"));
    recover::Part early = a;
    early.start = 512;
    l.parts = {early};
    report(recover::problem(l, 100 * MiB, 512).contains(QLatin1String("table itself")), QStringLiteral("a partition where the GPT itself goes is refused"));
    l.table = QStringLiteral("dos");
    l.parts.clear();
    for (int i = 0; i < 5; ++i) {
        recover::Part m = a;
        m.number = i + 1;
        m.start = quint64(i + 1) * MiB;
        m.size = MiB;
        m.type = QStringLiteral("0x83");
        l.parts.push_back(m);
    }
    report(recover::problem(l, 100 * MiB, 512).contains(QLatin1String("four")), QStringLiteral("five primary MBR partitions are refused"));

    // The writer refuses too, and writes nothing then.
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("refuse.img"));
    QFile f(image);
    if (!f.open(QIODevice::WriteOnly) || !f.resize(32 * MiB))
        return report(false, QStringLiteral("make an empty image"));
    f.close();
    gpt::Entry e1, e2;
    e1.index = 1;
    e1.type = QStringLiteral("0fc63daf-8483-4772-8e79-3d69d8477de4");
    e1.firstLba = 2048;
    e1.lastLba = 10000;
    e2 = e1;
    e2.index = 2;
    e2.firstLba = 9000;
    e2.lastLba = 20000;
    const gpt::Result r = withFd(image, [&](int fd) { return gpt::writeTable(fd, 512, QString(), {e1, e2}); });
    const QByteArray after = sh(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("head -c 1048576 '%1' | tr -d '\\000' | wc -c").arg(image)}).trimmed().toLatin1();
    report(!r.ok && r.error.contains(QLatin1String("overlap")) && after == "0", QStringLiteral("the GPT writer refuses overlaps and writes nothing"), r.error);
}

// Finding file systems where the table is gone: ext4 and FAT made in place, Btrfs, swap
// and LUKS made apart and copied in, at known places on a drive with no table at all.
void scanning()
{
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("lost.img"));
    QFile f(image);
    if (!f.open(QIODevice::WriteOnly) || !f.resize(512 * MiB))
        return report(false, QStringLiteral("make an image to scan"));
    f.close();
    auto run = [](const QString &command) {
        int code = -1;
        sh(QStringLiteral("sh"), {QStringLiteral("-c"), command}, &code);
        return code == 0;
    };
    auto copyIn = [&](const QString &part, qint64 atMiB) {
        return run(QStringLiteral("dd if='%1' of='%2' bs=1M seek=%3 conv=notrunc status=none").arg(part, image).arg(atMiB));
    };
    const QString btrfs = dir.filePath(QStringLiteral("btrfs.part")), swap = dir.filePath(QStringLiteral("swap.part")),
                  luks = dir.filePath(QStringLiteral("luks.part"));
    bool made = run(QStringLiteral("mke2fs -q -t ext4 -L root -E offset=%1 '%2' 100M").arg(1 * MiB).arg(image))
        && run(QStringLiteral("mkfs.fat -F 16 --offset %1 -n DATA '%2' 65536 >/dev/null").arg(102 * MiB / 512).arg(image))
        && run(QStringLiteral("truncate -s 200M '%1' && mkfs.btrfs -q -L store '%1'").arg(btrfs)) && copyIn(btrfs, 170)
        && run(QStringLiteral("truncate -s 32M '%1' && mkswap -q '%1'").arg(swap)) && copyIn(swap, 380)
        && run(QStringLiteral("truncate -s 32M '%1' && printf test | cryptsetup luksFormat --batch-mode --type luks2 --pbkdf pbkdf2 "
                              "--pbkdf-force-iterations 1000 --key-file=- '%1'").arg(luks))
        && copyIn(luks, 420);
    report(made, QStringLiteral("make a drive with five file systems and no partition table"));
    if (!made)
        return;

    const int fd = ::open(QFile::encodeName(image).constData(), O_RDWR | O_CLOEXEC);
    quint64 lastDone = 0;
    int calls = 0;
    const QVector<partscan::Found> found = partscan::scan(fd, quint64(512 * MiB), [&](quint64 done, quint64) {
        lastDone = done;
        ++calls;
        return true;
    });
    auto at = [&found](qint64 offset) -> partscan::Found {
        for (const partscan::Found &x : found) {
            if (x.offset == quint64(offset))
                return x;
        }
        return {};
    };
    report(at(1 * MiB).type == QLatin1String("ext4") && at(1 * MiB).label == QLatin1String("root") && at(102 * MiB).type == QLatin1String("vfat")
               && at(170 * MiB).type == QLatin1String("btrfs") && at(380 * MiB).type == QLatin1String("swap")
               && at(420 * MiB).type == QLatin1String("crypto_LUKS") && lastDone == quint64(512 * MiB) && calls > 1,
           QStringLiteral("the scan finds all five where they start, and reports its progress"),
           QStringLiteral("%1 found (ext4 with 1 KiB blocks keeps backup superblocks where a file system 8, 24, 40 MiB later would start)").arg(found.size()));

    const QVector<partscan::Found> parts = partscan::tidy(found, quint64(512 * MiB));
    const bool sizes = parts.size() == 5 && parts[0].size == quint64(100 * MiB) && parts[1].size == quint64(64 * MiB)
        && parts[2].size == quint64(200 * MiB) && parts[3].size == quint64(32 * MiB) && parts[4].sizeGuessed
        && parts[4].offset + parts[4].size <= quint64(512 * MiB) - 34 * 512;
    report(sizes, QStringLiteral("only the five are kept (not ext4's backup superblocks); sizes come from the file systems, and LUKS (which doesn't say) runs up to the end"));
    const recover::Layout layout = partscan::toLayout(parts, QStringLiteral("gpt"), quint64(512 * MiB));
    const gpt::Result r = recover::write(fd, layout, 512);
    ::close(fd);
    const QString table = dump(image, 512);
    report(r.ok && table.contains(QLatin1String("start=        2048, size=      204800, type=0FC63DAF")) && table.contains(QLatin1String("type=EBD0A0A2"))
               && table.contains(QLatin1String("type=0657FD6D")) && table.contains(QLatin1String("type=CA7D7CCB")) && table.contains(QLatin1String("name=\"root\"")),
           QStringLiteral("a table is written from what was found, with fitting types"), r.ok ? table : r.error);

    // Stop: the scan ends when asked, with what it found so far.
    const int again = ::open(QFile::encodeName(image).constData(), O_RDONLY | O_CLOEXEC);
    // It looks at Stop every 64 places (64 MiB here): asked at 300 MiB, it ends before the swap at 380.
    const QVector<partscan::Found> early = partscan::scan(again, quint64(512 * MiB), [](quint64 done, quint64) { return done < quint64(300 * MiB); });
    ::close(again);
    bool beforeStop = !early.isEmpty();
    for (const partscan::Found &x : early)
        beforeStop = beforeStop && x.offset < quint64(380 * MiB);
    report(beforeStop && partscan::tidy(early, quint64(512 * MiB)).size() == 3, QStringLiteral("Stop ends the scan, keeping what it had found"),
           QString::number(early.size()));
}

// DiskAnvil's own probe (what Windows has for Recover Partitions) against libblkid's, on file
// systems made by their own tools: the same type, and the same size, which is what a found
// partition gets. The partition has to hold all of it, so the own probe counts NTFS's last
// sector (the boot sector's copy) and swap's header page, which libblkid leaves out.
void probeSizes()
{
    QTemporaryDir dir;
    struct Case {
        const char *name;
        int mib;
        QString make; // %1: the image
        quint64 more = 0; // what libblkid leaves out
    };
    const QVector<Case> cases = {
        {"ext2 with 1 KiB blocks", 20, QStringLiteral("mke2fs -q -t ext2 -b 1024 '%1'")},
        {"ext4, 64-bit", 120, QStringLiteral("mke2fs -q -t ext4 -O 64bit '%1'")},
        {"FAT16", 64, QStringLiteral("mkfs.fat -F 16 '%1' >/dev/null")},
        {"FAT32", 100, QStringLiteral("mkfs.fat -F 32 '%1' >/dev/null")},
        {"exFAT", 64, QStringLiteral("mkfs.exfat -q '%1' >/dev/null 2>&1 || mkfs.exfat '%1' >/dev/null")},
        {"NTFS", 64, QStringLiteral("mkntfs -q -F -f '%1'"), 512},
        {"Btrfs", 200, QStringLiteral("mkfs.btrfs -q '%1'")},
        {"swap", 32, QStringLiteral("mkswap -q '%1'"), 4096},
    };
    for (const Case &c : cases) {
        const QString image = dir.filePath(QString::fromLatin1(c.name).replace(QLatin1Char(' '), QLatin1Char('_')) + QStringLiteral(".img"));
        int code = -1;
        sh(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("truncate -s %1M '%2' && ").arg(c.mib).arg(image) + c.make.arg(image)}, &code);
        if (code != 0) {
            out << "SKIP  " << c.name << ": its mkfs isn't installed" << Qt::endl;
            continue;
        }
        const quint64 size = quint64(c.mib) * MiB;
        const int fd = ::open(QFile::encodeName(image).constData(), O_RDONLY | O_CLOEXEC);
        const std::optional<partscan::Found> blkid = partscan::probe(fd, 0, size);
        QByteArray head(fsprobe::kHeadSize, '\0');
        const bool read = blockio::readAt(fd, head.data(), quint64(head.size()), 0);
        ::close(fd);
        const fsprobe::Result own = fsprobe::probe(head);
        const quint64 expected = blkid && blkid->size ? blkid->size + c.more : 0;
        report(read && blkid && own.type == blkid->type && own.size == expected && own.size > 0 && own.size <= size,
               QStringLiteral("own probe, %1: the same type and size as libblkid").arg(QLatin1String(c.name)),
               QStringLiteral("%1 %2 bytes, libblkid %3 %4").arg(own.type).arg(own.size).arg(blkid ? blkid->type : QString()).arg(blkid ? blkid->size : 0));
    }
}

// bcachefs, from an image bcachefs-tools made (tests/data/lostfs/bcachefs.img.zst, its label
// AnvilTest): the same type, label and ID as libblkid.
void probeBcachefs()
{
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("bcachefs.img"));
    int code = -1;
    sh(QStringLiteral("zstd"), {QStringLiteral("-q"), QStringLiteral("-d"), QStringLiteral(SOURCE_DIR "/tests/data/lostfs/bcachefs.img.zst"), QStringLiteral("-o"), image}, &code);
    const int fd = ::open(QFile::encodeName(image).constData(), O_RDONLY | O_CLOEXEC);
    QByteArray head(fsprobe::kHeadSize, '\0');
    const bool read = fd >= 0 && blockio::readAt(fd, head.data(), quint64(head.size()), 0);
    const std::optional<partscan::Found> blkid = fd >= 0 ? partscan::probe(fd, 0, 32 * MiB) : std::nullopt;
    if (fd >= 0)
        ::close(fd);
    const fsprobe::Result own = fsprobe::probe(head);
    report(read && own.type == QLatin1String("bcachefs") && own.label == QLatin1String("AnvilTest") && own.uuid == QLatin1String("11223344-5566-7788-99aa-bbccddeeff00")
               && (!blkid || blkid->type == own.type),
           QStringLiteral("own probe, bcachefs: its type, label and ID (libblkid: %1)").arg(blkid ? blkid->type : QStringLiteral("not asked")),
           own.type + QLatin1Char(' ') + own.label + QLatin1Char(' ') + own.uuid);
    // bcache, the block cache, has the same magic and isn't a file system.
    QByteArray cache = head;
    const QByteArray bcacheMagic = QByteArray::fromHex("c68573f64e1a45ca8265f57f48ba6d81");
    std::memcpy(cache.data() + 4096 + 24, bcacheMagic.constData(), 16);
    cache[4096 + 16] = 3;
    cache[4096 + 17] = 0;
    report(fsprobe::probe(cache).type != QLatin1String("bcachefs"), QStringLiteral("own probe: bcache's own superblock isn't taken for bcachefs"));
}

} // namespace

// The layout of an image as fromDisk() would see it, from sfdisk.
recover::Layout layoutOf(const QString &image, quint64 size)
{
    const QJsonObject table = QJsonDocument::fromJson(sh(QStringLiteral("sfdisk"), {QStringLiteral("--json"), image}).toUtf8())
                                  .object()
                                  .value(QStringLiteral("partitiontable"))
                                  .toObject();
    recover::Layout l;
    l.table = table.value(QStringLiteral("label")).toString();
    l.diskSize = size;
    l.diskId = table.value(QStringLiteral("id")).toString().toLower().remove(QStringLiteral("0x"));
    const int sector = table.value(QStringLiteral("sectorsize")).toInt(512);
    for (const QJsonValue &v : table.value(QStringLiteral("partitions")).toArray()) {
        const QJsonObject o = v.toObject();
        recover::Part p;
        p.number = o.value(QStringLiteral("node")).toString().section(QLatin1Char('g'), -1).toInt();
        p.start = quint64(o.value(QStringLiteral("start")).toInteger()) * quint64(sector);
        p.size = quint64(o.value(QStringLiteral("size")).toInteger()) * quint64(sector);
        if (l.table == QLatin1String("gpt")) {
            p.type = o.value(QStringLiteral("type")).toString().toLower();
            p.guid = o.value(QStringLiteral("uuid")).toString().toLower();
            p.name = o.value(QStringLiteral("name")).toString();
        } else {
            p.type = QStringLiteral("0x%1").arg(o.value(QStringLiteral("type")).toString().toUInt(nullptr, 16), 2, 16, QLatin1Char('0'));
            p.flags = o.value(QStringLiteral("bootable")).toBool() ? 0x80 : 0;
            p.container = p.type == QLatin1String("0x05") || p.type == QLatin1String("0x0f");
            p.logical = p.number >= 5;
        }
        l.parts.push_back(p);
    }
    return l;
}

// A mark in every partition, to show what's in them is never touched.
QByteArray marks(const QString &image, const recover::Layout &l, bool write)
{
    QFile f(image);
    if (!f.open(write ? QIODevice::ReadWrite : QIODevice::ReadOnly))
        return {};
    QByteArray all;
    for (const recover::Part &p : l.parts) {
        if (p.container)
            continue;
        const QByteArray mark = QByteArrayLiteral("partition at ") + QByteArray::number(p.start);
        // the first sector (a file system's boot sector) and the last
        for (const quint64 at : {p.start, p.start + p.size - 512}) {
            f.seek(qint64(at));
            if (write)
                f.write(mark.leftJustified(512, '.'));
            f.seek(qint64(at));
            all += f.read(512);
        }
    }
    return all;
}

void converting()
{
    QTemporaryDir dir;
    // MBR with logical partitions (and a bootable one) to GPT and back.
    const QString image = dir.filePath(QStringLiteral("convert.img"));
    const quint64 size = quint64(64 * MiB);
    if (!makeImage(image, qint64(size), "label: dos\nlabel-id: 0x1234abcd\nsize=8MiB, type=83, bootable\ntype=5\nsize=12MiB, type=7\nsize=16MiB, type=c\n", 512))
        return report(false, QStringLiteral("convert: make an MBR image"));
    const QString original = dump(image, 512);
    const recover::Layout before = layoutOf(image, size);
    const QByteArray inside = marks(image, before, true);
    const tableconvert::Plan toGpt = tableconvert::toGpt(before, size, 512, true);
    report(toGpt.refusal.isEmpty() && toGpt.to.parts.size() == 3 && toGpt.warnings.size() == 2,
           QStringLiteral("convert: MBR to GPT is planned, with the renumbering and BIOS-boot warnings"), toGpt.refusal + toGpt.warnings.join(QLatin1Char('|')));
    gpt::Result r = withFd(image, [&](int fd) { return recover::write(fd, toGpt.to, 512); });
    const recover::Layout after = layoutOf(image, size);
    bool same = after.table == QLatin1String("gpt") && after.parts.size() == 3;
    for (int i = 0; same && i < after.parts.size(); ++i)
        same = after.parts[i].start == toGpt.to.parts[i].start && after.parts[i].size == toGpt.to.parts[i].size;
    report(r.ok && same, QStringLiteral("convert: it's GPT now, every partition in the same place and size"), r.error + dump(image, 512));
    report(after.parts.value(0).type == QLatin1String("0fc63daf-8483-4772-8e79-3d69d8477de4")
               && after.parts.value(1).type == QLatin1String("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"),
           QStringLiteral("convert: Linux and NTFS partitions get GPT's kinds for them"));
    report(marks(image, before, false) == inside, QStringLiteral("convert: what's in the partitions isn't touched"));
    report(withFd(image, [](int fd) { return gpt::inspect(fd, 512); }).problems.isEmpty(), QStringLiteral("convert: the new GPT checks out"));
    r = withFd(image, [&](int fd) { return recover::write(fd, before, 512); });
    report(r.ok && dump(image, 512) == original && marks(image, before, false) == inside,
           QStringLiteral("convert: Undo puts the MBR back exactly"), r.ok ? dump(image, 512) : r.error);

    // GPT to MBR: Windows' reserved partition is left out; the rest keeps its place.
    const QString gptImage = dir.filePath(QStringLiteral("gpt.img"));
    if (!makeImage(gptImage, qint64(size),
                   "label: gpt\nsize=8MiB, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B\nsize=4MiB, type=E3C9E316-0B5C-4DB8-817D-F92DF00215AE\n"
                   "size=16MiB, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7\nsize=8MiB, type=0657FD6D-A4AB-43C4-84E5-0933C84B4F4F\n"
                   "type=0FC63DAF-8483-4772-8E79-3D69D8477DE4\n", 512))
        return report(false, QStringLiteral("convert: make a GPT image"));
    const QString gptOriginal = dump(gptImage, 512, false);
    const recover::Layout gptBefore = layoutOf(gptImage, size);
    const QByteArray gptInside = marks(gptImage, gptBefore, true);
    const tableconvert::Plan toMbr = tableconvert::toMbr(gptBefore, size, 512);
    report(toMbr.refusal.isEmpty() && toMbr.to.parts.size() == 4 && toMbr.warnings.size() == 1,
           QStringLiteral("convert: GPT to MBR is planned, the reserved partition left out, the EFI one warned about"), toMbr.refusal);
    r = withFd(gptImage, [&](int fd) { return recover::write(fd, toMbr.to, 512); });
    const recover::Layout mbr = layoutOf(gptImage, size);
    QStringList kinds;
    for (const recover::Part &p : mbr.parts)
        kinds << p.type;
    report(r.ok && mbr.table == QLatin1String("dos") && kinds == QStringList{QStringLiteral("0xef"), QStringLiteral("0x07"), QStringLiteral("0x82"), QStringLiteral("0x83")},
           QStringLiteral("convert: it's MBR now, with MBR's kinds"), r.error + kinds.join(QLatin1Char(',')));
    report(marks(gptImage, gptBefore, false) == gptInside, QStringLiteral("convert: what's in the partitions isn't touched (GPT to MBR)"));
    r = withFd(gptImage, [&](int fd) { return recover::write(fd, gptBefore, 512); });
    report(r.ok && dump(gptImage, 512, false) == gptOriginal, QStringLiteral("convert: Undo puts the GPT back exactly, IDs and all"), r.ok ? dump(gptImage, 512, false) : r.error);

    // Refused: five partitions for MBR, one past 2 TiB, one where GPT's tables go, a kind MBR has no match for.
    recover::Layout five = gptBefore;
    five.parts.removeAt(1); // no reserved partition: all five count
    five.parts.push_back(five.parts.last());
    five.parts.last().number = 6;
    report(!tableconvert::toMbr(five, size, 512).refusal.isEmpty(), QStringLiteral("convert: more than four partitions can't go on MBR"));
    recover::Layout big = gptBefore;
    big.diskSize = 3ull << 40;
    big.parts.last().size = (3ull << 40) - big.parts.last().start - MiB;
    report(tableconvert::toMbr(big, 3ull << 40, 512).refusal.contains(QStringLiteral("2 TiB")), QStringLiteral("convert: a partition past 2 TiB can't go on MBR"));
    recover::Layout odd = gptBefore;
    odd.parts[2].type = QStringLiteral("6a898cc3-1dd2-11b2-99a6-080020736631"); // a ZFS partition
    report(!tableconvert::toMbr(odd, size, 512).refusal.isEmpty(), QStringLiteral("convert: a kind MBR has no match for is refused"));
    recover::Layout early = before;
    early.parts[0].start = 512 * 2; // sector 2, inside GPT's list
    early.parts[0].size -= 1024;
    report(!tableconvert::toGpt(early, size, 512, false).refusal.isEmpty(), QStringLiteral("convert: a partition where GPT's table goes is refused"));
}

void recoverTests()
{
    converting();
    probeSizes();
    probeBcachefs();
    gptRecovery(512);
    gptRecovery(4096);
    mbrRecovery();
    remembering();
    refusals();
    scanning();
}
