// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Make a DiskForge Live USB without a USB stick: real ISOs made by xorriso from a test folder, read
// back with isofs, then copied into a plain folder (standing in for the mounted stick) by
// the same StickWriter the dialog uses. Damaged images and random corruption on top.

#include "testkit.h"

#include "../src/usb/filecopy.h"
#include "../src/usb/isofs.h"
#include "../src/usb/rescuestick.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(data);
}

QByteArray randomBytes(qsizetype size, quint32 seed)
{
    QRandomGenerator rng(seed);
    QByteArray data(size, Qt::Uninitialized);
    for (qsizetype i = 0; i < size; ++i)
        data[i] = char(rng.bounded(256));
    return data;
}

QByteArray sha(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

// The files of a small rescue image: path -> contents.
QMap<QString, QByteArray> sampleFiles()
{
    QMap<QString, QByteArray> files;
    files[QStringLiteral(".disk/diskforge-live")] = "DiskForge Live\nversion=9.8.7\nbuilt=2026-10-09\nid=1234-abcd\n";
    files[QStringLiteral(".disk/info")] = "DiskForge Live 9.8.7\n";
    files[QStringLiteral("README.txt")] = "read me\n";
    files[QStringLiteral("live/vmlinuz")] = randomBytes(3 * 1024 * 1024 + 17, 1);
    files[QStringLiteral("live/filesystem.squashfs")] = randomBytes(4 * 1024 * 1024, 2); // one copy chunk exactly
    files[QStringLiteral("EFI/BOOT/BOOTX64.EFI")] = randomBytes(100 * 1024, 3);
    files[QStringLiteral("EFI/BOOT/grubx64.efi")] = randomBytes(2048, 4); // one sector exactly
    files[QStringLiteral("boot/grub/grub.cfg")] = "menuentry \"x\" {}\n";
    files[QStringLiteral("boot/empty")] = QByteArray();
    files[QStringLiteral("A folder with spaces/a rather long file name, with Ünïcödé and more than sixty-four characters.txt")] = "long\n";
    QString deep;
    for (int i = 0; i < 10; ++i)
        deep += QStringLiteral("level%1/").arg(i);
    files[deep + QStringLiteral("bottom.txt")] = "deep\n";
    return files;
}

QByteArray sumsFor(const QMap<QString, QByteArray> &files)
{
    QByteArray text;
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        text += sha(it.value()) + "  ./" + it.key().toUtf8() + "\n";
    return text;
}

// Makes an ISO of `files` (plus sha256sum.txt unless `sums` is empty). Empty on failure.
QString makeIso(const QString &dir, const QString &name, const QMap<QString, QByteArray> &files, const QByteArray &sums,
                bool joliet = true)
{
    const QString tree = dir + QStringLiteral("/tree-") + name;
    QDir(tree).removeRecursively();
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        writeFile(tree + QLatin1Char('/') + it.key(), it.value());
    if (!sums.isEmpty())
        writeFile(tree + QStringLiteral("/sha256sum.txt"), sums);
    const QString iso = dir + QLatin1Char('/') + name + QStringLiteral(".iso");
    // The names are UTF-8 whatever the locale says (the .deb is built and tested with LANG=C,
    // where xorriso would turn them into something else for Joliet).
    QStringList args = {QStringLiteral("-as"), QStringLiteral("mkisofs"), QStringLiteral("-quiet"), QStringLiteral("-input-charset"),
                        QStringLiteral("UTF-8"), QStringLiteral("-iso-level"), QStringLiteral("3"), QStringLiteral("-R"),
                        QStringLiteral("-V"), QStringLiteral("DISKFORGE_LIVE")};
    if (joliet)
        args << QStringLiteral("-J") << QStringLiteral("-joliet-long");
    args << QStringLiteral("-o") << iso << tree;
    int code = -1;
    sh(QStringLiteral("xorriso"), args, &code);
    return code == 0 ? iso : QString();
}

struct WriteResult {
    bool ok = false;
    QString message;
    quint64 lastDone = 0;
};

WriteResult writeStick(const QString &iso, const QString &stick, bool cancelRightAway = false)
{
    QDir(stick).removeRecursively();
    QDir().mkpath(stick);
    WriteResult result;
    rescue::StickWriter writer(iso, stick);
    QObject::connect(&writer, &rescue::StickWriter::progress, [&](const QString &, quint64 done, quint64) {
        result.lastDone = done;
        if (cancelRightAway)
            writer.cancel();
    });
    QObject::connect(&writer, &rescue::StickWriter::finished, [&](bool ok, const QString &message) {
        result.ok = ok;
        result.message = message;
    });
    writer.run();
    return result;
}

void isoTests(const QString &dir)
{
    const QMap<QString, QByteArray> files = sampleFiles();
    const QByteArray sums = sumsFor(files);
    const QString iso = makeIso(dir, QStringLiteral("good"), files, sums);
    report(!iso.isEmpty(), QStringLiteral("xorriso makes the test image"));
    if (iso.isEmpty())
        return;

    // Listing
    const int fd = ::open(QFile::encodeName(iso).constData(), O_RDONLY | O_CLOEXEC);
    const isofs::Listing listing = isofs::list(fd);
    report(listing.ok(), QStringLiteral("the image is listed"), listing.error);
    report(listing.volumeId == QLatin1String("DISKFORGE_LIVE"), QStringLiteral("with its volume name"), listing.volumeId);
    bool allThere = true, sizesRight = true, contentsRight = true;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        const isofs::Entry *e = isofs::find(listing, it.key());
        if (!e || e->isDir) {
            allThere = false;
            out << "      missing: " << it.key() << Qt::endl;
            continue;
        }
        sizesRight = sizesRight && e->size == quint64(it.value().size());
        QByteArray data(qsizetype(e->size), '\0');
        if (e->size)
            contentsRight = contentsRight && ::pread(fd, data.data(), size_t(data.size()), off_t(e->offset)) == ssize_t(data.size());
        contentsRight = contentsRight && data == it.value();
    }
    report(allThere, QStringLiteral("every file is found, long and non-English names included"));
    report(sizesRight, QStringLiteral("with the right sizes"));
    report(contentsRight, QStringLiteral("and the right contents where it says they are"));
    bool foldersFirst = true;
    for (int i = 0; i < listing.entries.size(); ++i) {
        const QString parent = QFileInfo(listing.entries[i].path).path();
        if (parent == QLatin1String("."))
            continue;
        bool before = false;
        for (int j = 0; j < i; ++j)
            before = before || (listing.entries[j].isDir && listing.entries[j].path == parent);
        foldersFirst = foldersFirst && before;
    }
    report(foldersFirst, QStringLiteral("a folder comes before what's in it"));
    ::close(fd);

    // What the dialog shows
    const rescue::Image image = rescue::inspect(iso);
    report(image.error.isEmpty() && image.info.version == QLatin1String("9.8.7") && image.info.built == QLatin1String("2026-10-09")
               && image.info.id == QLatin1String("1234-abcd"),
           QStringLiteral("the dialog reads the version, date and build"), image.error);

    // Onto the "stick"
    const QString stick = dir + QStringLiteral("/stick");
    WriteResult written = writeStick(iso, stick);
    report(written.ok, QStringLiteral("the files are copied and checked"), written.message);
    bool same = true;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        QFile f(stick + QLatin1Char('/') + it.key());
        same = same && f.open(QIODevice::ReadOnly) && f.readAll() == it.value();
    }
    report(same, QStringLiteral("everything on the stick matches the image"));
    QFile copiedSums(stick + QStringLiteral("/sha256sum.txt"));
    report(copiedSums.open(QIODevice::ReadOnly) && copiedSums.readAll() == sums, QStringLiteral("sha256sum.txt goes along (Check the stick needs it)"));
    report(QFileInfo::exists(stick + QStringLiteral("/logs/README.txt")), QStringLiteral("the logs folder is there, with a note"));
    const rescue::Info info = rescue::stickInfo(stick);
    report(info.valid() && info.version == QLatin1String("9.8.7"), QStringLiteral("the stick is recognized as DiskForge Live"));
    QDir().mkpath(stick + QStringLiteral("/logs/2026-10-09_101500_LENOVO-20XY"));
    QDir().mkpath(stick + QStringLiteral("/logs/2026-10-09_111500_VMware"));
    report(rescue::logFolders(stick) == 2, QStringLiteral("and its logs are counted"), QString::number(rescue::logFolders(stick)));

    // Files the image has but sha256sum.txt doesn't list stay behind (the CD boot catalog).
    QMap<QString, QByteArray> extra = files;
    extra[QStringLiteral("boot/grub/boot.cat")] = "catalog";
    const QString withExtra = makeIso(dir, QStringLiteral("extra"), extra, sums);
    written = writeStick(withExtra, stick);
    report(written.ok && !QFileInfo::exists(stick + QStringLiteral("/boot/grub/boot.cat")),
           QStringLiteral("a file that isn't in sha256sum.txt isn't copied"), written.message);

    // Damaged: one byte of the kernel changed after the checksums were made.
    QMap<QString, QByteArray> damaged = files;
    damaged[QStringLiteral("live/vmlinuz")][123456] = char(damaged[QStringLiteral("live/vmlinuz")][123456] ^ 0x40);
    written = writeStick(makeIso(dir, QStringLiteral("damaged"), damaged, sums), stick);
    report(!written.ok && written.message.contains(QLatin1String("live/vmlinuz")) && written.message.contains(QLatin1String("damaged")),
           QStringLiteral("a damaged image is refused, naming the file"), written.message);

    // Listed but not there.
    QMap<QString, QByteArray> missing = files;
    missing.remove(QStringLiteral("README.txt"));
    written = writeStick(makeIso(dir, QStringLiteral("missing"), missing, sums), stick);
    report(!written.ok && written.message.contains(QLatin1String("README.txt")), QStringLiteral("a missing file is refused, naming it"),
           written.message);

    // Made before the rename: the old info file still counts, on an image and on a stick.
    QMap<QString, QByteArray> old = files;
    old.remove(QStringLiteral(".disk/diskforge-live"));
    old[QStringLiteral(".disk/diskforge-rescue")] = "DiskForge Rescue\nversion=0.5.0\nbuilt=2026-10-09\nid=old-1\n";
    const rescue::Image oldImage = rescue::inspect(makeIso(dir, QStringLiteral("old"), old, sumsFor(old)));
    report(oldImage.error.isEmpty() && oldImage.info.version == QLatin1String("0.5.0"),
           QStringLiteral("an image from before the rename to DiskForge Live is still taken"), oldImage.error);
    const QString oldStick = dir + QStringLiteral("/oldstick");
    writeFile(oldStick + QStringLiteral("/.disk/diskforge-rescue"), old.value(QStringLiteral(".disk/diskforge-rescue")));
    report(rescue::stickInfo(oldStick).id == QLatin1String("old-1"), QStringLiteral("and so is a stick made from one"));
    // And from while it was called Bluespark.
    QMap<QString, QByteArray> spark = files;
    spark.remove(QStringLiteral(".disk/diskforge-live"));
    spark[QStringLiteral(".disk/bluespark")] = "Bluespark\nversion=0.5.1\nbuilt=2026-10-09\nid=spark-1\n";
    const rescue::Image sparkImage = rescue::inspect(makeIso(dir, QStringLiteral("spark"), spark, sumsFor(spark)));
    const QString sparkStick = dir + QStringLiteral("/sparkstick");
    writeFile(sparkStick + QStringLiteral("/.disk/bluespark"), spark.value(QStringLiteral(".disk/bluespark")));
    report(sparkImage.error.isEmpty() && sparkImage.info.id == QLatin1String("spark-1") && rescue::stickInfo(sparkStick).id == QLatin1String("spark-1"),
           QStringLiteral("an image or stick from when it was called Bluespark too"), sparkImage.error);

    // Not a rescue image at all.
    QMap<QString, QByteArray> other = files;
    other.remove(QStringLiteral(".disk/diskforge-live"));
    const rescue::Image notRescue = rescue::inspect(makeIso(dir, QStringLiteral("other"), other, sumsFor(other)));
    report(!notRescue.error.isEmpty() && notRescue.error.contains(QLatin1String("Write Image")),
           QStringLiteral("another ISO is pointed to Write Image to USB"), notRescue.error);
    const rescue::Image noJoliet = rescue::inspect(makeIso(dir, QStringLiteral("nojoliet"), files, sums, false));
    report(noJoliet.error.contains(QLatin1String("Joliet")), QStringLiteral("an ISO without long names says so"), noJoliet.error);
    writeFile(dir + QStringLiteral("/not.iso"), randomBytes(200000, 9));
    report(!rescue::inspect(dir + QStringLiteral("/not.iso")).error.isEmpty(), QStringLiteral("a file that isn't an ISO is refused"));

    // Stop
    written = writeStick(iso, stick, true);
    report(!written.ok && written.message.startsWith(QLatin1String("Stopped")), QStringLiteral("Stop stops it, and says the stick isn't usable"),
           written.message);
}

