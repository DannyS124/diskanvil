// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Find Lost Files, finding files by their contents: real files of every kind it knows (made
// with Qt, libarchive, or by hand where nothing here writes them) dropped at known places in
// a stretch of random bytes, then found again, measured to the byte, saved and compared. Also
// damaged and cut-off files, bad spots, Stop, and random damage that must never crash it.

#include "testkit.h"

#include "../src/filetypes.h"
#include "../src/imagesource.h"
#include "../src/lostfs.h"
#include "../src/lostscan.h"
#include "../src/lostsource.h"
#include "../src/mediainfo.h"
#include "../src/officetext.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QtEndian>

#include <archive.h>
#include <archive_entry.h>

namespace {

constexpr qsizetype kMiB = 1024 * 1024;

struct Sample {
    QString type;    // what it should be found as
    QByteArray data;
    quint64 offset = 0;
};

QByteArray picture(const char *format)
{
    QImage image(320, 200, QImage::Format_RGB32);
    QPainter p(&image);
    QLinearGradient g(0, 0, 320, 200);
    g.setColorAt(0, QColor(0x06, 0x0a, 0x16));
    g.setColorAt(1, QColor(0x00, 0xe5, 0xff));
    p.fillRect(image.rect(), g);
    p.setBrush(QColor(0xff, 0x33, 0x55));
    p.drawEllipse(QRect(40, 40, 120, 120));
    p.setBrush(QColor(0x2b, 0xee, 0x8a));
    p.drawRect(QRect(180, 60, 100, 80));
    p.end();
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format, 85);
    return out;
}

// A small but proper PDF: one page with a line of text, the object table and the trailer.
QByteArray pdf()
{
    const QByteArray stream = "BT /F1 24 Tf 72 720 Td (A PDF that was lost and found again.) Tj ET";
    const QList<QByteArray> objects = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>",
        "<< /Length " + QByteArray::number(stream.size()) + " >>\nstream\n" + stream + "\nendstream",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    };
    QByteArray out = "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n";
    QList<qsizetype> offsets;
    for (int i = 0; i < objects.size(); ++i) {
        offsets << out.size();
        out += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const qsizetype xref = out.size();
    out += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (qsizetype o : offsets)
        out += QByteArray::number(o).rightJustified(10, '0') + " 00000 n \n";
    out += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return out;
}

// A ZIP with these files in it, stored as they are (the mimetype of an OpenDocument has to be).
QByteArray zip(const QList<QPair<QByteArray, QByteArray>> &files)
{
    QByteArray out(4 * kMiB, '\0');
    size_t used = 0;
    archive *a = archive_write_new();
    archive_write_set_format_zip(a);
    archive_write_set_options(a, "zip:compression=store");
    archive_write_open_memory(a, out.data(), size_t(out.size()), &used);
    for (const auto &f : files) {
        archive_entry *e = archive_entry_new();
        archive_entry_set_pathname(e, f.first.constData());
        archive_entry_set_size(e, f.second.size());
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, 0644);
        archive_write_header(a, e);
        archive_write_data(a, f.second.constData(), size_t(f.second.size()));
        archive_entry_free(e);
    }
    archive_write_close(a);
    archive_write_free(a);
    out.resize(qsizetype(used));
    return out;
}

void be32(QByteArray &d, quint32 v)
{
    d.append(char(v >> 24)).append(char(v >> 16)).append(char(v >> 8)).append(char(v));
}

void le32(QByteArray &d, quint32 v)
{
    d.append(char(v)).append(char(v >> 8)).append(char(v >> 16)).append(char(v >> 24));
}

QByteArray box(const char *type, const QByteArray &body)
{
    QByteArray b;
    be32(b, quint32(8 + body.size()));
    b.append(type, 4).append(body);
    return b;
}

QByteArray mp4()
{
    QByteArray ftyp("isom");
    be32(ftyp, 512);
    ftyp.append("isomiso2avc1mp41");
    QByteArray media(200000, Qt::Uninitialized);
    for (qsizetype i = 0; i < media.size(); ++i)
        media[i] = char(i * 7 + 3);
    return box("ftyp", ftyp) + box("moov", box("mvhd", QByteArray(100, '\1'))) + box("mdat", media);
}

QByteArray wav()
{
    QByteArray fmt, data(44100, '\x40'), out("RIFF");
    le32(fmt, 0x00010001); // PCM, mono
    le32(fmt, 44100);
    le32(fmt, 44100);
    le32(fmt, 0x00080001);
    le32(out, quint32(4 + 8 + fmt.size() + 8 + data.size()));
    out.append("WAVEfmt ");
    le32(out, quint32(fmt.size()));
    out.append(fmt).append("data");
    le32(out, quint32(data.size()));
    return out.append(data);
}

QByteArray mp3()
{
    QByteArray out("ID3\x03\x00\x00\x00\x00\x00\x0a", 10);
    out.append(QByteArray(10, '\0')); // the tag's padding
    // MPEG-1 layer III, 128 kbit/s, 44.1 kHz: 417 bytes a frame.
    for (int i = 0; i < 12; ++i) {
        QByteArray frame(417, char(i));
        frame[0] = char(0xFF);
        frame[1] = char(0xFB);
        frame[2] = char(0x90);
        frame[3] = char(0x00);
        out.append(frame);
    }
    return out;
}

quint32 oggCrc(const QByteArray &data)
{
    quint32 crc = 0;
    for (const char c : data) {
        crc ^= quint32(uchar(c)) << 24;
        for (int i = 0; i < 8; ++i)
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
    }
    return crc;
}

