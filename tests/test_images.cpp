// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Write Image to USB with compressed images and checksums: a random disk image packed in
// every format with libarchive's own writer, unpacked through ImageSource and written to a
// file by ImageWriter, plus checksum parsing and damaged archives.

#include "testkit.h"

#include "../src/usb/checksums.h"
#include "../src/usb/imagesource.h"
#include "../src/usb/imagewriter.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTemporaryDir>

#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr qsizetype kMiB = 1024 * 1024;

QByteArray makeImage()
{
    // Random data, then a stretch of zeros (packs well), with a boot signature like a real one.
    QByteArray data(5 * kMiB, Qt::Uninitialized);
    QRandomGenerator rng(4242);
    for (qsizetype i = 0; i < data.size(); i += 4)
        *reinterpret_cast<quint32 *>(data.data() + i) = rng.generate();
    data.append(QByteArray(kMiB + 1234, '\0'));
    data[510] = char(0x55);
    data[511] = char(0xAA);
    return data;
}

// Packs `data` with libarchive: a raw stream through `filter`, or a zip with a README first.
bool pack(const QString &path, const QByteArray &data, const QString &filter)
{
    archive *a = archive_write_new();
    if (filter == QLatin1String("zip")) {
        archive_write_set_format_zip(a);
    } else {
        archive_write_set_format_raw(a);
        if (filter == QLatin1String("xz"))
            archive_write_add_filter_xz(a);
        else if (filter == QLatin1String("gzip"))
            archive_write_add_filter_gzip(a);
        else if (filter == QLatin1String("bzip2"))
            archive_write_add_filter_bzip2(a);
        else if (filter == QLatin1String("lzma"))
            archive_write_add_filter_lzma(a);
        else if (filter == QLatin1String("zstd"))
            archive_write_add_filter_zstd(a);
    }
    bool ok = archive_write_open_filename(a, QFile::encodeName(path).constData()) == ARCHIVE_OK;
    auto add = [&](const char *name, const QByteArray &contents) {
        archive_entry *e = archive_entry_new();
        archive_entry_set_pathname(e, name);
        archive_entry_set_size(e, contents.size());
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, 0644);
        ok = ok && archive_write_header(a, e) == ARCHIVE_OK;
        ok = ok && archive_write_data(a, contents.constData(), size_t(contents.size())) == contents.size();
        archive_entry_free(e);
    };
    if (filter == QLatin1String("zip"))
        add("README.txt", "Write disk.img to a card.\n");
    add("disk.img", data);
    ok = archive_write_close(a) == ARCHIVE_OK && ok;
    archive_write_free(a);
    return ok;
}

QByteArray readAll(ImageSource &source, bool *ok)
{
    QByteArray out;
    QByteArray buffer(256 * 1024, Qt::Uninitialized);
    *ok = true;
    for (;;) {
        const qint64 n = source.read(buffer.data(), buffer.size());
        if (n < 0) {
            *ok = false;
            break;
        }
        if (n == 0)
            break;
        out.append(buffer.constData(), n);
    }
    return out;
}

struct Written {
    bool ok = false;
    QString message;
};

Written writeImage(const QString &image, const QString &target, qint64 targetSize, const checksums::Expected &expected = {})
{
    QFile::remove(target);
    QFile t(target);
    if (t.open(QIODevice::WriteOnly))
        t.resize(targetSize);
    t.close();
    const int fd = ::open(QFile::encodeName(target).constData(), O_RDWR | O_CLOEXEC);
    Written w;
    ImageWriter writer(image, fd, true, expected);
    QObject::connect(&writer, &ImageWriter::finished, [&](bool ok, const QString &m) {
        w.ok = ok;
        w.message = m;
    });
    writer.run();
    return w;
}

QByteArray fileHead(const QString &path, qint64 length)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.read(length) : QByteArray();
}

