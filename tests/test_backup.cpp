// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Back up and restore through the real jobs, on plain files (no root needed).

#include "testkit.h"

#include "../src/backup/ewf.h"
#include "../src/backup/imagebackup.h"
#include "../src/backup/partclone.h"
#include "../src/backup/zstdseek.h"
#include "../src/drives/blockio.h"
#include "../src/lostfiles/vdisk.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr qint64 MiB = 1024 * 1024;

int openFile(const QString &path, int flags)
{
    return ::open(QFile::encodeName(path).constData(), flags | O_CLOEXEC, 0644);
}

struct Outcome {
    bool ok = false;
    bool wrote = false;
    QString message;
};

Outcome backup(const QString &disk, const QString &image, const BackupInfo &info, bool cancel = false)
{
    Outcome o;
    BackupJob job(openFile(disk, O_RDONLY), image, info);
    QObject::connect(&job, &BackupJob::progress, [&](const QString &, quint64, quint64) {
        if (cancel)
            job.cancel();
    });
    QObject::connect(&job, &BackupJob::finished, [&](bool ok, const QString &m) { o = {ok, true, m}; });
    job.run();
    return o;
}

Outcome restore(const QString &image, const QString &target, bool checkFirst = true)
{
    Outcome o;
    RestoreJob job(image, imagebackup::describe(image), openFile(target, O_RDWR), checkFirst, true);
    QObject::connect(&job, &RestoreJob::finished, [&](bool ok, const QString &m, bool wrote) { o = {ok, wrote, m}; });
    job.run();
    return o;
}

void makeFile(const QString &path, qint64 size, char fill)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QByteArray(size, fill));
}

} // namespace