void parserTests()
{
    const QByteArray hash(64, 'a');
    const QHash<QString, QByteArray> sums = rescue::parseSums(
        hash + "  ./live/vmlinuz\n"
        + QByteArray(64, 'B') + " *./EFI/BOOT/BOOTX64.EFI\n"
        + hash + "  ./../escape\n"
        + hash + "  /absolute\n"
        + QByteArray(63, 'a') + "g  ./not-hex\n"
        + "garbage\n\n"
        + hash + "  boot/no-dot-slash\n");
    report(sums.size() == 3 && sums.value(QStringLiteral("live/vmlinuz")) == hash
               && sums.value(QStringLiteral("EFI/BOOT/BOOTX64.EFI")) == QByteArray(64, 'b')
               && sums.contains(QStringLiteral("boot/no-dot-slash")),
           QStringLiteral("sha256sum.txt: good lines read, \"..\", absolute paths and bad hashes skipped"),
           QStringList(sums.keys()).join(QLatin1Char(' ')));

    report(!rescue::parseInfo("Something Else\nversion=1\nid=2\n").valid(), QStringLiteral("an info file from something else isn't taken"));
    const rescue::Info odd = rescue::parseInfo("DiskForge Live\nversion=1.2<b>3</b>\nid=x\n");
    report(odd.valid() && odd.version == QLatin1String("1.2b3b"), QStringLiteral("only plain characters come out of the info file"), odd.version);
}