QByteArray oggPage(quint8 flags, quint32 sequence, const QByteArray &payload)
{
    QByteArray page("OggS\0", 5);
    page.append(char(flags));
    page.append(QByteArray(8, '\0'));         // granule position
    le32(page, 0x1234);                       // stream serial
    le32(page, sequence);
    le32(page, 0);                            // CRC, filled in below
    page.append(char(1)).append(char(payload.size()));
    page.append(payload);
    const quint32 crc = oggCrc(page);
    page[22] = char(crc);
    page[23] = char(crc >> 8);
    page[24] = char(crc >> 16);
    page[25] = char(crc >> 24);
    return page;
}

QByteArray ogg()
{
    return oggPage(0x02, 0, QByteArray("\x01vorbis") + QByteArray(23, '\x05')) + oggPage(0x00, 1, QByteArray(200, 'a'))
           + oggPage(0x04, 2, QByteArray(100, 'b'));
}

QByteArray sqlite()
{
    QByteArray out("SQLite format 3\0", 16);
    out.append(char(0x10)).append(char(0x00));  // 4096-byte pages
    out.append(QByteArray(6, '\x01'));
    be32(out, 7);                               // change counter
    be32(out, 3);                               // 3 pages
    out.append(QByteArray(92 - out.size(), '\0'));
    be32(out, 7);                               // valid for change 7
    out.append(QByteArray(3 * 4096 - out.size(), '\x2a'));
    return out;
}

QByteArray sevenZip()
{
    const QByteArray next = QByteArray("\x01\x04\x06\x00\x01\x09\x00\x00", 8);
    QByteArray start;
    auto le64 = [&start](quint64 v) {
        for (int i = 0; i < 8; ++i)
            start.append(char(v >> (8 * i)));
    };
    le64(16);
    le64(quint64(next.size()));
    le32(start, filetypes::crc32(reinterpret_cast<const uchar *>(next.constData()), next.size()));
    QByteArray out("7z\xbc\xaf\x27\x1c\x00\x04", 8);
    le32(out, filetypes::crc32(reinterpret_cast<const uchar *>(start.constData()), start.size()));
    return out + start + QByteArray(16, '\x33') + next;
}

QByteArray gif()
{
    return QByteArray::fromBase64("R0lGODlhAQABAIAAAP///wAAACH5BAEAAAAALAAAAAABAAEAAAICRAEAOw==");
}

QByteArray noise(qsizetype size, quint32 seed)
{
    QByteArray data(size, Qt::Uninitialized);
    QRandomGenerator rng(seed);
    for (qsizetype i = 0; i + 4 <= size; i += 4)
        *reinterpret_cast<quint32 *>(data.data() + i) = rng.generate();
    return data;
}

QVector<Sample> samples()
{
    QVector<Sample> s = {
        {QStringLiteral("jpg"), picture("JPG")},
        {QStringLiteral("png"), picture("PNG")},
        {QStringLiteral("bmp"), picture("BMP")},
        {QStringLiteral("gif"), gif()},
        {QStringLiteral("pdf"), pdf()},
        {QStringLiteral("docx"), zip({{"[Content_Types].xml", "<Types/>"}, {"word/document.xml", "<w:document>hello</w:document>"}})},
        {QStringLiteral("odt"), zip({{"mimetype", "application/vnd.oasis.opendocument.text"}, {"content.xml", "<office:document-content/>"}})},
        {QStringLiteral("zip"), zip({{"notes.txt", QByteArray(5000, 'n')}, {"inner.zip", zip({{"a.txt", "inside"}})}})},
        {QStringLiteral("mp4"), mp4()},
        {QStringLiteral("wav"), wav()},
        {QStringLiteral("mp3"), mp3()},
        {QStringLiteral("ogg"), ogg()},
        {QStringLiteral("sqlite"), sqlite()},
        {QStringLiteral("7z"), sevenZip()},
    };
    return s;
}

// The samples at sector starts in random bytes, with random gaps between them.
QByteArray image(QVector<Sample> &files, qsizetype size)
{
    QByteArray data = noise(size, 777);
    QRandomGenerator rng(31);
    quint64 at = 4096;
    for (Sample &f : files) {
        at = (at + 511) / 512 * 512;
        f.offset = at;
        data.replace(qsizetype(at), f.data.size(), f.data);
        at += quint64(f.data.size()) + 512 * rng.bounded(1, 400);
    }
    return data;
}

QVector<lost::Found> scan(std::shared_ptr<lost::Source> source, bool *stopped = nullptr, bool cancelFirst = false)
{
    QVector<lost::Found> all;
    lost::DeepScan deep(source);
    QObject::connect(&deep, &lost::DeepScan::found, [&all](const QVector<lost::Found> &batch) { all += batch; });
    QObject::connect(&deep, &lost::DeepScan::finished, [stopped](bool s) {
        if (stopped)
            *stopped = s;
    });
    if (cancelFirst)
        deep.cancel();
    deep.run();
    return all;
}