void checksumParsing()
{
    const QByteArray md5(32, 'a'), sha1(40, 'b'), sha256(64, 'c'), sha512(128, 'd');
    report(checksums::parse(QString::fromLatin1(md5)).algorithm == QCryptographicHash::Md5
               && checksums::parse(QString::fromLatin1(sha1)).algorithm == QCryptographicHash::Sha1
               && checksums::parse(QString::fromLatin1(sha256)).algorithm == QCryptographicHash::Sha256
               && checksums::parse(QString::fromLatin1(sha512)).algorithm == QCryptographicHash::Sha512,
           QStringLiteral("a checksum's kind is told by its length"));
    const checksums::Expected prefixed = checksums::parse(QStringLiteral("  sha256:") + QString::fromLatin1(sha256).toUpper() + QStringLiteral(" "));
    const checksums::Expected withName = checksums::parse(QString::fromLatin1(sha256) + QStringLiteral("  archlinux-x86_64.iso"));
    report(prefixed.hex == sha256 && withName.hex == sha256, QStringLiteral("a \"sha256:\" in front, capitals or the file name after it are fine"));
    report(!checksums::parse(QStringLiteral("not a checksum")).error.isEmpty() && !checksums::parse(QString(30, QLatin1Char('a'))).error.isEmpty()
               && !checksums::parse(QStringLiteral("hello")).isSet(),
           QStringLiteral("anything else is turned down, saying why"));
    report(!checksums::parse(QString()).isSet() && checksums::parse(QString()).error.isEmpty(), QStringLiteral("an empty field means no check"));

    const QByteArray gnu = sha256 + "  other.iso\n" + QByteArray(64, 'e') + " *./images/disk.img\n";
    const QByteArray bsd = "SHA512 (disk.img) = " + sha512 + "\nSHA512 (other.img) = " + QByteArray(128, 'f') + "\n";
    report(checksums::fromFile(gnu, QStringLiteral("/home/me/Downloads/disk.img")).hex == QByteArray(64, 'e'),
           QStringLiteral("a SHA256SUMS file gives the image's own line"));
    report(checksums::fromFile(bsd, QStringLiteral("disk.img")).algorithm == QCryptographicHash::Sha512
               && checksums::fromFile(bsd, QStringLiteral("disk.img")).hex == sha512,
           QStringLiteral("the BSD style works too"));
    report(checksums::fromFile(md5 + "  whatever.iso\n", QStringLiteral("disk.img")).hex == md5,
           QStringLiteral("a file with one line gives that line"));
    report(!checksums::fromFile(gnu, QStringLiteral("missing.iso")).error.isEmpty() && !checksums::fromFile("nothing here\n", QStringLiteral("a.iso")).error.isEmpty(),
           QStringLiteral("a file that doesn't list the image says so"));
}

void damaged(const QString &dir, const QStringList &packed)
{
    QRandomGenerator rng(777);
    int rounds = 0, failedCleanly = 0;
    bool crashed = false;
    for (const QString &path : packed) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QByteArray original = f.readAll();
        for (int round = 0; round < 40; ++round) {
            QByteArray data = original;
            const int hits = 1 + int(rng.bounded(30));
            for (int i = 0; i < hits; ++i)
                data[qsizetype(rng.bounded(quint32(data.size())))] = char(rng.bounded(256));
            if (rng.bounded(3) == 0)
                data.truncate(qsizetype(rng.bounded(quint32(data.size()))));
            const QString broken = dir + QStringLiteral("/broken-") + QFileInfo(path).fileName();
            QFile out(broken);
            if (out.open(QIODevice::WriteOnly))
                out.write(data);
            out.close();
            ++rounds;
            QString error;
            if (const std::unique_ptr<ImageSource> source = ImageSource::open(broken, &error)) {
                bool ok = false;
                const QByteArray got = readAll(*source, &ok);
                if (!ok)
                    ++failedCleanly;
                crashed = crashed || got.size() > 64 * kMiB;
            } else {
                ++failedCleanly;
            }
        }
    }
    report(!crashed, QStringLiteral("%1 damaged archives: no crash, nothing runs away (%2 refused)").arg(rounds).arg(failedCleanly));
}

} // namespace