// Random damage to a real image: listing must never crash, and anything it lists has to be
// inside the image with a clean path.
void fuzzListing(const QString &dir)
{
    const QString iso = makeIso(dir, QStringLiteral("fuzz"), sampleFiles(), sumsFor(sampleFiles()));
    QFile f(iso);
    if (iso.isEmpty() || !f.open(QIODevice::ReadOnly))
        return report(false, QStringLiteral("fuzz image"));
    const QByteArray original = f.readAll();
    // The directories and volume descriptors are in the first sectors; damage goes there.
    const qsizetype hot = qMin<qsizetype>(original.size(), 64 * 2048);
    QRandomGenerator rng(20261009);
    const int rounds = 3000;
    int listed = 0;
    bool clean = true;
    for (int round = 0; round < rounds && clean; ++round) {
        QByteArray data = original;
        const int hits = 1 + int(rng.bounded(40));
        for (int i = 0; i < hits; ++i)
            data[qsizetype(rng.bounded(quint32(hot)))] = char(rng.bounded(256));
        if (rng.bounded(4) == 0)
            data.truncate(qsizetype(rng.bounded(quint32(data.size()))));
        const int fd = memfd_create("isofuzz", MFD_CLOEXEC);
        if (fd < 0 || ::write(fd, data.constData(), size_t(data.size())) != ssize_t(data.size()))
            return report(false, QStringLiteral("fuzz: memfd"));
        const isofs::Listing listing = isofs::list(fd);
        ::close(fd);
        if (listing.ok())
            ++listed;
        for (const isofs::Entry &e : listing.entries) {
            const bool inside = e.offset <= quint64(data.size()) && e.size <= quint64(data.size()) - e.offset;
            const bool path = !e.path.isEmpty() && !e.path.startsWith(QLatin1Char('/'))
                && !e.path.split(QLatin1Char('/')).contains(QLatin1String("..")) && !e.path.contains(QLatin1Char('\\'));
            if ((!inside && !e.isDir) || !path) {
                clean = false;
                out << "      round " << round << ": " << e.path << " at " << e.offset << " size " << e.size << Qt::endl;
            }
        }
    }
    report(clean, QStringLiteral("%1 damaged images: no crash, nothing outside the image, no bad paths (%2 still listed)").arg(rounds).arg(listed));
}