void finding(const QString &dir)
{
    QVector<Sample> files = samples();
    for (const Sample &f : std::as_const(files))
        report(!f.data.isEmpty(), QStringLiteral("test file made: %1 (%2 bytes)").arg(f.type).arg(f.data.size()));
    const QByteArray data = image(files, 32 * kMiB);
    auto source = lost::Source::fromMemory(data);
    const QVector<lost::Found> found = scan(source);

    for (const Sample &f : std::as_const(files)) {
        const lost::Found *hit = nullptr;
        for (const lost::Found &g : found) {
            if (!g.extents.isEmpty() && g.extents[0].start == f.offset)
                hit = &g;
        }
        const QString type = hit && hit->type >= 0 ? filetypes::types()[hit->type].id : QString();
        report(hit && type == f.type && hit->size == quint64(f.data.size()) && hit->condition == lost::Condition::Good,
               QStringLiteral("%1 found where it was, to the byte, whole").arg(f.type),
               hit ? QStringLiteral("%1, %2 bytes (should be %3), condition %4").arg(type).arg(hit->size).arg(f.data.size()).arg(int(hit->condition))
                   : QStringLiteral("not found"));
    }
    report(found.size() == files.size(), QStringLiteral("nothing else turns up in the random bytes (or inside a whole file)"),
           QStringLiteral("%1 found, %2 placed").arg(found.size()).arg(files.size()));
    bool named = true;
    for (const lost::Found &g : found)
        named = named && g.name.endsWith(QLatin1Char('.') + filetypes::types()[g.type].id) && g.origin == lost::Origin::Contents;
    report(named, QStringLiteral("each gets a made-up name with the right extension"), found.value(0).name);

    // Saving: every byte as it was, a name that's taken isn't overwritten, a report goes along.
    lost::Saver saver(source, found, dir);
    QString savedTo;
    int saved = 0, failedCount = 0;
    QObject::connect(&saver, &lost::Saver::finished, [&](int s, int f, const QString &folder, const QString &) {
        saved = s;
        failedCount = f;
        savedTo = folder;
    });
    saver.run();
    int same = 0;
    for (const lost::Found &g : found) {
        const QString category = filetypes::categoryName(filetypes::types()[g.type].category);
        QFile out(savedTo + QLatin1Char('/') + category + QLatin1Char('/') + g.name);
        const QByteArray want = data.mid(qsizetype(g.extents[0].start), qsizetype(g.size));
        if (out.open(QIODevice::ReadOnly) && out.readAll() == want)
            ++same;
    }
    report(saved == found.size() && failedCount == 0 && same == found.size(), QStringLiteral("everything saved, byte for byte, sorted into folders"),
           QStringLiteral("%1 saved, %2 the same, in %3").arg(saved).arg(same).arg(savedTo));
    report(QFileInfo::exists(savedTo + QStringLiteral("/What was saved.txt")), QStringLiteral("with \"What was saved.txt\""));
    lost::Saver again(source, {found[0]}, dir);
    QString againTo;
    QObject::connect(&again, &lost::Saver::finished, [&](int, int, const QString &folder, const QString &) { againTo = folder; });
    again.run();
    const QString category = filetypes::categoryName(filetypes::types()[found[0].type].category);
    const QString stem = found[0].name.section(QLatin1Char('.'), 0, -2), ext = found[0].name.section(QLatin1Char('.'), -1);
    report(againTo != savedTo || QFileInfo::exists(againTo + QLatin1Char('/') + category + QStringLiteral("/%1 (2).%2").arg(stem, ext)),
           QStringLiteral("saving again never overwrites"));
}

void damage()
{
    // A PNG without its end and a JPEG cut short: found, but not as whole.
    QByteArray png = picture("PNG"), jpg = picture("JPG");
    png.chop(12);
    jpg.chop(jpg.size() / 3);
    QVector<Sample> files = {{QStringLiteral("png"), png}, {QStringLiteral("jpg"), jpg}, {QStringLiteral("png"), picture("PNG")}};
    QByteArray data = image(files, 4 * kMiB);
    // A PNG with one changed byte in the middle: its CRCs give it away.
    data[qsizetype(files[2].offset) + files[2].data.size() / 2] ^= 0x40;
    const QVector<lost::Found> found = scan(lost::Source::fromMemory(data));
    auto at = [&found](quint64 offset) -> const lost::Found * {
        for (const lost::Found &g : found) {
            if (g.extents.value(0).start == offset)
                return &g;
        }
        return nullptr;
    };
    const lost::Found *cutPng = at(files[0].offset), *cutJpg = at(files[1].offset), *changed = at(files[2].offset);
    report(cutPng && cutPng->condition == lost::Condition::MaybeDamaged, QStringLiteral("a PNG without its end is found, as maybe damaged"));
    report(cutJpg && cutJpg->condition == lost::Condition::MaybeDamaged, QStringLiteral("a JPEG cut short is found, as maybe damaged"));
    report(changed && changed->condition == lost::Condition::MaybeDamaged && changed->size == quint64(files[2].data.size()),
           QStringLiteral("a PNG with a changed byte is found whole in size, but as maybe damaged (bad CRC)"));

    // Bad spots: the part of a file on them reads as zeros, and the file says so.
    QVector<Sample> one = {{QStringLiteral("bmp"), picture("BMP")}};
    const QByteArray withBmp = image(one, 2 * kMiB);
    auto source = lost::Source::fromMemory(withBmp);
    const quint64 badStart = (one[0].offset + 64 * 1024 + 65535) / 65536 * 65536;
    source->failReads({{badStart, 4096}});
    const QVector<lost::Found> bmp = scan(source);
    const lost::Found *b = bmp.isEmpty() ? nullptr : &bmp[0];
    QByteArray readBack;
    if (b) {
        lost::FileReader reader(source, *b);
        readBack = reader.head(qint64(b->size));
    }
    const qsizetype inFile = qsizetype(badStart - one[0].offset);
    report(b && b->condition == lost::Condition::Unreadable && readBack.size() == one[0].data.size() && readBack.mid(inFile, 4096) == QByteArray(4096, '\0')
               && readBack.left(inFile) == one[0].data.left(inFile),
           QStringLiteral("a file on a bad spot: found, marked unreadable, the bad part comes back as zeros"));
    report(source->unreadable(badStart, 1) && !source->unreadable(0, 4096), QStringLiteral("the bad spot is remembered, the rest isn't"));

    bool stopped = false;
    scan(lost::Source::fromMemory(withBmp), &stopped, true);
    report(stopped, QStringLiteral("Stop stops it"));
}

