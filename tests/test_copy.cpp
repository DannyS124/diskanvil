// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// The copy engine behind Clone, Back Up and Restore, on plain files (no root needed).

#include "testkit.h"

#include "../src/backup/blockcopy.h"

#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>

#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr qint64 MiB = 1024 * 1024;
using blockcopy::Extent;
using blockcopy::Format;

int openFile(const QString &path, int flags)
{
    return ::open(QFile::encodeName(path).constData(), flags | O_CLOEXEC, 0644);
}

void writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(data);
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

blockcopy::Result run(const QString &from, Format inFormat, const QString &to, Format outFormat, const QVector<Extent> &extents,
                      std::atomic<bool> *cancelAfterFirst = nullptr)
{
    blockcopy::Options o;
    o.sourceFd = openFile(from, O_RDONLY);
    o.sourceFormat = inFormat;
    o.targetFd = to.isEmpty() ? -1 : openFile(to, O_RDWR | O_CREAT);
    o.targetFormat = outFormat;
    o.extents = extents;
    std::atomic<bool> never{false};
    std::atomic<bool> &cancel = cancelAfterFirst ? *cancelAfterFirst : never;
    const blockcopy::Result r = blockcopy::copy(o, cancel, [&](quint64, quint64) {
        if (cancelAfterFirst)
            cancel = true;
    });
    ::close(o.sourceFd);
    if (o.targetFd >= 0)
        ::close(o.targetFd);
    return r;
}

QString hex(const QByteArray &hash)
{
    return QString::fromLatin1(hash.toHex());
}

} // namespace

void copyTests()
{
    QTemporaryDir dir;
    auto path = [&](const char *name) { return dir.filePath(QLatin1String(name)); };

    // 40 MiB: random data with a long run of zeros in the middle, like a real disk.
    QByteArray source(40 * MiB, '\0');
    QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(source.data()), int(12 * MiB / 4));
    QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(source.data() + 30 * MiB), int(10 * MiB / 4));
    writeFile(path("source.img"), source);

    // Regions copied to different places in a target full of 0xAA; the rest stays 0xAA.
    writeFile(path("target.img"), QByteArray(48 * MiB, char(0xAA)));
    const QVector<Extent> regions = {{0, 1 * MiB, 0}, {5 * MiB, 7 * MiB, 9 * MiB}, {30 * MiB, 10 * MiB, 38 * MiB}};
    blockcopy::Result r = run(path("source.img"), Format::Raw, path("target.img"), Format::Raw, regions);
    const QByteArray target = readFile(path("target.img"));
    bool same = r.ok && r.bytes == 18 * MiB;
    for (const Extent &e : regions)
        same = same && target.mid(qsizetype(e.target), qsizetype(e.length)) == source.mid(qsizetype(e.source), qsizetype(e.length));
    report(same, QStringLiteral("regions land in the right places"), r.error);
    report(target.mid(1 * MiB, 8 * MiB) == QByteArray(8 * MiB, char(0xAA)) && target.mid(16 * MiB, 22 * MiB) == QByteArray(22 * MiB, char(0xAA)),
           QStringLiteral("space between the regions is left alone"));

    {
        const int fd = openFile(path("target.img"), O_RDONLY);
        std::atomic<bool> no{false};
        const blockcopy::Result back = blockcopy::readBack(fd, regions, no, {});
        ::close(fd);
        report(back.ok && back.dataSha256 == r.dataSha256, QStringLiteral("reading the copy back gives the same checksum"));
    }

    // Through zstd and back.
    const QVector<Extent> whole = {{0, quint64(source.size()), 0}};
    r = run(path("source.img"), Format::Raw, path("backup.img.zst"), Format::Zstd, whole);
    const QByteArray zst = readFile(path("backup.img.zst"));
    report(r.ok && zst.size() < source.size() * 3 / 4, QStringLiteral("back up to a compressed file"),
           QStringLiteral("%1 MB -> %2 MB %3").arg(source.size() / MiB).arg(zst.size() / MiB).arg(r.error));
    report(hex(r.fileSha256) == sha256File(path("backup.img.zst")), QStringLiteral("the stored file checksum matches sha256sum"));
    int code = -1;
    sh(QStringLiteral("zstd"), {QStringLiteral("-q"), QStringLiteral("-t"), path("backup.img.zst")}, &code);
    report(code == 0, QStringLiteral("the zstd tool accepts the backup"));
    report(hex(r.dataSha256) == sh(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("zstd -dc '%1' | sha256sum").arg(path("backup.img.zst"))}).section(QLatin1Char(' '), 0, 0),
           QStringLiteral("the data checksum matches what zstd unpacks"));

    const blockcopy::Result check = run(path("backup.img.zst"), Format::Zstd, {}, Format::Raw, whole);
    report(check.ok && check.dataSha256 == r.dataSha256, QStringLiteral("checking the backup without writing anything"), check.error);

    writeFile(path("restored.img"), QByteArray());
    const blockcopy::Result restored = run(path("backup.img.zst"), Format::Zstd, path("restored.img"), Format::Raw, whole);
    report(restored.ok && readFile(path("restored.img")) == source, QStringLiteral("restore gives back the exact data"), restored.error);

    // Damaged, cut short, or with junk after it: refused.
    QByteArray bad = zst;
    bad[bad.size() / 2] = char(bad[bad.size() / 2] ^ 0x01);
    writeFile(path("bad.zst"), bad);
    blockcopy::Result fail = run(path("bad.zst"), Format::Zstd, {}, Format::Raw, whole);
    report(!fail.ok && fail.error.contains(QLatin1String("damaged")), QStringLiteral("one changed byte is caught"), fail.error);
    writeFile(path("short.zst"), zst.left(zst.size() - 100));
    fail = run(path("short.zst"), Format::Zstd, {}, Format::Raw, whole);
    report(!fail.ok, QStringLiteral("a cut-short backup is caught"), fail.error);
    writeFile(path("long.zst"), zst + QByteArray("extra bytes"));
    fail = run(path("long.zst"), Format::Zstd, {}, Format::Raw, whole);
    report(!fail.ok && fail.error.contains(QLatin1String("more data")), QStringLiteral("junk after the backup is caught"), fail.error);
    fail = run(path("backup.img.zst"), Format::Zstd, {}, Format::Raw, {{0, quint64(source.size()) - MiB, 0}});
    report(!fail.ok && fail.error.contains(QLatin1String("more data")), QStringLiteral("a backup bigger than expected is caught"), fail.error);

    std::atomic<bool> cancel{false};
    fail = run(path("source.img"), Format::Raw, path("cancelled.img"), Format::Raw, whole, &cancel);
    report(fail.cancelled && fail.bytes < quint64(source.size()), QStringLiteral("cancel stops the copy"));
}