void backupTests()
{
    QTemporaryDir dir;
    auto path = [&](const char *name) { return dir.filePath(QLatin1String(name)); };

    // A 32 MiB GPT disk with two partitions full of data.
    const QString disk = path("disk.img");
    makeFile(disk, 32 * MiB, '\0');
    int code = -1;
    sh(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("printf 'label: gpt\\nsize=10MiB\\nsize=12MiB\\n' | sfdisk -q '%1'").arg(disk)}, &code);
    {
        QFile f(disk);
        if (f.open(QIODevice::ReadWrite)) {
            QByteArray data(20 * MiB, Qt::Uninitialized);
            QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(data.data()), int(data.size() / 4));
            f.seek(1 * MiB);
            f.write(data);
        }
    }
    report(code == 0, QStringLiteral("make a test disk"));

    Disk source;
    source.device = QStringLiteral("/dev/sdz");
    source.model = QStringLiteral("Test Disk");
    source.size = 32 * MiB;
    source.tableType = QStringLiteral("gpt");
    const QString image = path("disk.img.zst");
    Outcome o = backup(disk, image, imagebackup::infoFor(source, nullptr));
    report(o.ok && QFile::exists(imagebackup::infoPath(image)), QStringLiteral("back up a disk"), o.message);

    const BackupInfo info = imagebackup::describe(image);
    report(info.error.isEmpty() && info.size == quint64(32 * MiB) && info.kind == QLatin1String("disk") && info.model == QLatin1String("Test Disk"),
           QStringLiteral("the description reads back"), info.error);
    report(info.sha256 == sha256File(disk), QStringLiteral("its checksum matches the disk"));
    report(info.fileSha256 == sha256File(image), QStringLiteral("its file checksum matches sha256sum"));

    // Seekable (0.5.2 on): 8 MiB frames and their table, which the zstd program reads like any
    // other zstd file, and which Find Lost Files reads a piece at a time.
    {
        QFile f(image);
        const bool opened = f.open(QIODevice::ReadOnly);
        const auto table = zstdseek::readTable(quint64(f.size()), [&f](quint64 offset, char *buf, quint64 len) {
            return f.seek(qint64(offset)) && f.read(buf, qint64(len)) == qint64(len);
        });
        report(opened && table && table->size() == 4 && table->first().size == 8 * MiB && table->last().offset == 24 * MiB,
               QStringLiteral("the backup is seekable: four 8 MiB frames, and their table"), table ? QString::number(table->size()) : QStringLiteral("no table"));
    }
    if (!QStandardPaths::findExecutable(QStringLiteral("zstd")).isEmpty()) {
        const QString unpacked = sh(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("zstd -q -d -c '%1' | sha256sum").arg(image)}).left(64);
        report(unpacked == sha256File(disk), QStringLiteral("the zstd program unpacks it to the same bytes"), unpacked);
    }
    {
        QString problem;
        const auto opened = vdisk::open(image, &problem);
        bool same = opened && opened->size() == quint64(32 * MiB);
        QFile raw(disk);
        same = same && raw.open(QIODevice::ReadOnly);
        for (const quint64 at : {quint64(0), quint64(5 * MiB + 123), quint64(8 * MiB - 700), quint64(31 * MiB + 1)}) {
            QByteArray mine(4096, '\0');
            const quint64 n = opened ? opened->read(at, mine.data(), 4096) : 0;
            raw.seek(qint64(at));
            same = same && raw.read(qint64(n)) == mine.left(qsizetype(n)) && n > 0;
        }
        report(same && vdisk::formatOf(image) == QLatin1String("ZSTD"), QStringLiteral("Find Lost Files reads the backup anywhere, across frames too"), problem);
    }
    if (!QStandardPaths::findExecutable(QStringLiteral("zstd")).isEmpty()) {
        const QString old = path("old.img.zst");
        sh(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("zstd -q -c '%1' > '%2'").arg(disk, old)});
        QString problem;
        report(!vdisk::open(old, &problem) && problem.contains(QLatin1String("before DiskForge 0.5.2")) && imagebackup::describe(old).size == quint64(32 * MiB),
               QStringLiteral("an older one-piece backup: its size still read, and Find Lost Files says to unpack it"), problem);
    }

    // Restore onto a bigger disk.
    const QString target = path("target.img");
    makeFile(target, 48 * MiB, char(0x55));
    o = restore(image, target);
    report(o.ok, QStringLiteral("restore onto a bigger disk"), o.message);
    report(sha256File(target, MiB, 22 * MiB) == sha256File(disk, MiB, 22 * MiB), QStringLiteral("the partitions come back exactly"));
    const QString verify = sh(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("sfdisk --verify '%1' 2>&1").arg(target)});
    report(verify.contains(QLatin1String("No errors detected")) && !verify.contains(QLatin1String("not on the end")),
           QStringLiteral("the partition table fits the bigger disk"), verify.contains(QLatin1String("No errors")) ? QString() : verify);

    // One flipped byte in the middle of the backup: caught before anything is written.
    const QString damaged = path("damaged.img.zst");
    QFile::copy(image, damaged);
    QFile::copy(imagebackup::infoPath(image), imagebackup::infoPath(damaged));
    {
        QFile f(damaged);
        if (f.open(QIODevice::ReadWrite)) {
            f.seek(f.size() / 2);
            const char c = f.peek(1).at(0);
            f.write(QByteArray(1, char(c ^ 0x10)));
        }
    }
    makeFile(target, 48 * MiB, char(0x55));
    const QString untouched = sha256File(target);
    o = restore(damaged, target);
    report(!o.ok && !o.wrote && sha256File(target) == untouched, QStringLiteral("a damaged backup is refused and nothing is written"), o.message);

    // A backup whose description says a different checksum (swapped or edited files).
    const QString swapped = path("swapped.img.zst");
    QFile::copy(image, swapped);
    {
        BackupInfo edited = info;
        edited.sha256 = QString(64, QLatin1Char('0'));
        QFile json(imagebackup::infoPath(swapped));
        if (json.open(QIODevice::WriteOnly))
            json.write(QJsonDocument(edited.toJson()).toJson());
    }
    o = restore(swapped, target);
    report(!o.ok && !o.wrote && sha256File(target) == untouched, QStringLiteral("a checksum mismatch is refused and nothing is written"), o.message);

    // Too small a target.
    const QString small = path("small.img");
    makeFile(small, 16 * MiB, '\0');
    o = restore(image, small);
    report(!o.ok && !o.wrote, QStringLiteral("a target smaller than the backup is refused"), o.message);

    // Without its description: the size comes from the zstd header, the zstd checksum still guards it.
    const QString bare = path("bare.img.zst");
    QFile::copy(image, bare);
    const BackupInfo bareInfo = imagebackup::describe(bare);
    report(bareInfo.error.isEmpty() && bareInfo.size == quint64(32 * MiB), QStringLiteral("a backup without its description still knows its size"), bareInfo.error);

    // Stopping a backup leaves nothing behind.
    const QString stopped = path("stopped.img.zst");
    o = backup(disk, stopped, imagebackup::infoFor(source, nullptr), true);
    report(!o.ok && !QFile::exists(stopped) && !QFile::exists(imagebackup::infoPath(stopped)), QStringLiteral("a stopped backup is cleaned up"), o.message);
}