void names()
{
    report(lost::cleanName(QStringLiteral("a/b\\c:d*e?f\"g<h>i|j.txt")) == QLatin1String("a_b_c_d_e_f_g_h_i_j.txt")
               && lost::cleanName(QStringLiteral("..")) == QLatin1String("file") && lost::cleanName(QStringLiteral(" ok. ")) == QLatin1String("ok")
               && lost::cleanName(QString(QChar(7)) + QStringLiteral("x")) == QLatin1String("_x"),
           QStringLiteral("names any drive takes: no slashes, Windows' forbidden characters, control characters or dots at the ends"));
    const QString longName = QString(300, QLatin1Char('a')) + QStringLiteral(".jpeg");
    const QString cut = lost::cleanName(longName);
    report(cut.toUtf8().size() <= 200 && cut.endsWith(QLatin1String(".jpeg")), QStringLiteral("a name that's too long is shortened, keeping the extension"), cut.right(12));
}

// Random damage: measuring must never crash, never claim more than its limit and never read
// outside what it's given.
void fuzz()
{
    const QVector<Sample> files = samples();
    QRandomGenerator rng(4321);
    int rounds = 0;
    bool sane = true;
    for (; rounds < 1500; ++rounds) {
        QByteArray d = files[rounds % files.size()].data;
        const int changes = 1 + int(rng.bounded(12));
        for (int i = 0; i < changes && !d.isEmpty(); ++i) {
            const qsizetype at = qsizetype(rng.bounded(quint32(d.size())));
            d[at] = char(rng.bounded(256));
        }
        if (rng.bounded(4) == 0)
            d.truncate(qsizetype(rng.bounded(quint32(d.size()))));
        if (d.size() < 32)
            continue;
        const QVector<int> types = filetypes::candidates(reinterpret_cast<const uchar *>(d.constData()), int(std::min<qsizetype>(d.size(), 512)));
        for (int type : types) {
            const filetypes::Reader read = [&d](quint64 at, char *buf, qint64 len) -> qint64 {
                if (at >= quint64(d.size()))
                    return 0;
                const qint64 n = std::min<qint64>(len, d.size() - qint64(at));
                memcpy(buf, d.constData() + at, size_t(n));
                return n;
            };
            const filetypes::Measured m = filetypes::measure(type, read);
            sane = sane && m.size <= filetypes::sizeLimit(type) && (m.size == 0 || m.type >= 0) && (!m.complete || m.size <= quint64(d.size()));
        }
    }
    report(sane, QStringLiteral("%1 damaged files measured: no crash, nothing whole that runs past the data").arg(rounds));
}

} // namespace

QByteArray unpack(const QString &path)
{
    QString error;
    const std::unique_ptr<ImageSource> image = ImageSource::open(path, &error);
    QByteArray out;
    if (!image)
        return out;
    QByteArray buffer(kMiB, Qt::Uninitialized);
    for (qint64 n; (n = image->read(buffer.data(), buffer.size())) > 0;)
        out += buffer.left(n);
    return out;
}

// Runs a quick scan to the end on this thread and collects what it found.
QVector<lost::Found> quickScan(const std::shared_ptr<lost::Source> &source, QVector<lost::Volume> *volumes = nullptr)
{
    lost::QuickScan scan(source);
    QVector<lost::Found> all;
    QObject::connect(&scan, &lost::QuickScan::found, [&all](const QVector<lost::Found> &files) { all += files; });
    scan.run();
    if (volumes)
        *volumes = scan.volumes();
    return all;
}

