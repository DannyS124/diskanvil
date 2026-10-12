// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// DiskAnvil's own FAT32 formatter, checked by dosfstools' fsck.fat and mtools: small, past the
// 32 GB Windows stops at, and on a drive with 4096-byte sectors.

#include "testkit.h"

#include "../src/drives/blockio.h"
#include "../src/drives/exfatformat.h"
#include "../src/drives/fat32format.h"
#include "../src/drives/sdcard.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>

#include <functional>

#include <unistd.h>

namespace {

QString run(const QString &program, const QStringList &args, int *code)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(program, args);
    p.waitForFinished(120000);
    *code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    return QString::fromLocal8Bit(p.readAll());
}

// A sparse image of `size`, formatted from `offset` (as if a partition started there).
bool formatImage(const QString &path, quint64 size, quint64 offset, const fat32::Options &o, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite | QIODevice::Truncate) || !f.resize(qint64(offset + size))) {
        *error = f.errorString();
        return false;
    }
    f.close();
    const int fd = blockio::openFile(path, blockio::FileMode::Update);
    const bool ok = fd >= 0 && fat32::format(fd, offset, size, o, error);
    if (fd >= 0)
        ::close(fd);
    return ok;
}

} // namespace

void fat32Tests()
{
    if (QStandardPaths::findExecutable(QStringLiteral("fsck.fat")).isEmpty() || QStandardPaths::findExecutable(QStringLiteral("mdir")).isEmpty()) {
        out << "SKIP  FAT32: needs dosfstools and mtools" << Qt::endl;
        return;
    }
    QTemporaryDir dir;
    constexpr quint64 MiB = 1024 * 1024;
    QString error;
    int code = 0;

    // 600 MB at 1 MiB into its drive: fsck finds nothing wrong, mtools reads the label and
    // a file written onto it.
    const QString small = dir.filePath(QStringLiteral("small.img"));
    fat32::Options o;
    o.label = QStringLiteral("Test stick");
    o.hiddenSectors = 2048;
    report(formatImage(small, 600 * MiB, 0, o, &error), QStringLiteral("FAT32: a 600 MB volume is formatted"), error);
    QString said = run(QStringLiteral("fsck.fat"), {QStringLiteral("-n"), QStringLiteral("-v"), small}, &code);
    report(code == 0 && said.contains(QLatin1String("4096 bytes per cluster")), QStringLiteral("FAT32: fsck.fat finds nothing wrong with it"), said.right(300));
    said = run(QStringLiteral("mdir"), {QStringLiteral("-i"), small, QStringLiteral("::/")}, &code);
    report(code == 0 && said.contains(QLatin1String("TEST STICK")), QStringLiteral("FAT32: its label is read back, in capitals"), said.left(200));
    QFile note(dir.filePath(QStringLiteral("note.txt")));
    if (note.open(QIODevice::WriteOnly)) {
        note.write("written onto DiskAnvil's own FAT32\n");
        note.close();
    }
    run(QStringLiteral("mcopy"), {QStringLiteral("-i"), small, note.fileName(), QStringLiteral("::/note.txt")}, &code);
    said = run(QStringLiteral("mtype"), {QStringLiteral("-i"), small, QStringLiteral("::/note.txt")}, &code);
    int fsck = 0;
    run(QStringLiteral("fsck.fat"), {QStringLiteral("-n"), small}, &fsck);
    report(code == 0 && said.contains(QLatin1String("written onto")) && fsck == 0, QStringLiteral("FAT32: a file goes on and comes back, and it still checks out"), said.left(100));

    // 64 GB, past what Windows formats as FAT32: 32 KiB clusters, as Windows would choose.
    const QString big = dir.filePath(QStringLiteral("big.img"));
    report(formatImage(big, 64ull * 1024 * MiB, 0, fat32::Options(), &error), QStringLiteral("FAT32: a 64 GB volume is formatted"), error);
    said = run(QStringLiteral("fsck.fat"), {QStringLiteral("-n"), QStringLiteral("-v"), big}, &code);
    report(code == 0 && said.contains(QLatin1String("32768 bytes per cluster")), QStringLiteral("FAT32: the 64 GB one checks out, with 32 KiB clusters"), said.right(300));

    // A drive with 4096-byte sectors.
    const QString wide = dir.filePath(QStringLiteral("4k.img"));
    fat32::Options w;
    w.sectorSize = 4096;
    w.label = QStringLiteral("FOURK");
    report(formatImage(wide, 1024 * MiB, 0, w, &error), QStringLiteral("FAT32: a volume on 4096-byte sectors is formatted"), error);
    said = run(QStringLiteral("fsck.fat"), {QStringLiteral("-n"), QStringLiteral("-v"), wide}, &code);
    report(code == 0 && said.contains(QLatin1String("4096 bytes per logical sector")), QStringLiteral("FAT32: the 4096-byte-sector one checks out"), said.right(300));

    // Too small to be FAT32 at all.
    report(!formatImage(dir.filePath(QStringLiteral("tiny.img")), 20 * MiB, 0, fat32::Options(), &error) && error.contains(QLatin1String("too small")),
           QStringLiteral("FAT32: a volume too small for it is turned down"), error);
    report(fat32::clusterSize(8ull * 1024 * MiB) == 4096 && fat32::clusterSize(20ull * 1024 * MiB) == 16384 && fat32::clusterSize(1024ull * 1024 * MiB) == 32768,
           QStringLiteral("FAT32: cluster sizes are Windows' own"));
}