// The copier on its own, from a folder (a mounted ISO, or a running rescue stick).
void folderTests(const QString &dir)
{
    const QString tree = dir + QStringLiteral("/folder-tree");
    QDir(tree).removeRecursively();
    writeFile(tree + QStringLiteral("/boot/grub/grub.cfg"), "linux /vmlinuz root=live:CDLABEL=Some-Label quiet\n");
    writeFile(tree + QStringLiteral("/live/filesystem.squashfs"), randomBytes(5 * 1024 * 1024 + 3, 11));
    writeFile(tree + QStringLiteral("/skip/me.txt"), "nope\n");
    writeFile(tree + QStringLiteral("/a/b/c/deep.txt"), "deep\n");
    QDir().mkpath(tree + QStringLiteral("/empty"));
    // Debian's ISOs have "debian -> ." at the top: following it would never end.
    QFile::link(QStringLiteral("."), tree + QStringLiteral("/loop"));
    QFile::link(QStringLiteral("live/filesystem.squashfs"), tree + QStringLiteral("/link.squashfs"));

    const std::unique_ptr<filecopy::Source> source = filecopy::openFolder(tree);
    bool noLinks = true;
    for (const filecopy::Entry &e : source->entries())
        noLinks = noLinks && !e.path.startsWith(QLatin1String("loop")) && e.path != QLatin1String("link.squashfs");
    report(source->error().isEmpty() && noLinks, QStringLiteral("a folder is listed without its symbolic links"));

    filecopy::Options options;
    options.skip = [](const QString &path) { return path.startsWith(QLatin1String("skip")); };
    options.transform = [](const QString &path, const QByteArray &data) {
        return path.endsWith(QLatin1String(".cfg")) ? QByteArray(data).replace("Some-Label", "STICK") : QByteArray();
    };
    const QString target = dir + QStringLiteral("/folder-stick");
    QDir(target).removeRecursively();
    QDir().mkpath(target);
    filecopy::Copier copier(source.get(), target, options);
    bool ok = false;
    QString message;
    QObject::connect(&copier, &filecopy::Copier::finished, [&](bool done, const QString &text) {
        ok = done;
        message = text;
    });
    copier.run();
    QFile cfg(target + QStringLiteral("/boot/grub/grub.cfg"));
    QFile big(target + QStringLiteral("/live/filesystem.squashfs"));
    QFile bigSource(tree + QStringLiteral("/live/filesystem.squashfs"));
    report(ok, QStringLiteral("a folder is copied and checked"), message);
    report(cfg.open(QIODevice::ReadOnly) && cfg.readAll() == "linux /vmlinuz root=live:CDLABEL=STICK quiet\n",
           QStringLiteral("a transform changes a file on the way"));
    report(big.open(QIODevice::ReadOnly) && bigSource.open(QIODevice::ReadOnly) && big.readAll() == bigSource.readAll(),
           QStringLiteral("big files come over as they are"));
    report(!QFileInfo::exists(target + QStringLiteral("/skip/me.txt")) && QFileInfo::exists(target + QStringLiteral("/a/b/c/deep.txt"))
               && QFileInfo(target + QStringLiteral("/empty")).isDir(),
           QStringLiteral("skipped files stay behind, deep and empty folders come along"));
}

} // namespace