// FAT12, FAT16, FAT32 and exFAT images that Linux put files on and deleted some of them from
// (tests/data/lostfs/make.sh): every file comes back with its name, folder and contents.
void fileSystems(const QString &dir)
{
    const QString data = QStringLiteral(SOURCE_DIR "/tests/data/lostfs/");
    QFile list(data + QStringLiteral("expected.txt"));
    report(list.open(QIODevice::ReadOnly), QStringLiteral("file systems: the list of what's on the test images"));
    struct Expected {
        QString path;
        bool deleted;
        quint64 size;
        QByteArray sha256;
    };
    QHash<QString, QVector<Expected>> expected;
    for (const QByteArray &line : list.readAll().split('\n')) {
        const QList<QByteArray> f = line.split('|');
        if (f.size() == 5)
            expected[QString::fromUtf8(f[0])].push_back({QString::fromUtf8(f[1]), f[2] == "yes", f[3].toULongLong(), f[4]});
    }
    const QHash<QString, QString> labels = {{QStringLiteral("fat12"), QStringLiteral("TESTFAT12")}, {QStringLiteral("fat16"), QStringLiteral("TESTFAT16")},
                                            {QStringLiteral("fat32"), QStringLiteral("TESTFAT32")}, {QStringLiteral("exfat"), QStringLiteral("TESTEXFAT")},
                                            {QStringLiteral("ntfs"), QStringLiteral("TESTNTFS")}, {QStringLiteral("ntfs-windows"), QStringLiteral("WINNTFS")}};
    // ntfs-windows: made and deleted from by Windows itself (windows-job.cmd).
    for (const QString &name : {QStringLiteral("fat12"), QStringLiteral("fat16"), QStringLiteral("fat32"), QStringLiteral("exfat"), QStringLiteral("ntfs"),
                                QStringLiteral("ntfs-windows")}) {
        const QByteArray image = unpack(data + name + QStringLiteral(".img.zst"));
        report(!image.isEmpty(), QStringLiteral("%1: the test image unpacks").arg(name));
        auto source = lost::Source::fromMemory(image);
        QVector<lost::Volume> volumes;
        const QVector<lost::Found> found = quickScan(source, &volumes);
        const QString type = name.startsWith(QLatin1String("fat")) ? QStringLiteral("vfat") : name.section(QLatin1Char('-'), 0, 0);
        report(volumes.size() == 1 && volumes.value(0).label == labels[name] && volumes.value(0).type == type,
               QStringLiteral("%1: the file system and its label are found").arg(name),
               volumes.isEmpty() ? QStringLiteral("none") : volumes[0].type + QLatin1Char(' ') + volumes[0].label);

        for (const Expected &e : expected.value(name)) {
            const QString folder = labels[name] + (e.path.contains(QLatin1Char('/')) ? QLatin1Char('/') + e.path.section(QLatin1Char('/'), 0, -2) : QString());
            const QString file = e.path.section(QLatin1Char('/'), -1);
            const lost::Found *match = nullptr;
            for (const lost::Found &f : found) {
                // FAT forgets the first letter of a deleted short name like notes.txt.
                const bool sameName = f.name == file || (f.nameGuessed && f.name.mid(1) == file.mid(1));
                if (sameName && f.folder == folder)
                    match = &f;
            }
            const QString what = QStringLiteral("%1: %2").arg(name, e.path);
            report(match != nullptr, what + QStringLiteral(" is listed"),
                   [&] {
                       QStringList all;
                       for (const lost::Found &f : found)
                           all << f.folder + QLatin1Char('/') + f.name;
                       return all.join(QStringLiteral(", "));
                   }());
            if (!match)
                continue;
            report(match->deleted == e.deleted && match->size == e.size && match->origin == lost::Origin::FileSystem,
                   what + QStringLiteral(" is listed as it was"),
                   QStringLiteral("deleted %1, size %2").arg(match->deleted).arg(match->size));
            lost::FileReader reader(source, *match);
            const QByteArray bytes = reader.head(qint64(match->size));
            report(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() == e.sha256, what + QStringLiteral(" comes back whole"));
            const bool picture = file.endsWith(QLatin1String(".jpg")) || file.endsWith(QLatin1String(".png"));
            if (e.deleted && picture)
                report(match->checked && match->condition == lost::Condition::Good, what + QStringLiteral(" is checked and looks whole"),
                       QStringLiteral("checked %1, condition %2").arg(match->checked).arg(int(match->condition)));
            // A short name with no long one loses its first letter for good on FAT; a long name
            // gives it back.
            if (e.deleted && file == QLatin1String("README.TXT") && name.startsWith(QLatin1String("fat")))
                report(match->nameGuessed && match->name == QLatin1String("_EADME.TXT"), what + QStringLiteral(": the lost first letter is said to be a guess"),
                       match->name);
            if (e.deleted && file == QLatin1String("notes.txt"))
                report(!match->nameGuessed && match->name == file, what + QStringLiteral(": its whole name comes back"), match->name);
            if (e.path.startsWith(QLatin1String("Recycle Bin/")))
                report(match->note.contains(QLatin1String("C:\\Users\\Test\\Pictures")), what + QStringLiteral(": it says where it was before the bin"),
                       match->note);
        }

        // Saving keeps the folders the files were in.
        QVector<lost::Found> deleted;
        for (const lost::Found &f : found) {
            if (f.deleted)
                deleted << f;
        }
        lost::Saver saver(source, deleted, dir);
        QString base;
        QObject::connect(&saver, &lost::Saver::finished, [&base](int, int, const QString &folder, const QString &) { base = folder; });
        saver.run();
        const QString saved = base + QLatin1Char('/') + labels[name] + QStringLiteral("/Old stuff/scan.png");
        QFile back(saved);
        report(back.open(QIODevice::ReadOnly) && QCryptographicHash::hash(back.readAll(), QCryptographicHash::Sha256).toHex()
                   == expected.value(name).value(7).sha256,
               QStringLiteral("%1: a file from a deleted folder is saved into that folder").arg(name), saved);
        QDir(base).removeRecursively();

        // The deep scan doesn't list the pictures a second time.
        lost::QuickScan quick(source);
        quick.run();
        lost::DeepScan deep(source);
        deep.skip(quick.knownRanges(), quick.knownStarts());
        int carved = 0;
        QObject::connect(&deep, &lost::DeepScan::found, [&carved](const QVector<lost::Found> &files) { carved += int(files.size()); });
        deep.run();
        report(carved == 0, QStringLiteral("%1: the deep scan doesn't find them again").arg(name), QString::number(carved));
    }

    // Deleted by Linux's own NTFS driver, which takes the names away: the files are still all
    // there, named after what's in them.
    {
        auto source = lost::Source::fromMemory(unpack(data + QStringLiteral("ntfs-linux.img.zst")));
        QSet<QByteArray> nameless;
        QStringList names;
        for (const lost::Found &f : quickScan(source)) {
            if (f.deleted && f.nameGuessed && f.name.startsWith(QLatin1String("Nameless file"))) {
                lost::FileReader reader(source, f);
                nameless.insert(QCryptographicHash::hash(reader.head(qint64(f.size)), QCryptographicHash::Sha256).toHex());
                names << f.name;
            }
        }
        int missing = 0;
        for (const Expected &e : expected.value(QStringLiteral("ntfs-linux"))) {
            if (e.deleted && !e.path.startsWith(QLatin1String("Recycle Bin/")))
                missing += !nameless.contains(e.sha256);
        }
        report(missing == 0 && names.size() == 6, QStringLiteral("ntfs: files Linux deleted come back without their names"),
               QStringLiteral("%1 missing; %2").arg(missing).arg(names.join(QStringLiteral(", "))));
    }

    // Partition tables: the same file system inside an MBR and a GPT drive.
    const QByteArray fat16 = unpack(data + QStringLiteral("fat16.img.zst"));
    for (const bool gpt : {false, true}) {
        QByteArray drive(qsizetype(32 * kMiB), '\0');
        const qsizetype at = kMiB;
        drive.replace(at, fat16.size(), fat16);
        uchar *d = reinterpret_cast<uchar *>(drive.data());
        auto put32 = [](uchar *p, quint32 v) { qToLittleEndian(v, p); };
        auto put64 = [](uchar *p, quint64 v) { qToLittleEndian(v, p); };
        d[510] = 0x55;
        d[511] = 0xAA;
        if (!gpt) {
            d[446 + 4] = 0x06;
            put32(d + 446 + 8, quint32(at / 512));
            put32(d + 446 + 12, quint32(fat16.size() / 512));
        } else {
            d[446 + 4] = 0xEE;
            put32(d + 446 + 8, 1);
            put32(d + 446 + 12, quint32(drive.size() / 512 - 1));
            uchar *h = d + 512;
            memcpy(h, "EFI PART", 8);
            put64(h + 72, 2);
            put32(h + 80, 128);
            put32(h + 84, 128);
            uchar *e = d + 1024;
            const quint8 basicData[16] = {0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44, 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};
            memcpy(e, basicData, 16);
            e[16] = 1;
            put64(e + 32, quint64(at / 512));
            put64(e + 40, quint64((at + fat16.size()) / 512 - 1));
        }
        QVector<lost::Volume> volumes;
        const QVector<lost::Found> found = quickScan(lost::Source::fromMemory(drive), &volumes);
        report(volumes.size() == 1 && volumes[0].where.start == quint64(at) && found.size() == expected.value(QStringLiteral("fat16")).size(),
               QStringLiteral("file systems: found inside %1 partition table").arg(gpt ? QStringLiteral("a GPT") : QStringLiteral("an MBR")),
               QStringLiteral("%1 file systems, %2 files").arg(volumes.size()).arg(found.size()));
    }
}