void imageTests()
{
    QTemporaryDir dir;
    const QByteArray image = makeImage();
    const QString plain = dir.filePath(QStringLiteral("disk.img"));
    QFile f(plain);
    if (f.open(QIODevice::WriteOnly))
        f.write(image);
    f.close();

    QStringList packed;
    for (const QString &filter : {QStringLiteral("xz"), QStringLiteral("gzip"), QStringLiteral("bzip2"), QStringLiteral("lzma"),
                                  QStringLiteral("zstd"), QStringLiteral("zip")}) {
        const QString path = dir.filePath(QStringLiteral("disk.img.") + filter);
        if (!pack(path, image, filter)) {
            report(false, QStringLiteral("pack the test image with %1").arg(filter));
            continue;
        }
        packed << path;
        QString error;
        const std::unique_ptr<ImageSource> source = ImageSource::open(path, &error);
        bool ok = false;
        const QByteArray got = source ? readAll(*source, &ok) : QByteArray();
        const bool sizeRight = !source || source->size() == 0 || source->size() == quint64(image.size());
        report(source && ok && got == image && source->compression() == filter && sizeRight,
               QStringLiteral("%1: unpacked on the way, byte for byte").arg(filter), source ? QString() : error);
        if (filter == QLatin1String("zip"))
            report(source && source->innerName() == QLatin1String("disk.img") && source->size() == quint64(image.size()),
                   QStringLiteral("zip: the image is picked out past the README, with its size"));
        report(hasBootSignature(path), QStringLiteral("%1: the boot signature is seen through the packing").arg(filter));
    }
    const std::unique_ptr<ImageSource> raw = ImageSource::open(plain, nullptr);
    report(raw && raw->compression().isEmpty() && raw->size() == quint64(image.size()), QStringLiteral("a plain image is read as it is"));
    QFile noBoot(dir.filePath(QStringLiteral("data.img")));
    if (noBoot.open(QIODevice::WriteOnly))
        noBoot.write(QByteArray(4096, 'x'));
    noBoot.close();
    report(!hasBootSignature(noBoot.fileName()), QStringLiteral("an image without a boot record is told apart"));

    // Through the writer, onto a file standing in for a drive.
    const QString target = dir.filePath(QStringLiteral("drive.bin"));
    for (const QString &path : std::as_const(packed)) {
        const Written w = writeImage(path, target, 8 * kMiB);
        report(w.ok && fileHead(target, image.size()) == image, QStringLiteral("%1: written and verified").arg(QFileInfo(path).suffix()), w.message);
    }
    const QByteArray fileSha = QCryptographicHash::hash(fileHead(packed.value(0), 64 * kMiB), QCryptographicHash::Sha512).toHex();
    Written w = writeImage(packed.value(0), target, 8 * kMiB, checksums::parse(QString::fromLatin1(fileSha)));
    report(w.ok, QStringLiteral("a matching SHA-512 of the download lets it through"), w.message);
    w = writeImage(packed.value(0), target, 8 * kMiB, checksums::parse(QString(64, QLatin1Char('0'))));
    report(!w.ok && w.message.contains(QLatin1String("SHA-256")) && fileHead(target, 4096) == QByteArray(4096, '\0'),
           QStringLiteral("a wrong one stops it before anything is written"), w.message);
    w = writeImage(packed.value(0), target, 4 * kMiB);
    report(!w.ok && w.message.contains(QLatin1String("bigger")), QStringLiteral("an image that doesn't fit is refused"), w.message);

    // All four checksums in one pass.
    checksums::Hasher hasher(plain);
    QStringList hex;
    QObject::connect(&hasher, &checksums::Hasher::finished, [&](bool, const QStringList &h, const QString &) { hex = h; });
    hasher.run();
    QStringList expected;
    for (QCryptographicHash::Algorithm a : checksums::algorithms())
        expected << QString::fromLatin1(QCryptographicHash::hash(image, a).toHex());
    report(hex == expected, QStringLiteral("MD5, SHA-1, SHA-256 and SHA-512 in one pass"));

    checksumParsing();
    damaged(dir.path(), packed);
}