// Clonezilla's partclone images (tests/data/clonezilla/make.sh): each restores to exactly the
// file system it was made from, gzip parts, zstd and checksum-less alike; a damaged one stops.
void clonezillaTests()
{
    const QString folder = QStringLiteral(SOURCE_DIR "/tests/data/clonezilla/image");
    const QVector<partclone::Image> images = partclone::scanFolder(folder);
    QStringList found;
    for (const partclone::Image &i : images)
        found << QStringLiteral("%1 %2 %3 parts %4").arg(i.partition, i.fs, i.header.fs).arg(i.files.size());
    report(images.size() == 3 && images[0].partition == QLatin1String("sda1") && images[0].files.size() == 2 && images[0].header.fs == QLatin1String("EXTFS")
               && images[1].header.fs == QLatin1String("FAT32") && images[2].header.checksumMode == 0 && images[0].header.deviceSize == 8 * MiB,
           QStringLiteral("clonezilla: its folder's images, their parts, and what partclone says of each"), found.join(QStringLiteral(" | ")));
    QHash<QString, QString> expected;
    QFile sums(QStringLiteral(SOURCE_DIR "/tests/data/clonezilla/sha256.txt"));
    if (sums.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : sums.readAll().split('\n')) {
            const QList<QByteArray> f = line.simplified().split(' ');
            if (f.size() == 2)
                expected.insert(QString::fromLatin1(f[1]), QString::fromLatin1(f[0]));
        }
    }
    QTemporaryDir dir;
    const auto restore = [&dir](const partclone::Image &image, quint64 size, QString *where) {
        *where = dir.filePath(image.partition + QStringLiteral(".img"));
        makeFile(*where, qint64(size), '\0');
        partclone::Result result;
        partclone::Restore job(image, blockio::openFile(*where, blockio::FileMode::Update), size);
        QObject::connect(&job, &partclone::Restore::finished, [&result](const partclone::Result &r) { result = r; });
        job.run();
        return result;
    };
    QString path;
    for (int i = 0; i < images.size(); ++i) {
        const partclone::Image &image = images[i];
        const partclone::Result r = restore(image, image.header.deviceSize, &path);
        const QString want = expected.value(image.header.fs == QLatin1String("FAT32") ? QStringLiteral("fat.img") : QStringLiteral("ext4.img"));
        report(r.ok && sha256File(path) == want, QStringLiteral("clonezilla: %1 (%2) restores to exactly the file system it was").arg(image.partition, image.fs), r.error);
    }
    // A flipped byte in a block: its group's CRC catches it.
    partclone::Image damaged = images[2];
    {
        // The checksummed ext4 (sda1), unpacked, one byte changed past the header and map.
        const QString packed = dir.filePath(QStringLiteral("whole.gz"));
        sh(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("cat '%1'/sda1.ext4-ptcl-img.gz.a? | gzip -dc > '%2.ptcl'").arg(folder, packed)});
        QFile f(packed + QStringLiteral(".ptcl"));
        if (f.open(QIODevice::ReadWrite)) {
            f.seek(f.size() / 2);
            const char c = f.peek(1).at(0);
            f.write(QByteArray(1, char(c ^ 0x5A)));
        }
        damaged.files = {packed + QStringLiteral(".ptcl")};
        damaged.partition = QStringLiteral("damaged");
    }
    const partclone::Result broken = restore(damaged, 8 * MiB, &path);
    report(!broken.ok && broken.error.contains(QLatin1String("damaged")), QStringLiteral("clonezilla: a damaged block is caught by its checksum"), broken.error);
    const partclone::Result small = restore(images[0], 4 * MiB, &path);
    report(!small.ok && small.error.contains(QLatin1String("smaller")), QStringLiteral("clonezilla: a partition too small for it is turned down"), small.error);
}