// Damaged file systems: every byte of the tables and folders flipped at random, many times
// over. It must never crash or loop, whatever it lists.
void fileSystemFuzz()
{
    const QString data = QStringLiteral(SOURCE_DIR "/tests/data/lostfs/");
    QRandomGenerator rnd(20261010);
    // DISKFORGE_FUZZ_ROUNDS=1000 for a longer run by hand.
    const int rounds = qEnvironmentVariableIsSet("DISKFORGE_FUZZ_ROUNDS") ? qEnvironmentVariableIntValue("DISKFORGE_FUZZ_ROUNDS") : 60;
    int runs = 0;
    for (const QString &name : {QStringLiteral("fat12"), QStringLiteral("fat32"), QStringLiteral("exfat"), QStringLiteral("ntfs")}) {
        const QByteArray clean = unpack(data + name + QStringLiteral(".img.zst"));
        // Where the boot sector, the tables and the folders are: the first few hundred KiB.
        const qsizetype hot = std::min<qsizetype>(clean.size(), 640 * 1024);
        for (int round = 0; round < rounds; ++round) {
            QByteArray broken = clean;
            for (int k = 0; k < 400; ++k)
                broken[rnd.bounded(int(hot))] = char(rnd.bounded(256));
            if (round % 3 == 0) // and sometimes the boot sector itself
                broken[rnd.bounded(512)] = char(rnd.bounded(256));
            quickScan(lost::Source::fromMemory(broken));
            ++runs;
        }
    }
    report(runs == 4 * rounds, QStringLiteral("file systems: %1 damaged images read without a crash").arg(4 * rounds));
}

int scanImage(const QString &path)
{
    QString error;
    auto source = lost::Source::fromImage(path, &error);
    if (!source) {
        out << error << Qt::endl;
        return 1;
    }
    QVector<lost::Volume> volumes;
    const QVector<lost::Found> found = quickScan(source, &volumes);
    for (const lost::Volume &v : std::as_const(volumes))
        out << v.type << " " << v.label << " at " << v.where.start << ": " << v.files << " files, " << v.deleted << " deleted" << Qt::endl;
    for (const lost::Found &f : found) {
        out << (f.deleted ? "deleted " : "        ") << f.folder << "/" << f.name << "  " << f.size << " bytes, condition "
            << int(f.condition) << (f.checked ? " checked" : "") << (f.nameGuessed ? " name-guessed" : "")
            << (f.note.isEmpty() ? QString() : QStringLiteral("  (%1)").arg(f.note)) << Qt::endl;
    }
    return 0;
}

// --- What the previews read: media details, office text, archive listings

QByteArray fullBox(const char *type, quint8 version, const QByteArray &body)
{
    return box(type, QByteArray(1, char(version)) + QByteArray(3, '\0') + body);
}