// The boot code for old BIOS PCs: GRUB's in the first 440 bytes, its core right after, the disk's
// ID and partition table left alone, and refused when it doesn't fit or there's no table.
void biosBootTests(const QString &dir)
{
    const QString image = dir + QStringLiteral("/bios.img");
    QFile f(image);
    if (!f.open(QIODevice::WriteOnly) || !f.resize(8 * 1024 * 1024)) {
        report(false, QStringLiteral("make an image for the boot code"));
        return;
    }
    f.close();
    QProcess sfdisk;
    sfdisk.start(QStringLiteral("sfdisk"), {QStringLiteral("-q"), image});
    sfdisk.waitForStarted();
    sfdisk.write("label: dos\nlabel-id: 0x5eed1234\nstart=2048, type=c, bootable\n");
    sfdisk.closeWriteChannel();
    sfdisk.waitForFinished();
    auto readBack = [&image](qint64 len) {
        QFile in(image);
        return in.open(QIODevice::ReadOnly) ? in.read(len) : QByteArray();
    };
    const QByteArray before = readBack(512);
    report(sfdisk.exitCode() == 0 && uchar(before[510]) == 0x55, QStringLiteral("sfdisk makes a stick image with an MBR"));

    QByteArray boot(512, '\xab'), core(60000, Qt::Uninitialized);
    for (qsizetype i = 0; i < core.size(); ++i)
        core[i] = char(i * 13 + 1);
    const int fd = ::open(QFile::encodeName(image).constData(), O_RDWR | O_CLOEXEC);
    QString error;
    const bool ok = rescue::writeBiosBoot(fd, boot, core, &error);
    const QByteArray after = readBack(512 + core.size());
    report(ok && after.left(440) == boot.left(440) && after.mid(440, 72) == before.mid(440, 72) && after.mid(512) == core,
           QStringLiteral("BIOS boot code: GRUB's code in front, its core behind, the disk ID and partitions as they were"), error);

    QString tooBig, noTable, badBoot, gpt;
    rescue::writeBiosBoot(fd, boot, QByteArray(2 * 1024 * 1024, 'x'), &tooBig);
    rescue::writeBiosBoot(fd, QByteArray(100, 'x'), core, &badBoot);
    ::close(fd);
    QFile blank(dir + QStringLiteral("/blank.img"));
    if (blank.open(QIODevice::ReadWrite) && blank.resize(4 * 1024 * 1024)) {
        rescue::writeBiosBoot(blank.handle(), boot, core, &noTable);
        blank.close();
    }
    // A GPT stick: its sector 1 is the GPT itself.
    QProcess gptTable;
    gptTable.start(QStringLiteral("sfdisk"), {QStringLiteral("-q"), blank.fileName()});
    gptTable.waitForStarted();
    gptTable.write("label: gpt\nstart=2048, type=uefi\n");
    gptTable.closeWriteChannel();
    gptTable.waitForFinished();
    const QByteArray gptBefore = [&blank] { return blank.open(QIODevice::ReadOnly) ? blank.read(1024) : QByteArray(); }();
    blank.close();
    if (blank.open(QIODevice::ReadWrite)) {
        rescue::writeBiosBoot(blank.handle(), boot, core, &gpt);
        blank.close();
    }
    const QByteArray gptAfter = [&blank] { return blank.open(QIODevice::ReadOnly) ? blank.read(1024) : QByteArray(); }();
    report(tooBig.contains(QLatin1String("room")) && noTable.contains(QLatin1String("partition table")) && badBoot.contains(QLatin1String("usable"))
               && gpt.contains(QLatin1String("GPT")) && gptAfter == gptBefore,
           QStringLiteral("refused when it doesn't fit, with no partition table, a GPT, or a broken boot image"),
           QStringList{tooBig, noTable, badBoot, gpt}.join(QStringLiteral(" | ")));
}