// Evidence files (.E01): ours read back the same and keep the case; libewf's own (tests/data/ewf,
// made by ewfacquire) reads as the data it was made from; and when there, ewfverify agrees.
void evidenceTests()
{
    QTemporaryDir dir;
    const QString raw = dir.filePath(QStringLiteral("drive.img"));
    {
        QFile f(raw);
        if (f.open(QIODevice::WriteOnly)) {
            QByteArray random(300 * 1024, Qt::Uninitialized);
            QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(random.data()), int(random.size() / 4));
            f.write(random);
            f.write(QByteArray(200 * 1024, '\0'));
            f.write(QByteArray("DiskForge").repeated(5000));
            f.resize(1024 * 1024);
        }
    }
    const QString e01 = dir.filePath(QStringLiteral("evidence.E01"));
    ewf::Result result;
    {
        ewf::Case details{QStringLiteral("CASE7"), QStringLiteral("EV-12"), QStringLiteral("A test drive"), QStringLiteral("Sam Tester"),
                          QStringLiteral("Bagged\tat 10:02"), QStringLiteral("WDC WD10EZEX"), QStringLiteral("WD-TEST123")};
        ewf::Acquire job(blockio::openFile(raw, blockio::FileMode::Read), 1024 * 1024, 512, e01, details);
        QObject::connect(&job, &ewf::Acquire::finished, [&result](const ewf::Result &r) { result = r; });
        job.run();
    }
    QFile rawFile(raw);
    const QByteArray original = rawFile.open(QIODevice::ReadOnly) ? rawFile.readAll() : QByteArray();
    report(result.ok && result.md5 == QCryptographicHash::hash(original, QCryptographicHash::Md5) && result.sha1 == QCryptographicHash::hash(original, QCryptographicHash::Sha1)
               && result.unreadable.isEmpty() && result.fileSize < 600 * 1024,
           QStringLiteral("evidence: an E01 is made, its MD5 and SHA-1 the drive's, packed smaller"), result.error);
    ewf::Info info;
    const auto layer = ewf::open(e01, &info);
    QByteArray back(qsizetype(info.size), '\0');
    report(layer && info.size == 1024 * 1024 && layer->read(0, back.data(), info.size) && back == original && info.caseNumber == QLatin1String("CASE7")
               && info.notes == QLatin1String("Bagged at 10:02") && info.serial == QLatin1String("WD-TEST123") && ewf::verify(e01).isEmpty(),
           QStringLiteral("evidence: it reads back exactly, with its case (a tab in a note made a space), and checks out"), info.problem);
    // A changed byte inside: the check catches it.
    {
        QFile f(e01);
        if (f.open(QIODevice::ReadWrite)) {
            f.seek(f.size() / 2);
            const char c = f.peek(1).at(0);
            f.write(QByteArray(1, char(c ^ 0x10)));
        }
    }
    report(!ewf::verify(e01).isEmpty(), QStringLiteral("evidence: a changed byte inside is caught"), ewf::verify(e01));

    const QString ref = QStringLiteral(SOURCE_DIR "/tests/data/ewf/ref.E01");
    QFile sum(QStringLiteral(SOURCE_DIR "/tests/data/ewf/ref.sha256"));
    const QByteArray want = sum.open(QIODevice::ReadOnly) ? sum.readAll().trimmed() : QByteArray();
    ewf::Info refInfo;
    const auto refLayer = ewf::open(ref, &refInfo);
    QByteArray refData(qsizetype(refInfo.size), '\0');
    const bool read = refLayer && refLayer->read(0, refData.data(), refInfo.size);
    report(read && QCryptographicHash::hash(refData, QCryptographicHash::Sha256).toHex() == want && refInfo.caseNumber == QLatin1String("REF-CASE")
               && ewf::verify(ref).isEmpty() && vdisk::formatOf(ref) == QLatin1String("E01"),
           QStringLiteral("evidence: libewf's own E01 reads as the data it was made from"), refInfo.problem);
    if (!QStandardPaths::findExecutable(QStringLiteral("ewfverify")).isEmpty()) {
        QFile::remove(e01);
        ewf::Acquire job(blockio::openFile(raw, blockio::FileMode::Read), 1024 * 1024, 512, e01, ewf::Case{});
        job.run();
        const QString said = sh(QStringLiteral("ewfverify"), {QStringLiteral("-q"), e01});
        report(said.contains(QLatin1String("SUCCESS")), QStringLiteral("evidence: libewf's ewfverify agrees with ours"), said.right(200));
    }
}