QByteArray mp4Film()
{
    QByteArray mvhd(8, '\0'); // created, changed
    be32(mvhd, 1000);         // time scale
    be32(mvhd, 125000);       // duration: 125 s
    mvhd.append(QByteArray(80, '\0'));
    QByteArray tkhd(72, '\0');
    be32(tkhd, 1920 << 16);
    be32(tkhd, 1080 << 16);
    QByteArray stsd;
    be32(stsd, 1); // one entry
    QByteArray entry("avc1");
    entry.append(QByteArray(78, '\0'));
    QByteArray sized;
    be32(sized, quint32(4 + entry.size()));
    stsd.append(sized + entry);
    QByteArray hdlr(4, '\0');
    hdlr.append("vide").append(QByteArray(12, '\0')).append("Video\0", 6);
    const QByteArray trak = box("trak", fullBox("tkhd", 0, tkhd)
                                            + box("mdia", fullBox("hdlr", 0, hdlr) + box("minf", box("stbl", fullBox("stsd", 0, stsd)))));
    auto tag = [](const char *type, const QByteArray &value) {
        QByteArray data;
        be32(data, 1); // UTF-8
        be32(data, 0);
        return box(type, box("data", data + value));
    };
    const QByteArray ilst = box("ilst", tag("\xa9nam", "Beach day") + tag("\xa9" "ART", "Danny"));
    const QByteArray udta = box("udta", fullBox("meta", 0, QByteArray() + ilst).mid(0));
    return box("ftyp", QByteArray("isom\0\0\x02\0isomavc1", 16)) + box("moov", fullBox("mvhd", 0, mvhd) + trak + udta)
           + box("mdat", QByteArray(4096, '\x11'));
}

QByteArray syncsafe(quint32 v)
{
    return QByteArray(1, char((v >> 21) & 0x7F)) + char((v >> 14) & 0x7F) + char((v >> 7) & 0x7F) + char(v & 0x7F);
}

QByteArray mp3Song(const QByteArray &cover)
{
    auto frame = [](const char *id, const QByteArray &body) {
        QByteArray f(id, 4);
        be32(f, quint32(body.size()));
        return f + QByteArray(2, '\0') + body;
    };
    const QByteArray frames = frame("TIT2", QByteArray("\3Song title", 11)) + frame("TPE1", QByteArray("\3The band", 9))
                              + frame("TALB", QByteArray("\3First album", 12)) + frame("TYER", QByteArray("\0" "2024", 5))
                              + frame("APIC", QByteArray("\0image/png\0\3\0", 13) + cover);
    QByteArray out = QByteArray("ID3\3\0\0", 6) + syncsafe(quint32(frames.size())) + frames;
    for (int i = 0; i < 400; ++i) { // 128 kbit/s MPEG-1 layer III frames: 400 of them, about 10 s
        QByteArray f(417, '\x55');
        f[0] = char(0xFF);
        f[1] = char(0xFB);
        f[2] = char(0x90);
        f[3] = char(0x00);
        out += f;
    }
    return out;
}

QByteArray oggSong()
{
    QByteArray id("\x01vorbis", 7);
    le32(id, 0); // version
    id.append(char(2));
    le32(id, 44100);
    id.append(QByteArray(13, '\0')).append(char(1));
    QByteArray comments("\x03vorbis", 7);
    le32(comments, 4);
    comments.append("test");
    le32(comments, 2);
    for (const QByteArray &c : {QByteArray("TITLE=Night song"), QByteArray("ARTIST=Someone")}) {
        le32(comments, quint32(c.size()));
        comments.append(c);
    }
    comments.append(char(1));
    return oggPage(0x02, 0, id) + oggPage(0x00, 1, comments) + oggPage(0x04, 2, QByteArray(100, 'b'));
}

