// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// DiskForge's own FAT32 formatter, checked by dosfstools' fsck.fat and mtools: small, past the
// 32 GB Windows stops at, and on a drive with 4096-byte sectors.

#include "testkit.h"

#include "../src/drives/blockio.h"
#include "../src/drives/fat32format.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>

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
        note.write("written onto DiskForge's own FAT32\n");
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