// DiskAnvil's own exFAT, and memory cards laid out the SD Card Association's way.
void sdcardTests()
{
    if (QStandardPaths::findExecutable(QStringLiteral("fsck.exfat")).isEmpty() || QStandardPaths::findExecutable(QStringLiteral("fsck.fat")).isEmpty()
        || QStandardPaths::findExecutable(QStringLiteral("mdir")).isEmpty()) {
        out << "SKIP  SD cards: needs exfatprogs, dosfstools and mtools" << Qt::endl;
        return;
    }
    QTemporaryDir dir;
    constexpr quint64 MiB = 1024 * 1024, GiB = 1024 * MiB;
    QString error;
    int code = 0;
    const auto sparse = [](const QString &path, quint64 size) {
        QFile f(path);
        return f.open(QIODevice::ReadWrite | QIODevice::Truncate) && f.resize(qint64(size));
    };
    const auto withFd = [](const QString &path, const std::function<bool(int)> &work) {
        const int fd = blockio::openFile(path, blockio::FileMode::Update);
        const bool ok = fd >= 0 && work(fd);
        if (fd >= 0)
            ::close(fd);
        return ok;
    };
    const auto readAt = [](const QString &path, quint64 at, int n) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) && f.seek(qint64(at)) ? f.read(n) : QByteArray();
    };

    // exFAT on its own: fsck.exfat finds nothing wrong, dump.exfat reads the label and layout.
    for (const quint64 size : {600 * MiB, 40 * GiB}) {
        const QString image = dir.filePath(QStringLiteral("exfat-%1.img").arg(size / MiB));
        exfat::Options o;
        o.label = QStringLiteral("Café card");
        const bool made = sparse(image, size) && withFd(image, [&](int fd) { return exfat::format(fd, 0, size, o, &error); });
        const QString said = made ? run(QStringLiteral("fsck.exfat"), {QStringLiteral("-n"), QStringLiteral("-v"), image}, &code) : error;
        report(made && code == 0 && said.contains(QLatin1String("clean")), QStringLiteral("exFAT: a %1 MB volume is formatted, and fsck.exfat finds it clean").arg(size / MiB),
               said.right(300));
        const QString dump = run(QStringLiteral("dump.exfat"), {image}, &code);
        report(dump.contains(QStringLiteral("Café card")) && dump.contains(size > GiB ? QLatin1String("131072") : QLatin1String("32768")),
               QStringLiteral("exFAT: dump.exfat reads its label and Windows' cluster size"), dump.left(900));
    }
    // The up-case table: a's capital is A, é's is É, and a long run of characters without one is
    // counted rather than listed.
    // Unpacked the way Linux's exfat driver reads it.
    const QByteArray upcase = exfat::upcaseTable();
    QVector<quint16> upper(0x10000);
    for (quint32 i = 0; i < 0x10000; ++i)
        upper[i] = quint16(i);
    quint32 index = 0;
    bool skip = false;
    for (qsizetype at = 0; at + 1 < upcase.size() && index <= 0xFFFF; at += 2) {
        const quint16 v = qFromLittleEndian<quint16>(upcase.constData() + at);
        if (skip) {
            index += v;
            skip = false;
        } else if (v == index) {
            ++index;
        } else if (v == 0xFFFF) {
            skip = true;
        } else {
            upper[index++] = v;
        }
    }
    report(index == 0x10000 && upper[0x61] == 0x41 && upper[0x41] == 0x41 && upper[0xE9] == 0xC9 && upper[0x3B1] == 0x391 && upper[0x4E00] == 0x4E00
               && upcase.size() < 8000 && exfat::checksum(reinterpret_cast<const uchar *>("ab"), 2) == 0x80000092,
           QStringLiteral("exFAT: the up-case table is compressed, and the checksum is the specification's"), QString::number(upcase.size()));

    // The SD table: file system, cluster and boundary for each size, and where the partition is.
    const sdcard::Plan p128 = sdcard::plan(122 * MiB), p2 = sdcard::plan(1900 * MiB), p16 = sdcard::plan(15 * GiB), p64 = sdcard::plan(59 * GiB),
                       p256 = sdcard::plan(238 * GiB), p1t = sdcard::plan(950 * GiB);
    report(p128.fs == sdcard::Plan::Fs::Fat16 && p128.cluster == 16384 && p128.boundary == 32 * 1024 && p2.fs == sdcard::Plan::Fs::Fat16 && p2.cluster == 32768
               && p16.fs == sdcard::Plan::Fs::Fat32 && p16.cluster == 32768 && p16.boundary == 4 * MiB && p16.partitionStart == 4 * MiB && p16.mbrType == 0x0C
               && p64.fs == sdcard::Plan::Fs::Exfat && p64.cluster == 128 * 1024 && p64.boundary == 16 * MiB && p64.partitionStart == 16 * MiB
               && p256.cluster == 256 * 1024 && p256.boundary == 32 * MiB && p1t.cluster == 512 * 1024 && p1t.boundary == 64 * MiB,
           QStringLiteral("SD cards: the SD Card Association's file system, cluster size and boundary for each size"));
    report(!sdcard::plan(30 * MiB).problem.isEmpty() && !sdcard::plan(3000 * GiB).problem.isEmpty() && !sdcard::plan(16 * GiB, 4096).problem.isEmpty(),
           QStringLiteral("SD cards: what the table doesn't cover is turned down, saying why"));

    // Whole cards: the MBR's one partition, and each file system's data on a boundary.
    struct Card {
        quint64 size;
        sdcard::Plan::Fs fs;
    };
    for (const Card &card : {Card{1900 * MiB, sdcard::Plan::Fs::Fat16}, Card{15 * GiB, sdcard::Plan::Fs::Fat32}, Card{59 * GiB, sdcard::Plan::Fs::Exfat}}) {
        const sdcard::Plan plan = sdcard::plan(card.size);
        const QString image = dir.filePath(QStringLiteral("card-%1.img").arg(card.size / MiB));
        const bool made = sparse(image, card.size) && withFd(image, [&](int fd) { return sdcard::write(fd, card.size, 512, QStringLiteral("CAMERA"), &error); });
        const QByteArray mbr = readAt(image, 0, 512);
        const QByteArray vbr = readAt(image, plan.partitionStart, 512);
        const quint32 start = qFromLittleEndian<quint32>(mbr.constData() + 454), length = qFromLittleEndian<quint32>(mbr.constData() + 458);
        quint64 dataAt = 0;
        if (card.fs == sdcard::Plan::Fs::Exfat) {
            dataAt = plan.partitionStart + quint64(qFromLittleEndian<quint32>(vbr.constData() + 88)) * 512;
        } else {
            const quint64 reserved = qFromLittleEndian<quint16>(vbr.constData() + 14), fats = quint8(vbr[16]), rootEntries = qFromLittleEndian<quint16>(vbr.constData() + 17);
            const quint64 fatSize = qFromLittleEndian<quint16>(vbr.constData() + 22) ? qFromLittleEndian<quint16>(vbr.constData() + 22) : qFromLittleEndian<quint32>(vbr.constData() + 36);
            dataAt = plan.partitionStart + (reserved + fats * fatSize + rootEntries * 32 / 512) * 512;
        }
        report(made && mbr.size() == 512 && quint8(mbr[450]) == plan.mbrType && quint64(start) * 512 == plan.partitionStart && quint64(length) * 512 == plan.partitionSize
                   && dataAt % plan.boundary == 0,
               QStringLiteral("SD cards: a %1 GB card gets %2 with its data on a %3 KiB boundary").arg(double(card.size) / GiB, 0, 'f', 1).arg(plan.fsName()).arg(plan.boundary / 1024),
               error + QStringLiteral(" start %1 data at %2").arg(start).arg(dataAt));
        if (card.fs != sdcard::Plan::Fs::Exfat) {
            const QString said = run(QStringLiteral("mdir"), {QStringLiteral("-i"), image + QStringLiteral("@@%1").arg(plan.partitionStart), QStringLiteral("::/")}, &code);
            report(code == 0 && said.contains(QLatin1String("CAMERA")), QStringLiteral("SD cards: mtools reads the %1 card's volume").arg(plan.fsName()), said.left(200));
        }
    }
    // FAT16 on its own, for fsck.fat (which can't start partway into an image).
    const QString fat16 = dir.filePath(QStringLiteral("fat16.img"));
    fat32::Options o16;
    o16.label = QStringLiteral("OLD CARD");
    o16.clusterBytes = 32768;
    o16.hiddenSectors = 97;
    const bool made16 = sparse(fat16, 1900 * MiB) && withFd(fat16, [&](int fd) { return fat32::formatFat16(fd, 0, 1900 * MiB, o16, &error); });
    QString said = made16 ? run(QStringLiteral("fsck.fat"), {QStringLiteral("-n"), QStringLiteral("-v"), fat16}, &code) : error;
    report(made16 && code == 0 && said.contains(QLatin1String("32768 bytes per cluster")) && said.contains(QLatin1String("16 bit entries")),
           QStringLiteral("FAT16: fsck.fat finds a 1.9 GB card's volume clean"), said.right(400));
    QFile note(dir.filePath(QStringLiteral("photo.txt")));
    if (note.open(QIODevice::WriteOnly)) {
        note.write("a file on DiskAnvil's FAT16\n");
        note.close();
    }
    run(QStringLiteral("mcopy"), {QStringLiteral("-i"), fat16, note.fileName(), QStringLiteral("::/photo.txt")}, &code);
    said = run(QStringLiteral("mtype"), {QStringLiteral("-i"), fat16, QStringLiteral("::/photo.txt")}, &code);
    int fsck = 0;
    run(QStringLiteral("fsck.fat"), {QStringLiteral("-n"), fat16}, &fsck);
    report(code == 0 && said.contains(QLatin1String("DiskAnvil's FAT16")) && fsck == 0, QStringLiteral("FAT16: a file goes on and comes back, and it still checks out"));
}