void mediaAndOffice()
{
    const int mp4Type = filetypes::typeNumber(QStringLiteral("mp4")), mp3Type = filetypes::typeNumber(QStringLiteral("mp3"));
    const QByteArray film = mp4Film();
    const mediainfo::Info f = mediainfo::read(mp4Type, film, quint64(film.size()));
    report(f.durationMs == 125000 && f.width == 1920 && f.height == 1080 && f.codec == QLatin1String("H.264") && f.title == QLatin1String("Beach day"),
           QStringLiteral("previews: an MP4 film's length, size, codec and title"), mediainfo::describe(f).replace(QLatin1Char('\n'), QLatin1String(" / ")));

    const QByteArray cover = picture("PNG");
    const QByteArray song = mp3Song(cover);
    const mediainfo::Info m = mediainfo::read(mp3Type, song, quint64(song.size()));
    report(m.title == QLatin1String("Song title") && m.artist == QLatin1String("The band") && m.album == QLatin1String("First album")
               && m.year == QLatin1String("2024") && m.cover == cover && m.sampleRate == 44100 && m.durationMs > 9000 && m.durationMs < 12000,
           QStringLiteral("previews: an MP3's tags, its cover and about how long it is"), mediainfo::describe(m).replace(QLatin1Char('\n'), QLatin1String(" / ")));

    const QByteArray night = oggSong();
    const mediainfo::Info o = mediainfo::read(filetypes::typeNumber(QStringLiteral("ogg")), night, quint64(night.size()));
    report(o.codec == QLatin1String("Vorbis") && o.title == QLatin1String("Night song") && o.artist == QLatin1String("Someone") && o.sampleRate == 44100
               && o.channels == 2,
           QStringLiteral("previews: an Ogg song's codec and tags"), mediainfo::describe(o).replace(QLatin1Char('\n'), QLatin1String(" / ")));

    const QByteArray sound = wav();
    const mediainfo::Info w = mediainfo::read(filetypes::typeNumber(QStringLiteral("wav")), sound, quint64(sound.size()));
    report(w.durationMs == 1000 && w.sampleRate == 44100 && w.channels == 1, QStringLiteral("previews: how long a WAV is"), mediainfo::describe(w));

    // Office files: their text, in order.
    const QByteArray docx = zip({{"[Content_Types].xml", "<Types/>"},
                                 {"word/document.xml", "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>"
                                                       "<w:p><w:r><w:t>Dear landlord,</w:t></w:r></w:p><w:p><w:r><w:t>the rent </w:t></w:r>"
                                                       "<w:r><w:t>is paid.</w:t></w:r></w:p></w:body></w:document>"}});
    report(officetext::text(filetypes::typeNumber(QStringLiteral("docx")), docx) == QLatin1String("Dear landlord,\nthe rent is paid."),
           QStringLiteral("previews: a Word document's text"), officetext::text(filetypes::typeNumber(QStringLiteral("docx")), docx));
    const QByteArray odt = zip({{"mimetype", "application/vnd.oasis.opendocument.text"},
                                {"content.xml", "<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
                                                "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\"><office:body><office:text>"
                                                "<text:h>Shopping</text:h><text:p>Milk<text:s/>and <text:span>bread</text:span></text:p>"
                                                "</office:text></office:body></office:document-content>"}});
    report(officetext::text(filetypes::typeNumber(QStringLiteral("odt")), odt) == QLatin1String("Shopping\nMilk and bread"),
           QStringLiteral("previews: an OpenDocument text's words"), officetext::text(filetypes::typeNumber(QStringLiteral("odt")), odt));
    const QByteArray pptx = zip({{"ppt/slides/slide2.xml", "<p:sld xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" xmlns:p=\"x\">"
                                                           "<a:p><a:r><a:t>Second</a:t></a:r></a:p></p:sld>"},
                                 {"ppt/slides/slide1.xml", "<p:sld xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" xmlns:p=\"x\">"
                                                           "<a:p><a:r><a:t>First</a:t></a:r></a:p></p:sld>"}});
    report(officetext::text(filetypes::typeNumber(QStringLiteral("pptx")), pptx) == QLatin1String("First\n\nSecond"),
           QStringLiteral("previews: a PowerPoint's slides in order"), officetext::text(filetypes::typeNumber(QStringLiteral("pptx")), pptx));
    const QByteArray xlsx = zip({{"xl/sharedStrings.xml", "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
                                                          "<si><t>Name</t></si><si><t>Total</t></si></sst>"}});
    report(officetext::text(filetypes::typeNumber(QStringLiteral("xlsx")), xlsx) == QLatin1String("Name\nTotal"),
           QStringLiteral("previews: the words in an Excel sheet"));

    bool complete = false;
    const QVector<officetext::Entry> entries = officetext::list(docx, 100, &complete);
    report(entries.size() == 2 && complete && entries[1].path == QLatin1String("word/document.xml") && entries[1].size > 100,
           QStringLiteral("previews: what's in a ZIP"), QString::number(entries.size()));

    // XML that would grow without end if every entity were expanded: it stops, quickly.
    QByteArray bomb = "<?xml version=\"1.0\"?><!DOCTYPE d [<!ENTITY a \"aaaaaaaaaa\">";
    for (int i = 1; i < 12; ++i)
        bomb += "<!ENTITY " + QByteArray(1, char('a' + i)) + " \"" + QByteArray("&").append(char('a' + i - 1)).append(';').repeated(10) + "\">";
    bomb += "]><w:document xmlns:w=\"x\"><w:p><w:t>&l;</w:t></w:p></w:document>";
    QElapsedTimer timer;
    timer.start();
    const QString exploded = officetext::text(filetypes::typeNumber(QStringLiteral("docx")), zip({{"word/document.xml", bomb}}));
    report(timer.elapsed() < 5000 && exploded.size() <= 4000, QStringLiteral("previews: an XML bomb is cut short"),
           QStringLiteral("%1 ms, %2 characters").arg(timer.elapsed()).arg(exploded.size()));

    // Damaged ones: random bytes, cut-off copies and flipped bytes never crash the readers.
    QRandomGenerator rnd(42);
    int runs = 0;
    for (const QByteArray &sample : {film, song, night, sound, docx, odt}) {
        for (int round = 0; round < 200; ++round) {
            QByteArray broken = sample.left(rnd.bounded(int(sample.size())) + 1);
            for (int k = 0; k < 20 && !broken.isEmpty(); ++k)
                broken[rnd.bounded(int(broken.size()))] = char(rnd.bounded(256));
            for (const QString &id : {QStringLiteral("mp4"), QStringLiteral("mp3"), QStringLiteral("ogg"), QStringLiteral("wav"), QStringLiteral("avi")})
                mediainfo::read(filetypes::typeNumber(id), broken, quint64(sample.size()));
            for (const QString &id : {QStringLiteral("docx"), QStringLiteral("odt"), QStringLiteral("pptx"), QStringLiteral("xlsx")})
                officetext::text(filetypes::typeNumber(id), broken);
            officetext::list(broken, 50);
            ++runs;
        }
    }
    report(runs == 1200, QStringLiteral("previews: 1200 damaged files read without a crash"));
}

void lostFilesTests()
{
    QTemporaryDir dir;
    finding(dir.path());
    damage();
    names();
    fuzz();
    fileSystems(dir.path());
    fileSystemFuzz();
    mediaAndOffice();
}