// Sleep Now warns when the live system still runs from its stick, not after "Copy to memory".
void fromStickTests(const QString &dir)
{
    const QString realConf = rescue::rescueConfPath, realOptions = rescue::bootOptionsPath;
    rescue::rescueConfPath = dir + QStringLiteral("/live.conf");
    rescue::bootOptionsPath = dir + QStringLiteral("/cmdline");
    writeFile(rescue::bootOptionsPath, "BOOT_IMAGE=/live/vmlinuz boot=live quiet splash\n");
    const bool notLive = !rescue::runningFromStick();
    writeFile(rescue::rescueConfPath, "VERSION=0.5.1\n");
    const bool fromStick = rescue::runningFromStick();
    writeFile(rescue::bootOptionsPath, "BOOT_IMAGE=/live/vmlinuz boot=live toram quiet splash\n");
    const bool inMemory = !rescue::runningFromStick();
    report(notLive && fromStick && inMemory, QStringLiteral("running from the stick: yes when live, no after Copy to memory, no on a PC"));
    rescue::rescueConfPath = realConf;
    rescue::bootOptionsPath = realOptions;
}

void rescueUsbTests()
{
    QTemporaryDir dir;
    folderTests(dir.path());
    fromStickTests(dir.path());
    parserTests();
    biosBootTests(dir.path());
    if (QStandardPaths::findExecutable(QStringLiteral("xorriso")).isEmpty()) {
        out << "SKIP  xorriso isn't installed, so there are no test images" << Qt::endl;
        return;
    }
    isoTests(dir.path());
    fuzzListing(dir.path());
}
