// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Find Lost Files, finding files by their contents: real files of every kind it knows (made
// with Qt, libarchive, or by hand where nothing here writes them) dropped at known places in
// a stretch of random bytes, then found again, measured to the byte, saved and compared. Also
// damaged and cut-off files, bad spots, Stop, and random damage that must never crash it.

#include "testkit.h"

#include "../src/lostfiles/enhance.h"
#include "../src/lostfiles/jpegexact.h"
#include "../src/lostfiles/exif.h"
#include "../src/lostfiles/filetypes.h"
#include "../src/usb/imagesource.h"
#include "../src/lostfiles/lookcopies.h"
#include "../src/lostfiles/lostfs.h"
#include "../src/backup/rescuecopy.h"
#include "../src/lostfiles/hitfiles.h"
#include "../src/lostfiles/lostscan.h"
#include "../src/lostfiles/lostsource.h"
#include "../src/lostfiles/mediainfo.h"
#include "../src/lostfiles/officetext.h"
#include "../src/lostfiles/ole.h"
#include "../src/lostfiles/places.h"
#include "../src/lostfiles/foundreport.h"
#include "../src/lostfiles/pictures.h"
#include "../src/lostfiles/structure.h"
#include "../src/lostfiles/repair.h"
#include "../src/lostfiles/thumbcache.h"
#include "../src/lostfiles/ransomware.h"
#include "../src/lostfiles/readthrough.h"
#include "../src/lostfiles/btrfsrestore.h"
#include "../src/common/hostprocess.h"
#include "../src/lostfiles/textkinds.h"
#include "../src/lostfiles/preview.h"
#include "../src/lostfiles/previewloader.h"
#include "../src/lostfiles/viewer.h"
#include "../src/lostfiles/vdisk.h"
#include "../src/lostfiles/vshadow.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTransform>
#include <QImage>
#include <QMutex>
#include <QImageReader>
#include <QPainter>
#include <QProcess>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtEndian>

#include <archive.h>
#include <zlib.h>
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
QByteArray zip(const QList<QPair<QByteArray, QByteArray>> &files, bool packed = false)
{
    QByteArray out(4 * kMiB, '\0');
    size_t used = 0;
    archive *a = archive_write_new();
    archive_write_set_format_zip(a);
    archive_write_set_options(a, packed ? "zip:compression=deflate" : "zip:compression=store");
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

// --- The kinds added in 0.5.2, written by hand the way their makers write them.

QByteArray le(quint64 v, int bytes)
{
    QByteArray b(bytes, '\0');
    for (int i = 0; i < bytes; ++i)
        b[i] = char(v >> (8 * i));
    return b;
}

QByteArray be(quint64 v, int bytes)
{
    QByteArray b(bytes, '\0');
    for (int i = 0; i < bytes; ++i)
        b[bytes - 1 - i] = char(v >> (8 * i));
    return b;
}

// A little-endian TIFF with a 16 by 16 picture in one strip. `make` and an embedded JPEG make
// it a camera's raw file; `magic` is Olympus' or Panasonic's number, `canon` adds CR2's "CR".
QByteArray tiff(const QByteArray &make = {}, bool dng = false, quint16 magic = 42, bool canon = false)
{
    struct Tag {
        quint16 tag, type;
        quint32 count;
        QByteArray value;
    };
    const QByteArray jpeg = make.isEmpty() ? QByteArray() : picture("JPG");
    QByteArray pixels(16 * 16 * 3, '\0');
    for (int i = 0; i < pixels.size(); ++i)
        pixels[i] = char(i * 7);
    QVector<Tag> tags = {{256, 3, 1, le(16, 2)}, {257, 3, 1, le(16, 2)}, {258, 3, 3, le(8, 2) + le(8, 2) + le(8, 2)}, {259, 3, 1, le(1, 2)},
                         {262, 3, 1, le(2, 2)},  {273, 4, 1, {}},          {277, 3, 1, le(3, 2)},                        {278, 3, 1, le(16, 2)},
                         {279, 4, 1, le(quint64(pixels.size()), 4)}};
    if (!make.isEmpty()) {
        tags.insert(5, {271, 2, quint32(make.size() + 1), make + '\0'});
        tags << Tag{513, 4, 1, {}} << Tag{514, 4, 1, le(quint64(jpeg.size()), 4)};
    }
    if (dng)
        tags << Tag{50706, 1, 4, QByteArray("\x01\x04\x00\x00", 4)};
    const quint32 ifd = 16;
    quint32 data = ifd + 2 + 12 * quint32(tags.size()) + 4;
    QByteArray entries = le(quint64(tags.size()), 2), values;
    QVector<int> pixelsAt, jpegAt;
    for (const Tag &t : std::as_const(tags)) {
        entries += le(t.tag, 2) + le(t.type, 2) + le(t.count, 4);
        if (t.tag == 273)
            pixelsAt << int(entries.size());
        if (t.tag == 513)
            jpegAt << int(entries.size());
        if (t.value.size() <= 4) {
            entries += t.value + QByteArray(4 - t.value.size(), '\0');
        } else {
            entries += le(data + quint32(values.size()), 4);
            values += t.value + QByteArray(t.value.size() % 2, '\0');
        }
    }
    entries += le(0, 4);
    const quint32 pixelsOffset = data + quint32(values.size());
    const quint32 jpegOffset = pixelsOffset + quint32(pixels.size());
    for (int at : pixelsAt)
        entries.replace(at, 4, le(pixelsOffset, 4));
    for (int at : jpegAt)
        entries.replace(at, 4, le(jpegOffset, 4));
    QByteArray head = QByteArray("II", 2) + le(magic, 2) + le(ifd, 4) + (canon ? QByteArray("CR\x02\x00\x00\x00\x00\x00", 8) : QByteArray(8, '\0'));
    return head + entries + values + pixels + jpeg;
}

QByteArray raf()
{
    const QByteArray jpeg = picture("JPG");
    QByteArray sensor(3000, '\x5A');
    QByteArray head = QByteArray("FUJIFILMCCD-RAW 0201FF383501", 28) + QByteArray(56, '\0'); // up to byte 84
    const quint32 jpegAt = 160, sensorAt = jpegAt + quint32(jpeg.size());
    head += be(jpegAt, 4) + be(quint64(jpeg.size()), 4) + be(0, 4) + be(0, 4) + be(sensorAt, 4) + be(quint64(sensor.size()), 4);
    head += QByteArray(int(jpegAt) - head.size(), '\0');
    return head + jpeg + sensor;
}

// Photoshop: a 4 by 4 RGB picture, as it is or packed row by row (PackBits).
QByteArray psd(bool packed)
{
    QByteArray out = QByteArray("8BPS", 4) + be(1, 2) + QByteArray(6, '\0') + be(3, 2) + be(4, 4) + be(4, 4) + be(8, 2) + be(3, 2);
    out += be(0, 4) + be(0, 4) + be(0, 4); // no colour table, resources or layers
    out += be(packed ? 1 : 0, 2);
    if (!packed)
        return out + QByteArray(3 * 4 * 4, '\x77');
    const QByteArray row("\x03\x10\x20\x30\x40", 5); // 4 bytes as they are
    for (int r = 0; r < 12; ++r)
        out += be(quint64(row.size()), 2);
    for (int r = 0; r < 12; ++r)
        out += row;
    return out;
}

QByteArray rarNumberBytes(quint64 v)
{
    QByteArray b;
    do {
        b += char((v & 0x7F) | (v > 0x7F ? 0x80 : 0));
        v >>= 7;
    } while (v);
    return b;
}

QByteArray rar5()
{
    const auto block = [](const QByteArray &body) { // body: type, flags and the rest
        const QByteArray sized = rarNumberBytes(quint64(body.size())) + body;
        const quint32 crc = filetypes::crc32(reinterpret_cast<const uchar *>(sized.constData()), sized.size());
        return le(crc, 4) + sized;
    };
    const QByteArray data(2500, 'd');
    QByteArray file = rarNumberBytes(2) + rarNumberBytes(2) + rarNumberBytes(quint64(data.size())); // a file, with data
    file += rarNumberBytes(0) + rarNumberBytes(quint64(data.size())) + rarNumberBytes(0x20) + rarNumberBytes(0) + rarNumberBytes(1)
          + rarNumberBytes(9) + QByteArray("notes.txt");
    return QByteArray("Rar!\x1a\x07\x01\x00", 8) + block(rarNumberBytes(1) + rarNumberBytes(0) + rarNumberBytes(0)) + block(file) + data
         + block(rarNumberBytes(5) + rarNumberBytes(0) + rarNumberBytes(0));
}

QByteArray rar4()
{
    const auto block = [](quint8 type, quint16 flags, const QByteArray &rest) {
        const QByteArray body = QByteArray(1, char(type)) + le(flags, 2) + le(quint64(rest.size() + 7), 2) + rest;
        const quint32 crc = filetypes::crc32(reinterpret_cast<const uchar *>(body.constData()), body.size());
        return le(crc & 0xFFFF, 2) + body;
    };
    const QByteArray data(1800, 'e');
    const QByteArray name("old.txt");
    QByteArray file = le(quint64(data.size()), 4) + le(quint64(data.size()), 4) + QByteArray(1, '\3') + le(0, 4) + le(0, 4) + QByteArray(1, 29)
                    + QByteArray(1, '\x30') + le(quint64(name.size()), 2) + le(0x20, 4) + name;
    return QByteArray("Rar!\x1a\x07\x00", 7) + block(0x73, 0, QByteArray(6, '\0')) + block(0x74, 0x8000, file) + data + block(0x7B, 0, {});
}

// Matroska: elements with 8-byte sizes, which any size fits in. An "open" Segment is what a
// live recording leaves.
QByteArray ebml(const QByteArray &id, const QByteArray &payload)
{
    return id + QByteArray(1, '\x01') + be(quint64(payload.size()), 7) + payload;
}

QByteArray matroska(const char *docType, bool open = false)
{
    const QByteArray header = ebml("\x1A\x45\xDF\xA3", ebml("\x42\x86", "\x01") + ebml("\x42\x82", docType) + ebml("\x42\x87", "\x04"));
    const QByteArray info = ebml("\x15\x49\xA9\x66", ebml("\x2A\xD7\xB1", be(1000000, 3)));
    const QByteArray tracks = ebml("\x16\x54\xAE\x6B", ebml("\xAE", ebml("\xD7", "\x01") + ebml("\x86", "V_MS/VFW/FOURCC")));
    const QByteArray cluster = ebml("\x1F\x43\xB6\x75", ebml("\xE7", "\x00") + ebml("\xA3", QByteArray("\x81\x00\x00\x80", 4) + QByteArray(1200, 'v')));
    const QByteArray content = info + tracks + cluster;
    if (open)
        return header + QByteArray("\x18\x53\x80\x67\x01\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 12) + content;
    return header + ebml("\x18\x53\x80\x67", content);
}

QByteArray flac()
{
    const auto crc8 = [](const QByteArray &d) {
        quint8 c = 0;
        for (char ch : d) {
            c ^= quint8(ch);
            for (int b = 0; b < 8; ++b)
                c = quint8(c & 0x80 ? (c << 1) ^ 0x07 : c << 1);
        }
        return char(c);
    };
    const auto crc16 = [](const QByteArray &d) {
        quint32 c = 0;
        for (char ch : d) {
            c ^= quint32(quint8(ch)) << 8;
            for (int b = 0; b < 8; ++b)
                c = (c & 0x8000 ? (c << 1) ^ 0x8005 : c << 1) & 0xFFFF;
        }
        return be(c, 2);
    };
    // STREAMINFO: blocks of 192 samples, 44.1 kHz, stereo, 16-bit, 384 samples in all.
    QByteArray info = be(192, 2) + be(192, 2) + be(0, 3) + be(0, 3);
    const quint64 packed = (quint64(44100) << 44) | (quint64(1) << 41) | (quint64(15) << 36) | 384;
    info += be(packed, 8) + QByteArray(16, '\0');
    QByteArray out = QByteArray("fLaC") + QByteArray(1, char(0x80)) + be(34, 3) + info;
    for (int frame = 0; frame < 2; ++frame) {
        QByteArray f = QByteArray("\xFF\xF8", 2) + QByteArray(1, char(0x19)) + QByteArray(1, char(0x18)) + QByteArray(1, char(frame));
        f += crc8(f);
        f += QByteArray("\x00\x12\x34\x00\x56\x78", 6); // two CONSTANT subframes
        out += f + crc16(f);
    }
    return out;
}

QByteArray asf(bool video)
{
    const QByteArray headerGuid("\x30\x26\xB2\x75\x8E\x66\xCF\x11\xA6\xD9\x00\xAA\x00\x62\xCE\x6C", 16);
    const QByteArray dataGuid("\x36\x26\xB2\x75\x8E\x66\xCF\x11\xA6\xD9\x00\xAA\x00\x62\xCE\x6C", 16);
    const QByteArray fileGuid("\xA1\xDC\xAB\x8C\x47\xA9\xCF\x11\x8E\xE4\x00\xC0\x0C\x20\x53\x65", 16);
    const QByteArray streamGuid("\x91\x07\xDC\xB7\xB7\xA9\xCF\x11\x8E\xE6\x00\xC0\x0C\x20\x53\x65", 16);
    const QByteArray videoGuid("\xC0\xEF\x19\xBC\x4D\x5B\xCF\x11\xA8\xFD\x00\x80\x5F\x5C\x44\x2B", 16);
    const QByteArray audioGuid("\x40\x9E\x69\xF8\x4D\x5B\xCF\x11\xA8\xFD\x00\x80\x5F\x5C\x44\x2B", 16);
    const QByteArray packets(4000, 'p');
    const quint64 headerSize = 30 + 104 + 78, dataSize = 50 + quint64(packets.size()), indexSize = 300;
    const quint64 total = headerSize + dataSize + indexSize;
    QByteArray file = fileGuid + le(104, 8) + QByteArray(16, 'i') + le(total, 8) + QByteArray(104 - 48, '\0');
    QByteArray stream = streamGuid + le(78, 8) + (video ? videoGuid : audioGuid) + QByteArray(78 - 40, '\0');
    QByteArray out = headerGuid + le(headerSize, 8) + le(2, 4) + QByteArray("\x01\x02", 2) + file + stream;
    out += dataGuid + le(dataSize, 8) + QByteArray(16, 'i') + le(1, 8) + QByteArray("\x01\x01", 2) + packets;
    return out + QByteArray(int(indexSize), 'x');
}

// A compound file (OLE2) as Office 97-2003 writes them: 512-byte sectors, a big stream in
// sectors of its own and a small one in the mini stream.
QByteArray compoundFile(const QString &bigName, const QByteArray &big, const QString &smallName, const QByteArray &small)
{
    const quint32 kEnd = 0xFFFFFFFE, kFree = 0xFFFFFFFF, kFatSector = 0xFFFFFFFD;
    const int bigSectors = int((big.size() + 511) / 512), miniSectors = int((small.size() + 63) / 64);
    // Sectors: 0 the table, 1 the directory, 2 the mini table, 3 the mini stream, then the big stream.
    QVector<quint32> fat(128, kFree);
    fat[0] = kFatSector;
    fat[1] = kEnd;
    fat[2] = kEnd;
    fat[3] = kEnd;
    for (int i = 0; i < bigSectors; ++i)
        fat[4 + i] = i + 1 < bigSectors ? quint32(5 + i) : kEnd;
    QByteArray header("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8);
    header += QByteArray(16, '\0') + le(0x3E, 2) + le(3, 2) + le(0xFFFE, 2) + le(9, 2) + le(6, 2) + QByteArray(6, '\0') + le(0, 4) + le(1, 4)
            + le(1, 4) + le(0, 4) + le(4096, 4) + le(2, 4) + le(1, 4) + le(kEnd, 4) + le(0, 4) + le(0, 4);
    for (int i = 1; i < 109; ++i)
        header += le(kFree, 4);
    QByteArray fatSector;
    for (quint32 v : fat)
        fatSector += le(v, 4);
    const auto entry = [](const QString &name, int type, quint32 child, quint32 right, quint32 start, quint64 size) {
        QByteArray e(128, '\0');
        const QByteArray utf16 = QByteArray(reinterpret_cast<const char *>(name.utf16()), name.size() * 2) + QByteArray(2, '\0');
        e.replace(0, utf16.size(), utf16);
        e.replace(64, 2, le(quint64(utf16.size()), 2));
        e[66] = char(type);
        e[67] = 1;
        e.replace(68, 4, le(0xFFFFFFFF, 4));
        e.replace(72, 4, le(right, 4));
        e.replace(76, 4, le(child, 4));
        e.replace(116, 4, le(start, 4));
        e.replace(120, 8, le(size, 8));
        return e;
    };
    QByteArray dir = entry(QStringLiteral("Root Entry"), 5, 1, 0xFFFFFFFF, 3, quint64(miniSectors) * 64);
    dir += entry(bigName, 2, 0xFFFFFFFF, 2, 4, quint64(big.size()));
    dir += entry(smallName, 2, 0xFFFFFFFF, 0xFFFFFFFF, 0, quint64(small.size()));
    dir += QByteArray(128, '\0');
    QByteArray miniFat;
    for (int i = 0; i < 128; ++i)
        miniFat += le(i < miniSectors ? (i + 1 < miniSectors ? quint32(i + 1) : kEnd) : kFree, 4);
    const QByteArray miniStream = small + QByteArray(512 - small.size(), '\0');
    QByteArray bigData = big + QByteArray(bigSectors * 512 - big.size(), '\0');
    return header + fatSector + dir + miniFat + miniStream + bigData;
}

// gzip with the data stored as it is (deflate's "stored" blocks): no zlib needed to make it.
QByteArray gzipped(const QByteArray &data)
{
    QByteArray out("\x1F\x8B\x08\x08\x00\x00\x00\x00\x00\x03", 10);
    out += "notes.txt";
    out += '\0';
    for (qsizetype at = 0; at < data.size() || at == 0; at += 65535) {
        const QByteArray part = data.mid(at, 65535);
        const bool last = at + 65535 >= data.size();
        out += char(last ? 1 : 0) + le(quint64(part.size()), 2) + le(quint64(~quint16(part.size())) & 0xFFFF, 2) + part;
        if (last)
            break;
    }
    return out + le(filetypes::crc32(reinterpret_cast<const uchar *>(data.constData()), data.size()), 4) + le(quint64(data.size()), 4);
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
        {QStringLiteral("tif"), tiff()},
        {QStringLiteral("nef"), tiff("NIKON CORPORATION")},
        {QStringLiteral("arw"), tiff("SONY")},
        {QStringLiteral("dng"), tiff("Apple", true)},
        {QStringLiteral("cr2"), tiff("Canon", false, 42, true)},
        {QStringLiteral("orf"), tiff("OLYMPUS", false, 0x4F52)},
        {QStringLiteral("rw2"), tiff("Panasonic", false, 0x55)},
        {QStringLiteral("raf"), raf()},
        {QStringLiteral("psd"), psd(false)},
        {QStringLiteral("psd"), psd(true)},
        {QStringLiteral("rar"), rar5()},
        {QStringLiteral("rar"), rar4()},
        {QStringLiteral("mkv"), matroska("matroska")},
        {QStringLiteral("webm"), matroska("webm")},
        {QStringLiteral("flac"), flac()},
        {QStringLiteral("wmv"), asf(true)},
        {QStringLiteral("wma"), asf(false)},
        {QStringLiteral("doc"), compoundFile(QStringLiteral("WordDocument"), QByteArray(5000, 'w'), QStringLiteral("\x05SummaryInformation"), QByteArray(200, 's'))},
        {QStringLiteral("xls"), compoundFile(QStringLiteral("Workbook"), QByteArray(4200, 'x'), QStringLiteral("\x05SummaryInformation"), QByteArray(100, 's'))},
        {QStringLiteral("ppt"), compoundFile(QStringLiteral("PowerPoint Document"), QByteArray(4500, 'p'), QStringLiteral("Current User"), QByteArray(60, 'u'))},
        {QStringLiteral("msg"), compoundFile(QStringLiteral("__substg1.0_1000001F"), QByteArray(4800, 'b'), QStringLiteral("__substg1.0_0037001F"), QByteArray(40, 't'))},
        {QStringLiteral("gz"), gzipped(QByteArray(70000, 'z'))},
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
    // SHA256SUMS, the way sha256sum writes it: each saved file's fingerprint, checked here.
    QFile sums(savedTo + QStringLiteral("/SHA256SUMS"));
    int matching = 0, listed = 0;
    if (sums.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : sums.readAll().split('\n')) {
            if (line.size() < 67 || line.mid(64, 2) != "  ")
                continue;
            ++listed;
            QFile saved(savedTo + QLatin1Char('/') + QString::fromUtf8(line.mid(66)));
            if (saved.open(QIODevice::ReadOnly) && QCryptographicHash::hash(saved.readAll(), QCryptographicHash::Sha256).toHex() == line.left(64))
                ++matching;
        }
    }
    report(listed == found.size() && matching == listed, QStringLiteral("with SHA256SUMS: each saved file's fingerprint, right"),
           QStringLiteral("%1 listed, %2 right").arg(listed).arg(matching));
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
    for (; rounds < 6000; ++rounds) {
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

QByteArray unpackImage(const QString &path)
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

// A big test image that's mostly zeros, unpacked keeping only the 64 KiB pieces that aren't:
// the shadow copies' gigabyte volume takes a few megabytes of memory this way.
struct SparseImage {
    static constexpr quint64 kPiece = 64 * 1024;
    quint64 size = 0;
    QHash<quint64, QByteArray> pieces;

    static SparseImage unpackImage(const QString &path)
    {
        SparseImage out;
        QString error;
        const std::unique_ptr<ImageSource> image = ImageSource::open(path, &error);
        QByteArray piece(qsizetype(kPiece), Qt::Uninitialized);
        for (bool more = image != nullptr; more;) {
            qsizetype filled = 0;
            for (qint64 n = 1; filled < piece.size() && n > 0; filled += qsizetype(std::max<qint64>(n, 0)))
                n = image->read(piece.data() + filled, piece.size() - filled);
            more = filled == piece.size();
            if (piece.left(filled).count('\0') != filled)
                out.pieces.insert(out.size / kPiece, piece.left(filled));
            out.size += quint64(filled);
        }
        return out;
    }

    std::shared_ptr<lost::Source> source() const
    {
        struct Layer : lost::Layer {
            SparseImage image;
            quint64 size() const override { return image.size; }
            bool read(quint64 offset, char *buf, quint64 len) override
            {
                while (len > 0) {
                    const quint64 in = offset % kPiece, n = std::min(len, kPiece - in);
                    const QByteArray piece = image.pieces.value(offset / kPiece);
                    const quint64 have = quint64(piece.size()) > in ? std::min<quint64>(n, quint64(piece.size()) - in) : 0;
                    memcpy(buf, piece.constData() + in, have);
                    memset(buf + have, 0, n - have);
                    offset += n;
                    buf += n;
                    len -= n;
                }
                return true;
            }
        };
        auto layer = std::make_unique<Layer>();
        layer->image = *this;
        return lost::Source::fromLayer(std::move(layer));
    }
};

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
        const QByteArray image = unpackImage(data + name + QStringLiteral(".img.zst"));
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
        auto source = lost::Source::fromMemory(unpackImage(data + QStringLiteral("ntfs-linux.img.zst")));
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

    // NTFS's change journal, on a stick Windows used with it on (journal-job.cmd): a folder and its
    // files deleted and their records taken by new files, so only the journal still has their
    // names; and a file Linux's driver deleted, taking its name away, named again from the journal.
    {
        auto source = lost::Source::fromMemory(unpackImage(data + QStringLiteral("ntfs-journal.img.zst")));
        const QVector<lost::Found> found = quickScan(source);
        int namesOnly = 0;
        for (const lost::Found &f : found)
            namesOnly += f.nameOnly;
        report(namesOnly == 2, QStringLiteral("ntfs journal: only the files whose records were taken are listed by name alone"), QString::number(namesOnly));
        for (const Expected &e : expected.value(QStringLiteral("ntfs-journal"))) {
            const QString folder = QStringLiteral("WINJRNL") + (e.path.contains(QLatin1Char('/')) ? QLatin1Char('/') + e.path.section(QLatin1Char('/'), 0, -2) : QString());
            const lost::Found *match = nullptr;
            for (const lost::Found &f : found) {
                if (f.name == e.path.section(QLatin1Char('/'), -1) && f.folder == folder)
                    match = &f;
            }
            const QString what = QStringLiteral("ntfs journal: ") + e.path;
            report(match && match->deleted == e.deleted, what + QStringLiteral(" is listed where it was"));
            if (!match)
                continue;
            if (e.sha256 == "-") {
                const QDateTime when = QDateTime::fromMSecsSinceEpoch(match->deletedAt);
                report(match->nameOnly && match->condition == lost::Condition::Overwritten && when.date().year() == 2026
                           && match->note.contains(QLatin1String("change journal")),
                       what + QStringLiteral(": by name alone, with when it was deleted"), when.toString(Qt::ISODate));
                continue;
            }
            lost::FileReader reader(source, *match);
            report(!match->nameOnly && match->size == e.size
                       && QCryptographicHash::hash(reader.head(qint64(match->size)), QCryptographicHash::Sha256).toHex() == e.sha256,
                   what + QStringLiteral(" comes back whole"));
            if (e.path == QLatin1String("Trip/beach.jpg"))
                report(match->deletedAt > 0, what + QStringLiteral(": the journal says when it was deleted"));
            if (e.path == QLatin1String("keep.txt"))
                report(!match->nameGuessed && match->note.contains(QLatin1String("change journal")), what + QStringLiteral(": its name comes from the journal"),
                       match->note);
        }
        // Saving: the files are saved, the names alone are listed in the summary.
        QVector<lost::Found> deleted;
        for (const lost::Found &f : found) {
            if (f.deleted)
                deleted << f;
        }
        lost::Saver saver(source, deleted, dir);
        QString base;
        int saved = -1;
        QObject::connect(&saver, &lost::Saver::finished, [&](int n, int, const QString &folder, const QString &) {
            saved = n;
            base = folder;
        });
        saver.run();
        QFile summary(base + QStringLiteral("/What was saved.txt"));
        const QString text = summary.open(QIODevice::ReadOnly) ? QString::fromUtf8(summary.readAll()) : QString();
        report(saved == 2 && text.contains(QLatin1String("WINJRNL/Drafts/plan.txt")) && !QFile::exists(base + QStringLiteral("/WINJRNL/Drafts/plan.txt")),
               QStringLiteral("ntfs journal: what's only a name isn't saved, and the summary says so"), QString::number(saved));
        QDir(base).removeRecursively();
    }

    // Partition tables: the same file system inside an MBR and a GPT drive.
    const QByteArray fat16 = unpackImage(data + QStringLiteral("fat16.img.zst"));
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

QByteArray usnRecord(int version, quint64 record, quint16 sequence, quint64 parent, quint32 reason, const QString &name);

// Damaged file systems: every byte of the tables and folders flipped at random, many times
// over. It must never crash or loop, whatever it lists.
void fileSystemFuzz()
{
    const QString data = QStringLiteral(SOURCE_DIR "/tests/data/lostfs/");
    QRandomGenerator rnd(20261010);
    // DISKANVIL_FUZZ_ROUNDS=1000 for a longer run by hand.
    const int rounds = qEnvironmentVariableIsSet("DISKANVIL_FUZZ_ROUNDS") ? qEnvironmentVariableIntValue("DISKANVIL_FUZZ_ROUNDS") : 60;
    int runs = 0;
    for (const QString &name : {QStringLiteral("fat12"), QStringLiteral("fat32"), QStringLiteral("exfat"), QStringLiteral("ntfs")}) {
        const QByteArray clean = unpackImage(data + name + QStringLiteral(".img.zst"));
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

    // Windows put this one's MFT 22 MB in: the flips go there, where the change journal's record is.
    {
        const QByteArray clean = unpackImage(data + QStringLiteral("ntfs-journal.img.zst"));
        const uchar *b = reinterpret_cast<const uchar *>(clean.constData());
        const qsizetype mft = clean.size() > 0x40 ? qsizetype(qFromLittleEndian<quint64>(b + 0x30) * qFromLittleEndian<quint16>(b + 11) * b[13]) : 0;
        int journalRuns = 0;
        for (int round = 0; round < rounds && mft > 0 && mft + 96 * 1024 <= clean.size(); ++round) {
            QByteArray broken = clean;
            for (int k = 0; k < 200; ++k)
                broken[mft + rnd.bounded(96 * 1024)] = char(rnd.bounded(256));
            quickScan(lost::Source::fromMemory(broken));
            ++journalRuns;
        }
        report(journalRuns == rounds, QStringLiteral("ntfs journal: %1 damaged MFTs read without a crash").arg(journalRuns));
    }
    // And the journal's records themselves, cut short and flipped.
    QByteArray records;
    for (int i = 0; i < 40; ++i)
        records += usnRecord(i % 3 == 0 ? 3 : 2, quint64(64 + i), quint16(1 + i % 4), 5, i % 2 ? 0x200 : 0x100, QStringLiteral("file %1.txt").arg(i));
    int parsed = 0;
    for (int round = 0; round < rounds * 20; ++round) {
        QByteArray broken = records.left(rnd.bounded(int(records.size()) + 1));
        for (int k = 0; k < 20 && !broken.isEmpty(); ++k)
            broken[rnd.bounded(int(broken.size()))] = char(rnd.bounded(256));
        parsed += int(lost::readJournal(broken).size() >= 0);
    }
    report(parsed == rounds * 20, QStringLiteral("ntfs journal: %1 damaged journals read without a crash").arg(parsed));
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
    QVector<lost::Found> found = quickScan(source, &volumes);
    for (const lost::Volume &v : std::as_const(volumes))
        out << v.type << " " << v.label << " at " << v.where.start << ": " << v.files << " files, " << v.deleted << " deleted" << Qt::endl;
    // DISKANVIL_SCAN_DEEP=1: the files found by their contents too.
    if (qEnvironmentVariableIsSet("DISKANVIL_SCAN_DEEP")) {
        lost::DeepScan deep(source);
        QObject::connect(&deep, &lost::DeepScan::found, [&found](const QVector<lost::Found> &batch) { found += batch; });
        deep.run();
    }
    // DISKANVIL_SCAN_PREVIEWS=1: what the preview makes of each one (here, not in the sandbox).
    const bool previews = qEnvironmentVariableIsSet("DISKANVIL_SCAN_PREVIEWS");
    for (const lost::Found &f : found) {
        out << (f.deleted ? "deleted " : "        ") << f.folder << "/" << f.name << "  " << f.size << " bytes, condition "
            << int(f.condition) << (f.checked ? " checked" : "") << (f.nameGuessed ? " name-guessed" : "")
            << (f.piecesGuessed ? " pieces-guessed" : "") << (f.origin == lost::Origin::Contents ? " by-contents" : "")
            << (f.note.isEmpty() ? QString() : QStringLiteral("  (%1)").arg(f.note)) << Qt::endl;
        if (previews && !f.nameOnly) {
            preview::Request r;
            r.type = f.type;
            r.name = f.name;
            r.size = f.size;
            r.pixels = 480;
            r.data = lost::FileReader(source, f).head(preview::kMostData);
            const preview::Result result = preview::make(r);
            out << "        preview: " << (result.image.isNull() ? QStringLiteral("no picture") : QStringLiteral("%1x%2").arg(result.image.width()).arg(result.image.height()))
                << ", " << result.text.section(QLatin1Char('\n'), 0, 0).left(60) << Qt::endl;
        }
    }
    return 0;
}

// Windows' shadow copies, on a disk Windows made and took two copies of (shadow-job.ps1): each
// copy reads back the way Windows itself read it, the same files and the same bytes (but where
// Windows leaves them undefined: shadow-finish.sh).
void shadowCopies()
{
    const QString data = QStringLiteral(SOURCE_DIR "/tests/data/lostfs/");
    const SparseImage image = SparseImage::unpackImage(data + QStringLiteral("ntfs-shadow.img.zst"));
    const auto source = image.source();
    const lost::Extent where{0, source->size()};
    QFile list(data + QStringLiteral("shadow-expected.txt"));
    report(image.size > 0 && list.open(QIODevice::ReadOnly), QStringLiteral("shadow copies: the test disk and what Windows read from it"));
    struct Expected {
        QString id, time;
        quint64 size = 0;
        QByteArray digest;
        QSet<quint64> undefined;
        QVector<QPair<QString, QByteArray>> files;
    };
    QVector<Expected> expected;
    for (const QByteArray &line : list.readAll().split('\n')) {
        const QList<QByteArray> f = line.split('|');
        if (f.value(0) == "copy" && f.size() == 7) {
            Expected e{QString::fromUtf8(f[2]), QString::fromUtf8(f[3]), f[4].toULongLong(), f[5], {}, {}};
            for (const QByteArray &mib : f[6].split(','))
                e.undefined.insert(mib.toULongLong());
            expected << e;
        } else if (f.value(0) == "file" && f.size() == 4 && !expected.isEmpty()) {
            expected.back().files.push_back({QString::fromUtf8(f[2]), f[3]});
        }
    }

    QString problem;
    const QVector<vss::Copy> copies = vss::copies(*source, where, &problem);
    report(copies.size() == expected.size() && expected.size() == 2 && problem.isEmpty(), QStringLiteral("shadow copies: both of Windows' copies are found"),
           QStringLiteral("%1, %2").arg(copies.size()).arg(problem));
    for (int i = 0; i < std::min(copies.size(), expected.size()); ++i) {
        const Expected &e = expected[i];
        const QString what = QStringLiteral("shadow copy %1").arg(i + 1);
        const QString time = QDateTime::fromMSecsSinceEpoch(copies[i].created, QTimeZone::UTC).toString(Qt::ISODate).chopped(1);
        report(copies[i].id == e.id && time == e.time, what + QStringLiteral(": its ID and when it was taken"), copies[i].id + QLatin1Char(' ') + time);
        const auto copy = vss::open(source, where, i, &problem);
        report(copy != nullptr, what + QStringLiteral(": opens"), problem);
        if (!copy)
            continue;
        QHash<QString, lost::Found> files;
        for (const lost::Found &f : quickScan(copy)) {
            const QString folder = f.folder.section(QLatin1Char('/'), 1);
            if (!f.deleted)
                files.insert(folder.isEmpty() ? f.name : folder + QLatin1Char('/') + f.name, f);
        }
        for (const auto &[path, sha256] : e.files) {
            if (sha256 == "missing") {
                report(!files.contains(path), what + QStringLiteral(": %1 isn't in it, as Windows said").arg(path));
                continue;
            }
            const auto f = files.constFind(path);
            QByteArray got;
            if (f != files.cend())
                got = QCryptographicHash::hash(lost::FileReader(copy, *f).head(qint64(f->size)), QCryptographicHash::Sha256).toHex();
            report(got == sha256, what + QStringLiteral(": %1 is what Windows read").arg(path));
        }
        // The whole volume, a MiB at a time.
        QCryptographicHash digest(QCryptographicHash::Sha256);
        QByteArray buffer(qsizetype(kMiB), Qt::Uninitialized);
        for (quint64 mib = 0; mib * kMiB < e.size; ++mib) {
            const quint64 n = std::min<quint64>(kMiB, e.size - mib * kMiB);
            if (e.undefined.contains(mib))
                continue;
            copy->read(mib * kMiB, buffer.data(), n);
            digest.addData(QStringLiteral("mib %1 %2 %3\n")
                               .arg(mib)
                               .arg(n)
                               .arg(QString::fromLatin1(QCryptographicHash::hash(QByteArrayView(buffer.constData(), qsizetype(n)), QCryptographicHash::Sha256).toHex()))
                               .toUtf8());
        }
        report(digest.result().toHex() == e.digest, what + QStringLiteral(": every MiB is what Windows read"));
    }

    // Find Lost Files: the drive's quick scan sees the copies; looking in them lists the files that
    // aren't the same on the drive now, read from their copy, and saving brings back those bytes.
    QVector<lost::Volume> volumes;
    QHash<QString, QByteArray> now; // path on the drive -> SHA-256
    for (const lost::Found &f : quickScan(source, &volumes)) {
        if (!f.deleted) {
            const QString folder = f.folder.section(QLatin1Char('/'), 1);
            now.insert(folder.isEmpty() ? f.name : folder + QLatin1Char('/') + f.name,
                       QCryptographicHash::hash(lost::FileReader(source, f).head(qint64(f.size)), QCryptographicHash::Sha256).toHex());
        }
    }
    report(volumes.size() == 1 && volumes.value(0).copies.size() == 2 && volumes.value(0).records,
           QStringLiteral("shadow copies: the drive's scan sees Windows' two copies of it"));
    lost::CopyScan scan(source, volumes);
    QVector<lost::Found> fromCopies;
    QObject::connect(&scan, &lost::CopyScan::found, [&fromCopies](const QVector<lost::Found> &files) { fromCopies += files; });
    int listed = -1;
    QObject::connect(&scan, &lost::CopyScan::finished, [&listed](bool, int n, const QString &) { listed = n; });
    scan.run();
    for (int i = 0; i < std::min(copies.size(), expected.size()); ++i) {
        const QString top = volumes.value(0).label + QStringLiteral(" as it was on ")
                          + QDateTime::fromMSecsSinceEpoch(copies[i].created).toString(QStringLiteral("yyyy-MM-dd HH.mm.ss"));
        QStringList want, got, wrong;
        for (const auto &[path, sha256] : expected[i].files) {
            if (sha256 != "missing" && now.value(path) != sha256)
                want << path + (now.contains(path) ? QStringLiteral(" (changed)") : QStringLiteral(" (gone)"));
        }
        for (const lost::Found &f : std::as_const(fromCopies)) {
            if (f.folder != top && !f.folder.startsWith(top + QLatin1Char('/')))
                continue;
            const QString folder = f.folder.mid(top.size() + 1);
            const QString path = folder.isEmpty() ? f.name : folder + QLatin1Char('/') + f.name;
            const bool changed = f.note.startsWith(QLatin1String("It's different on the drive now"));
            got << path + (changed ? QStringLiteral(" (changed)") : QStringLiteral(" (gone)"));
            QByteArray sha;
            for (const auto &[p, h] : expected[i].files) {
                if (p == path)
                    sha = h;
            }
            const QByteArray bytes = lost::FileReader(source, f).head(qint64(f.size));
            if (!f.from || !f.deleted || QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != sha)
                wrong << path;
        }
        want.sort();
        got.sort();
        report(want == got && !want.isEmpty(), QStringLiteral("shadow copy %1: what's changed or gone since is listed, and only that").arg(i + 1),
               got.join(QStringLiteral(", ")) + QStringLiteral(" / expected ") + want.join(QStringLiteral(", ")));
        report(wrong.isEmpty(), QStringLiteral("shadow copy %1: each of them is read from the copy, as Windows read it").arg(i + 1), wrong.join(QStringLiteral(", ")));
    }
    report(listed == fromCopies.size(), QStringLiteral("shadow copies: the count adds up"));
    {
        QTemporaryDir out;
        lost::Saver saver(source, fromCopies, out.path());
        QString base;
        int saved = -1;
        QObject::connect(&saver, &lost::Saver::finished, [&](int n, int, const QString &folder, const QString &) {
            saved = n;
            base = folder;
        });
        saver.run();
        int same = 0;
        for (const lost::Found &f : std::as_const(fromCopies)) {
            QFile back(base + QLatin1Char('/') + f.folder + QLatin1Char('/') + f.name);
            same += back.open(QIODevice::ReadOnly) && back.readAll() == lost::FileReader(source, f).head(qint64(f.size));
        }
        report(saved == fromCopies.size() && same == saved, QStringLiteral("shadow copies: saved, their files come back from the copies"),
               QStringLiteral("%1 saved, %2 the same").arg(saved).arg(same));
    }

    // A volume with no copies has none, and says nothing's wrong.
    {
        auto plain = lost::Source::fromMemory(unpackImage(data + QStringLiteral("ntfs-windows.img.zst")));
        QString why;
        report(vss::copies(*plain, {0, plain->size()}, &why).isEmpty() && why.isEmpty(), QStringLiteral("shadow copies: none on a volume without any"));
    }
}

// Shadow copies' own records flipped at random: the catalog and every store's header, list and
// bitmaps. It must never crash or loop, whatever it reads.
void shadowCopyFuzz()
{
    const SparseImage clean = SparseImage::unpackImage(QStringLiteral(SOURCE_DIR "/tests/data/lostfs/ntfs-shadow.img.zst"));
    const auto source = clean.source();
    const lost::Extent where{0, source->size()};
    // Where the records are: the header, the catalog, and from it each store's blocks.
    QVector<quint64> hot = {0x1e00};
    QByteArray block(16384, Qt::Uninitialized);
    source->read(0x1e00, block.data(), 512);
    const quint64 catalog = qFromLittleEndian<quint64>(block.constData() + 48);
    hot << catalog;
    source->read(catalog, block.data(), 16384);
    for (int e = 128; e + 128 <= 16384; e += 128) {
        if (qFromLittleEndian<quint64>(block.constData() + e) == 3) {
            for (const int field : {8, 32, 48, 72})
                hot << qFromLittleEndian<quint64>(block.constData() + e + field);
        }
    }
    QRandomGenerator rnd(20261011);
    const int rounds = qEnvironmentVariableIsSet("DISKANVIL_FUZZ_ROUNDS") ? qEnvironmentVariableIntValue("DISKANVIL_FUZZ_ROUNDS") : 40;
    int runs = 0;
    for (int round = 0; round < rounds; ++round) {
        SparseImage broken = clean;
        for (int k = 0; k < 30; ++k) {
            const quint64 at = hot[rnd.bounded(int(hot.size()))] + quint64(rnd.bounded(k % 3 == 0 ? 128 : 16384));
            QByteArray &piece = broken.pieces[at / SparseImage::kPiece];
            if (piece.size() < qsizetype(SparseImage::kPiece))
                piece.resize(qsizetype(SparseImage::kPiece), '\0');
            piece[qsizetype(at % SparseImage::kPiece)] = char(rnd.bounded(256));
        }
        const auto damaged = broken.source();
        QString problem;
        const QVector<vss::Copy> copies = vss::copies(*damaged, where, &problem);
        for (int i = 0; i < copies.size(); ++i) {
            if (const auto copy = vss::open(damaged, where, i, &problem)) {
                quickScan(copy);
                for (int r = 0; r < 20; ++r)
                    copy->read(quint64(rnd.bounded(quint32(copy->size() / 512))) * 512, block.data(), 16384);
            }
        }
        ++runs;
    }
    report(runs == rounds, QStringLiteral("shadow copies: %1 damaged sets of records read without a crash").arg(runs));
}

// A compressed test file unpacked to `to`, sparse: the 64 KiB pieces that are only zeros are left
// as holes, so a big, mostly empty disk takes up little room (memory, when it's in /tmp).
bool unpackSparse(const QString &from, const QString &to)
{
    QString error;
    const std::unique_ptr<ImageSource> image = ImageSource::open(from, &error);
    QFile out(to);
    if (!image || !out.open(QIODevice::WriteOnly))
        return false;
    QByteArray piece(64 * 1024, Qt::Uninitialized);
    qint64 size = 0;
    for (bool more = true; more;) {
        qint64 filled = 0;
        for (qint64 n = 1; filled < piece.size() && n > 0; filled += std::max<qint64>(n, 0))
            n = image->read(piece.data() + filled, piece.size() - filled);
        more = filled == piece.size();
        if (piece.left(filled).count('\0') != filled && (!out.seek(size) || out.write(piece.constData(), filled) != filled))
            return false;
        size += filled;
    }
    return out.resize(size);
}

// Virtual machines' disk files (tests/data/vdisk/make.sh): the same small disk in every format and
// kind qemu-img writes reads back byte for byte, with its files found; the disks that only hold
// changes made to another one are refused, naming it.
void virtualDisks()
{
    const QString data = QStringLiteral(SOURCE_DIR "/tests/data/vdisk/");
    QTemporaryDir dir;
    int unpacked = 0;
    for (const QString &name : QDir(data).entryList({QStringLiteral("*.zst")}, QDir::Files))
        unpacked += unpackSparse(data + name, dir.filePath(name.chopped(4)));
    QFile sum(data + QStringLiteral("disk.sha256"));
    const QByteArray want = sum.open(QIODevice::ReadOnly) ? sum.readAll().trimmed() : QByteArray();
    report(unpacked >= 23 && !want.isEmpty(), QStringLiteral("virtual disks: the test disks unpack"), QString::number(unpacked));
    constexpr quint64 kDisk = 40 * kMiB;
    struct Case {
        const char *file, *format, *refused;
    };
    const Case cases[] = {
        {"dynamic.vdi", "VDI", nullptr},    {"fixed.vdi", "VDI", nullptr},          {"dynamic.vhd", "VHD", nullptr},
        {"fixed.vhd", "VHD", nullptr},      {"dynamic.vhdx", "VHDX", nullptr},      {"fixed.vhdx", "VHDX", nullptr},
        {"sparse.vmdk", "VMDK", nullptr},   {"stream.vmdk", "VMDK", nullptr},       {"split.vmdk", "VMDK", nullptr},
        {"flat.vmdk", "VMDK", nullptr},     {"plain.qcow2", "QCOW2", nullptr},      {"v2.qcow2", "QCOW2", nullptr},
        {"packed.qcow2", "QCOW2", nullptr}, {"zstd.qcow2", "QCOW2", nullptr},       {"subclusters.qcow2", "QCOW2", nullptr},
        {"child.qcow2", "QCOW2", "plain.qcow2"}, {"child.vmdk", "VMDK", "sparse.vmdk"},
        // Made by Windows itself (windows-job.ps1).
        {"win-dynamic.vhdx", "VHDX", nullptr}, {"win-dynamic.vhd", "VHD", nullptr}, {"win-fixed.vhd", "VHD", nullptr},
    };
    for (const Case &c : cases) {
        const QString name = QString::fromLatin1(c.file), path = dir.filePath(name);
        report(vdisk::formatOf(path) == QLatin1String(c.format), QStringLiteral("virtual disks: %1 is known as %2").arg(name, QLatin1String(c.format)),
               vdisk::formatOf(path));
        QString problem, warning;
        const auto disk = vdisk::open(path, &problem, &warning);
        if (c.refused) {
            report(!disk && problem.contains(QLatin1String(c.refused)), QStringLiteral("%1: refused on its own, naming the disk it needs").arg(name), problem);
            continue;
        }
        report(disk != nullptr && warning.isEmpty(), QStringLiteral("%1: opens").arg(name), problem + warning);
        if (!disk)
            continue;
        QCryptographicHash sha(QCryptographicHash::Sha256);
        QByteArray buffer(qsizetype(kMiB), Qt::Uninitialized);
        bool zerosAfter = true;
        for (quint64 at = 0, n = 0; at < disk->size(); at += n) {
            n = std::min<quint64>(kMiB, disk->size() - at);
            disk->read(at, buffer.data(), n);
            const quint64 inside = at < kDisk ? std::min(n, kDisk - at) : 0;
            sha.addData(QByteArrayView(buffer.constData(), qsizetype(inside)));
            zerosAfter = zerosAfter && QByteArrayView(buffer.constData() + inside, qsizetype(n - inside)).count('\0') == qsizetype(n - inside);
        }
        report(disk->size() >= kDisk && sha.result().toHex() == want && zerosAfter, QStringLiteral("%1: reads back as the disk, byte for byte").arg(name),
               QString::number(disk->size()));
        QVector<lost::Volume> volumes;
        QStringList files;
        for (const lost::Found &f : quickScan(disk, &volumes))
            files << f.name.toLower();
        files.sort();
        report(volumes.size() == 1 && volumes[0].label == QLatin1String("VDISKFAT") && files == QStringList({QStringLiteral("notes.txt"), QStringLiteral("numbers.txt")}),
               QStringLiteral("%1: Find Lost Files finds what's on it").arg(name), files.join(QStringLiteral(", ")));
    }
}

// Virtual machines' disk files with their headers and tables flipped at random (the first MiB and
// the last 64 KiB of each, where those are, and anywhere now and then): opening, reading all of
// the disk and looking through it must never crash or loop.
void virtualDiskFuzz()
{
    const QString data = QStringLiteral(SOURCE_DIR "/tests/data/vdisk/");
    QTemporaryDir dir;
    for (const QString &name : QDir(data).entryList({QStringLiteral("*.zst")}, QDir::Files))
        unpackSparse(data + name, dir.filePath(name.chopped(4)));
    QRandomGenerator rnd(20261012);
    const int rounds = qEnvironmentVariableIsSet("DISKANVIL_FUZZ_ROUNDS") ? qEnvironmentVariableIntValue("DISKANVIL_FUZZ_ROUNDS") / 10 + 1 : 4;
    int runs = 0;
    const QStringList files = {QStringLiteral("dynamic.vdi"), QStringLiteral("dynamic.vhd"), QStringLiteral("fixed.vhd"), QStringLiteral("dynamic.vhdx"),
                               QStringLiteral("sparse.vmdk"), QStringLiteral("stream.vmdk"), QStringLiteral("split-s001.vmdk"), QStringLiteral("plain.qcow2"),
                               QStringLiteral("packed.qcow2"), QStringLiteral("zstd.qcow2"), QStringLiteral("subclusters.qcow2")};
    for (const QString &name : files) {
        QFile original(dir.filePath(name));
        if (!original.open(QIODevice::ReadOnly))
            continue;
        const qint64 size = original.size();
        const QByteArray head = original.read(std::min<qint64>(size, qint64(kMiB)));
        original.seek(std::max<qint64>(0, size - 65536));
        const QByteArray tail = original.read(65536);
        original.close();
        // The split disk's descriptor names its extent file, so that one is changed in place.
        const QString broken = name.startsWith(QLatin1String("split")) ? dir.filePath(name) : dir.filePath(QStringLiteral("broken-") + name);
        if (broken != dir.filePath(name))
            QFile::copy(dir.filePath(name), broken);
        for (int round = 0; round < rounds; ++round) {
            QByteArray h = head, t = tail;
            for (int k = 0; k < 40; ++k) {
                if (k % 4 == 3)
                    t[rnd.bounded(int(t.size()))] = char(rnd.bounded(256));
                else
                    h[rnd.bounded(k % 2 ? 4096 : int(h.size()))] = char(rnd.bounded(256));
            }
            QFile f(broken);
            if (!f.open(QIODevice::ReadWrite))
                break;
            f.write(h);
            f.seek(std::max<qint64>(0, size - 65536));
            f.write(t);
            f.close();
            QString problem;
            const QString open = broken.endsWith(QLatin1String("split-s001.vmdk")) ? dir.filePath(QStringLiteral("split.vmdk")) : broken;
            if (const auto disk = vdisk::open(open, &problem)) {
                QByteArray buffer(qsizetype(kMiB), Qt::Uninitialized);
                for (quint64 at = 0; at < std::min<quint64>(disk->size(), 64 * kMiB); at += kMiB)
                    disk->read(at, buffer.data(), std::min<quint64>(kMiB, disk->size() - at));
                quickScan(disk);
            }
            ++runs;
        }
    }
    report(runs == rounds * int(files.size()), QStringLiteral("virtual disks: %1 damaged disk files read without a crash").arg(runs));
}

// For looking into shadow copies by hand: each copy of the NTFS volume an image holds (from its
// start), the way shadow-job.ps1 has Windows write it down.
int shadowImage(const QString &path)
{
    QString error;
    auto source = lost::Source::fromImage(path, &error);
    if (!source) {
        out << error << Qt::endl;
        return 1;
    }
    const lost::Extent where{0, source->size()};
    const QVector<vss::Copy> copies = vss::copies(*source, where, &error);
    if (!error.isEmpty())
        out << "problem: " << error << Qt::endl;
    for (int i = 0; i < copies.size(); ++i) {
        out << "copy " << copies[i].id << " " << QDateTime::fromMSecsSinceEpoch(copies[i].created, QTimeZone::UTC).toString(Qt::ISODate).chopped(1) << Qt::endl;
        auto copy = vss::open(source, where, i, &error);
        if (!copy) {
            out << "problem: " << error << Qt::endl;
            continue;
        }
        for (const lost::Found &f : quickScan(copy)) {
            if (f.deleted)
                continue;
            lost::FileReader reader(copy, f);
            out << "file " << f.folder.section(QLatin1Char('/'), 1) << (f.folder.contains(QLatin1Char('/')) ? "/" : "") << f.name << " "
                << QCryptographicHash::hash(reader.head(qint64(f.size)), QCryptographicHash::Sha256).toHex() << Qt::endl;
        }
        // DISKANVIL_SHADOW_BLOCKS=1:106,2:10: those MiBs of those copies, 16 KiB at a time.
        for (const QString &want : qEnvironmentVariable("DISKANVIL_SHADOW_BLOCKS").split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            if (want.section(QLatin1Char(':'), 0, 0).toInt() != i + 1)
                continue;
            const quint64 mib = want.section(QLatin1Char(':'), 1).toULongLong();
            QByteArray block(16384, Qt::Uninitialized);
            for (int b = 0; b < 64; ++b) {
                copy->read((mib << 20) + quint64(b) * 16384, block.data(), 16384);
                out << "block " << mib << " " << b << " "
                    << (block.count('\0') == block.size() ? QByteArray("zeros") : QCryptographicHash::hash(block, QCryptographicHash::Sha256).toHex()) << Qt::endl;
            }
        }
        QByteArray buffer(1024 * 1024, Qt::Uninitialized);
        for (quint64 at = 0, n = 0; at < copy->size(); at += n) {
            n = std::min<quint64>(buffer.size(), copy->size() - at);
            copy->read(at, buffer.data(), n);
            out << "mib " << at / (1024 * 1024) << " " << n << " "
                << QCryptographicHash::hash(QByteArrayView(buffer.constData(), qsizetype(n)), QCryptographicHash::Sha256).toHex() << Qt::endl;
        }
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

    // Who made them and when, and how they were worked on.
    const QByteArray docxMeta = zip({{"docProps/core.xml", "<cp:coreProperties xmlns:cp=\"x\" xmlns:dc=\"y\" xmlns:dcterms=\"z\">"
                                                           "<dc:title>Rent</dc:title><dc:creator>Sam Tester</dc:creator><cp:lastModifiedBy>Alex Other</cp:lastModifiedBy>"
                                                           "<cp:revision>14</cp:revision><cp:lastPrinted>2024-07-15T08:00:00Z</cp:lastPrinted>"
                                                           "<dc:subject>Flat</dc:subject><cp:keywords>rent; landlord</cp:keywords></cp:coreProperties>"},
                                     {"docProps/app.xml", "<Properties xmlns=\"x\"><Template>Letter.dotx</Template><TotalTime>125</TotalTime>"
                                                          "<Application>Microsoft Office Word</Application><Company>Acme</Company></Properties>"}});
    const QStringList docxFacts = officetext::properties(filetypes::typeNumber(QStringLiteral("docx")), docxMeta);
    const auto factOf = [](const QStringList &facts, const QString &label) {
        for (const QString &f : facts) {
            if (f.section(QLatin1Char('\t'), 0, 0) == label)
                return f.section(QLatin1Char('\t'), 1);
        }
        return QString();
    };
    report(factOf(docxFacts, QStringLiteral("Times saved")) == QLatin1String("14") && factOf(docxFacts, QStringLiteral("Editing time")) == QLatin1String("2 h 5 min")
               && factOf(docxFacts, QStringLiteral("Template")) == QLatin1String("Letter.dotx") && factOf(docxFacts, QStringLiteral("Last printed")).contains(QLatin1String("2024"))
               && factOf(docxFacts, QStringLiteral("Last changed by")) == QLatin1String("Alex Other") && factOf(docxFacts, QStringLiteral("Keywords")) == QLatin1String("rent; landlord"),
           QStringLiteral("documents: a Word file's saves, editing time, template, printing"), docxFacts.join(QStringLiteral(" | ")));
    const QByteArray odtMeta = zip({{"mimetype", "application/vnd.oasis.opendocument.text"},
                                    {"meta.xml", "<office:document-meta xmlns:office=\"o\" xmlns:meta=\"m\" xmlns:dc=\"d\"><office:meta>"
                                                 "<meta:initial-creator>Sam Tester</meta:initial-creator><meta:editing-cycles>3</meta:editing-cycles>"
                                                 "<meta:editing-duration>PT1H2M30S</meta:editing-duration><meta:print-date>2024-07-15T09:00:00</meta:print-date>"
                                                 "<meta:printed-by>Sam Tester</meta:printed-by></office:meta></office:document-meta>"}});
    const QStringList odtFacts = officetext::properties(filetypes::typeNumber(QStringLiteral("odt")), odtMeta);
    report(factOf(odtFacts, QStringLiteral("Times saved")) == QLatin1String("3") && factOf(odtFacts, QStringLiteral("Editing time")) == QLatin1String("1 h 3 min")
               && factOf(odtFacts, QStringLiteral("Printed by")) == QLatin1String("Sam Tester") && factOf(odtFacts, QStringLiteral("Last printed")).contains(QLatin1String("2024")),
           QStringLiteral("documents: an OpenDocument file's saves, editing time and printing"), odtFacts.join(QStringLiteral(" | ")));

    // A PDF saved again keeps its earlier versions: where each one ends.
    const QByteArray first = "%PDF-1.7\n1 0 obj<<>>endobj\nxref\n0 1\ntrailer<<>>\nstartxref\n9\n%%EOF\n";
    const QByteArray second = first + "2 0 obj<<>>endobj\nxref\n0 1\ntrailer<<>>\nstartxref\n80\n%%EOF\r\n";
    const QByteArray third = second + "3 0 obj<<>>endobj\nxref\n0 1\ntrailer<<>>\nstartxref\n150\n%%EOF";
    const QVector<qint64> ends = repair::pdfVersions(third);
    QByteArray linear = "%PDF-1.5\n1 0 obj<</Linearized 1>>endobj\nxref\ntrailer<<>>\nstartxref\n0\n%%EOF\n";
    linear += "2 0 obj<<>>endobj\nxref\ntrailer<<>>\nstartxref\n9\n%%EOF\n";
    report(ends == QVector<qint64>{first.size(), second.size(), third.size()} && repair::pdfVersions(first).size() == 1
               && repair::pdfVersions(linear).size() == 1 && repair::pdfVersions("not a pdf %%EOF").isEmpty()
               && repair::pdfVersions("%PDF-1.4\n%%EOF in a comment, no table\n").isEmpty(),
           QStringLiteral("documents: a PDF's earlier versions, each whole, found where they end (a web PDF's first-page table isn't one)"));

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

    // Where a phone film was made. An Android phone's: ©xyz in udta, its moov box written last.
    QByteArray xyz;
    be32(xyz, 0);
    xyz.replace(0, 4, QByteArray("\0\x12\x15\xc7", 4)); // its length (18) and language
    xyz.append("+53.8008-001.5491/");
    const QByteArray android = box("ftyp", QByteArray("isom\0\0\x02\0isomavc1", 16)) + box("mdat", QByteArray(300000, '\x22'))
                             + box("moov", box("udta", box("\xa9xyz", xyz)));
    quint64 where = 0;
    const QByteArray moov = mediainfo::mp4Moov([&android](quint64 at, char *buf, qint64 len) {
        const qint64 n = std::max<qint64>(0, std::min<qint64>(len, android.size() - qint64(at)));
        memcpy(buf, android.constData() + at, size_t(n));
        return n;
    }, quint64(android.size()), &where);
    const mediainfo::Info a = mediainfo::read(mp4Type, moov, quint64(android.size()));
    const QStringList aFacts = mediainfo::facts(a);
    report(where > 300000 && a.hasLocation && std::abs(a.latitude - 53.8008) < 1e-6 && std::abs(a.longitude + 1.5491) < 1e-6
               && aFacts.contains(QStringLiteral("Where\tLeeds, England, United Kingdom")),
           QStringLiteral("films: where an Android phone's was made, from the moov box at its end"), aFacts.join(QLatin1Char('|')));
    // An iPhone's: QuickTime keys in moov's meta.
    const QVector<std::pair<QByteArray, QByteArray>> apple = {
        {"com.apple.quicktime.location.ISO6709", "+37.3349-122.0090+010.000/"}, {"com.apple.quicktime.make", "Apple"},
        {"com.apple.quicktime.model", "iPhone 15 Pro"}, {"com.apple.quicktime.software", "17.5.1"},
        {"com.apple.quicktime.creationdate", "2024-07-14T13:22:05-0700"}};
    QByteArray keys(4, '\0'), items;
    be32(keys, quint32(apple.size()));
    for (int k = 0; k < apple.size(); ++k) {
        be32(keys, quint32(8 + apple[k].first.size()));
        keys.append("mdta").append(apple[k].first);
        QByteArray data;
        be32(data, 1);
        be32(data, 0);
        QByteArray item = box("data", data + apple[k].second);
        QByteArray numbered;
        be32(numbered, quint32(8 + item.size()));
        be32(numbered, quint32(k + 1));
        items.append(numbered + item);
    }
    QByteArray hdlr(8, '\0');
    hdlr.append("mdta").append(QByteArray(12, '\0'));
    const QByteArray iphone = box("ftyp", QByteArray("qt  \0\0\0\0qt  ", 12)) + box("moov", box("meta", fullBox("hdlr", 0, hdlr).mid(0) + box("keys", keys) + box("ilst", items)));
    const mediainfo::Info i = mediainfo::read(filetypes::typeNumber(QStringLiteral("mov")), iphone, quint64(iphone.size()));
    const QStringList iFacts = mediainfo::facts(i);
    report(i.hasLocation && std::abs(i.latitude - 37.3349) < 1e-6 && i.hasAltitude && i.altitude == 10 && i.make == QLatin1String("Apple")
               && i.model == QLatin1String("iPhone 15 Pro") && i.software == QLatin1String("17.5.1")
               && i.made.toUTC() == QDateTime(QDate(2024, 7, 14), QTime(20, 22, 5), QTimeZone::UTC)
               && iFacts.filter(QStringLiteral("Where\t")).join(QString()).contains(QLatin1String("United States")),
           QStringLiteral("films: an iPhone's place, model and time, from its QuickTime keys"), iFacts.join(QLatin1Char('|')));
    double lat = 0, lon = 0, alt = 0;
    bool hasAlt = false;
    report(mediainfo::parseIso6709(QStringLiteral("+27.5916+086.5640+8850CRSWGS_84/"), &lat, &lon, &alt, &hasAlt) && hasAlt && alt == 8850
               && !mediainfo::parseIso6709(QStringLiteral("+00.0000+000.0000/"), &lat, &lon, &alt, &hasAlt)
               && !mediainfo::parseIso6709(QStringLiteral("+95.0000+010.0000/"), &lat, &lon, &alt, &hasAlt)
               && !mediainfo::parseIso6709(QStringLiteral("nowhere"), &lat, &lon, &alt, &hasAlt),
           QStringLiteral("films: ISO 6709 places read, nonsense and 0,0 refused"));
    for (int round = 0; round < 2000; ++round) {
        QByteArray broken = (round % 2 ? iphone : android.right(200));
        for (int k = 0; k < 6; ++k)
            broken[rnd.bounded(int(broken.size()))] = char(rnd.bounded(256));
        mediainfo::facts(mediainfo::read(filetypes::typeNumber(QStringLiteral("mov")), broken, quint64(broken.size())));
    }
    report(true, QStringLiteral("films: 2000 damaged phone films read without a crash"));
}

// One record of NTFS's change journal, the way Windows writes it (USN_RECORD_V2 or V3).
QByteArray usnRecord(int version, quint64 record, quint16 sequence, quint64 parent, quint32 reason, const QString &name)
{
    {
        const int header = version == 3 ? 76 : 60;
        QByteArray utf16;
        for (QChar ch : name) {
            utf16 += char(ch.unicode() & 0xff);
            utf16 += char(ch.unicode() >> 8);
        }
        QByteArray r(header, '\0');
        r += utf16;
        r.append((8 - r.size() % 8) % 8, '\0');
        char *p = r.data();
        qToLittleEndian<quint32>(quint32(r.size()), p);
        qToLittleEndian<quint16>(quint16(version), p + 4);
        qToLittleEndian<quint64>(record | (quint64(sequence) << 48), p + 8);
        qToLittleEndian<quint64>(parent | (quint64(5) << 48), p + (version == 3 ? 24 : 16));
        qToLittleEndian<quint64>(133000000000000000ull, p + (version == 3 ? 48 : 32)); // some day in 2022
        qToLittleEndian<quint32>(reason, p + (version == 3 ? 56 : 40));
        qToLittleEndian<quint16>(quint16(utf16.size()), p + (version == 3 ? 72 : 56));
        qToLittleEndian<quint16>(quint16(header), p + (version == 3 ? 74 : 58));
        return r;
    }
}

// NTFS's change journal: records the way Windows writes them, V2 and V3, among zeros and junk.
void journal()
{
    const auto record = usnRecord;
    QByteArray bytes(4096, '\0'); // a stretch the journal hasn't used
    bytes += record(2, 64, 3, 5, 0x100, QStringLiteral("holiday.jpg"));
    bytes += QByteArray("junk that isn't a record at all", 32);
    bytes += record(2, 64, 3, 5, 0x200, QStringLiteral("holiday.jpg"));
    bytes += record(3, 70, 1, 5, 0x2000, QStringLiteral("Grüße.txt"));
    QByteArray broken = record(2, 71, 1, 5, 0x200, QStringLiteral("cut.txt"));
    qToLittleEndian<quint16>(500, broken.data() + 56); // a name longer than the record
    bytes += broken;
    const QVector<lost::JournalEntry> entries = lost::readJournal(bytes);
    QStringList seen;
    for (const lost::JournalEntry &e : entries)
        seen << QStringLiteral("%1/%2 %3 %4").arg(e.record).arg(e.sequence).arg(e.reason, 0, 16).arg(e.name);
    report(seen == QStringList{QStringLiteral("64/3 100 holiday.jpg"), QStringLiteral("64/3 200 holiday.jpg"), QStringLiteral("70/1 2000 Grüße.txt")}
               && entries.value(0).parent == 5 && entries.value(0).time > 0,
           QStringLiteral("NTFS's change journal: V2 and V3 records, between zeros and junk"), seen.join(QStringLiteral(", ")));
}

// Thumbnails asked for while scrolling pile up; the oldest are let go, and the window is told so it
// asks again when they're on screen once more (they used to stay a plain card for good).
void previewQueue()
{
    PreviewLoader loader(lost::Source::fromMemory(QByteArray(4096, '\0')));
    QVector<int> dropped;
    QObject::connect(&loader, &PreviewLoader::dropped, [&dropped](int index, int) { dropped << index; });
    lost::Found f;
    f.size = 4096;
    for (int i = 0; i < 300; ++i)
        loader.want(i, f, 128);
    loader.want(5, f, 128); // the same one again: moved up, not queued twice
    report(dropped.isEmpty(), QStringLiteral("previews: 300 thumbnails wait, the same one asked twice counts once"));
    loader.want(300, f, 128);
    report(dropped == QVector<int>{0}, QStringLiteral("previews: one more lets the oldest go, and says which"),
           QStringLiteral("%1 let go").arg(dropped.size()));
}

// EXIF written by hand, the way a camera would: a folder of tags (IFD) for the camera, one for
// the photo's settings and one for where it was taken, in either byte order.
struct ExifTag {
    quint16 tag, type;
    quint32 count;
    QByteArray bytes; // its value, already in the file's byte order
};

QByteArray exifTiff(bool little, bool loop = false)
{
    const auto number = [little](quint32 v, int bytes) {
        QByteArray b(bytes, '\0');
        for (int i = 0; i < bytes; ++i)
            b[little ? i : bytes - 1 - i] = char(v >> (8 * i));
        return b;
    };
    const auto n16 = [&number](quint16 v) { return number(v, 2); };
    const auto n32 = [&number](quint32 v) { return number(v, 4); };
    const auto text = [](const char *s) { return ExifTag{0, 2, quint32(qstrlen(s) + 1), QByteArray(s, int(qstrlen(s) + 1))}; };
    const auto rational = [&n32](QVector<std::pair<quint32, quint32>> v) {
        QByteArray b;
        for (const auto &[top, bottom] : v)
            b += n32(top) + n32(bottom);
        return ExifTag{0, 5, quint32(v.size()), b};
    };
    const auto tagged = [](quint16 tag, ExifTag t) { t.tag = tag; return t; };
    QByteArray t = QByteArray(little ? "II" : "MM") + n16(42) + n32(0); // where the first folder is: filled in last
    // A folder of tags at the end of what's written, its longer values after it.
    const auto folder = [&](const QVector<ExifTag> &tags) {
        const quint32 at = quint32(t.size());
        quint32 data = at + 2 + 12 * quint32(tags.size()) + 4;
        QByteArray entries = n16(quint16(tags.size())), values;
        for (const ExifTag &e : tags) {
            QByteArray value = e.bytes;
            entries += n16(e.tag) + n16(e.type) + n32(e.count);
            if (value.size() <= 4) {
                entries += value + QByteArray(4 - value.size(), '\0');
            } else {
                entries += n32(data + quint32(values.size()));
                values += value + QByteArray(value.size() % 2, '\0');
            }
        }
        t += entries + n32(0) + values;
        return at;
    };
    const quint32 exif = folder({tagged(0x829A, rational({{1, 250}})), tagged(0x829D, rational({{28, 10}})), ExifTag{0x8827, 3, 1, n16(100)},
                                 tagged(0x9003, text("2024:07:14 13:22:05")), tagged(0x9011, text("+02:00")), tagged(0x920A, rational({{42, 10}})),
                                 ExifTag{0xA002, 4, 1, n32(4032)}, ExifTag{0xA003, 4, 1, n32(3024)}, tagged(0xA431, text("093021001234")),
                                 tagged(0xA434, text("EF-S 18-55mm"))});
    const quint32 gps = folder({tagged(1, text("N")), tagged(2, rational({{51, 1}, {30, 1}, {2604, 100}})), tagged(3, text("W")),
                                tagged(4, rational({{0, 1}, {7, 1}, {3900, 100}})), ExifTag{5, 1, 1, QByteArray(1, '\0')}, tagged(6, rational({{35, 1}})),
                                tagged(7, rational({{11, 1}, {22, 1}, {4, 1}})), tagged(0x10, text("T")), tagged(0x11, rational({{4700, 100}})),
                                tagged(0x1D, text("2024:07:14"))});
    const quint32 first = quint32(t.size());
    const quint32 first0 = folder({tagged(0x010F, text("Canon")), tagged(0x0110, text("Canon EOS 80D")), tagged(0x0132, text("2024:07:14 13:20:00")),
                                   ExifTag{0x8769, 4, 1, n32(loop ? first : exif)}, ExifTag{0x8825, 4, 1, n32(loop ? exif : gps)}});
    t.replace(4, 4, n32(first0));
    return t;
}

QByteArray jpegWith(const QByteArray &tiff)
{
    const QByteArray app1 = QByteArray("Exif\0\0", 6) + tiff;
    const int length = int(app1.size()) + 2;
    return QByteArray("\xFF\xD8\xFF\xE1", 4) + char(length >> 8) + char(length & 0xFF) + app1 + QByteArray("\xFF\xDA\x00\x02\xFF\xD9", 6);
}

void photoFacts()
{
    for (const bool little : {true, false}) {
        const exif::Info i = exif::read(jpegWith(exifTiff(little)));
        const QString order = little ? QStringLiteral("Intel order") : QStringLiteral("Motorola order");
        report(i.make == QLatin1String("Canon") && i.model == QLatin1String("Canon EOS 80D") && i.lens == QLatin1String("EF-S 18-55mm"),
               QStringLiteral("EXIF (%1): the camera and lens").arg(order), i.make + QLatin1Char('/') + i.model + QLatin1Char('/') + i.lens);
        report(i.taken.isValid() && i.takenHasZone && i.taken.toUTC() == QDateTime(QDate(2024, 7, 14), QTime(11, 22, 5), QTimeZone::UTC),
               QStringLiteral("EXIF (%1): when it was taken, with its time zone").arg(order), i.taken.toString(Qt::ISODate));
        report(qFuzzyCompare(i.exposure, 0.004) && qFuzzyCompare(i.aperture, 2.8) && i.iso == 100 && qFuzzyCompare(i.focalLength, 4.2)
                   && i.width == 4032 && i.height == 3024,
               QStringLiteral("EXIF (%1): the settings and size").arg(order));
        report(i.hasLocation && std::abs(i.latitude - 51.507233) < 1e-5 && std::abs(i.longitude + 0.1275) < 1e-6 && i.hasAltitude && i.altitude == 35,
               QStringLiteral("EXIF (%1): where it was taken").arg(order), QStringLiteral("%1, %2").arg(i.latitude, 0, 'f', 6).arg(i.longitude, 0, 'f', 6));
    }
    const QStringList said = exif::describe(exif::read(jpegWith(exifTiff(true))));
    const QString all = said.join(QLatin1Char('|'));
    report(said.contains(QStringLiteral("Camera\tCanon EOS 80D")) && all.contains(QStringLiteral("1/250 s · f/2.8 · ISO 100 · 4.2 mm"))
               && all.contains(QStringLiteral("51.50723° N, 0.12750° W, 35 m up")) && all.contains(QLatin1String("UTC+02:00")),
           QStringLiteral("EXIF: said in plain words"), said.filter(QStringLiteral("Location")).join(QString()) + QLatin1Char('|') + said.value(3));
    // A raw file is TIFF itself; PNG, WebP, HEIC and CR3 carry the same block somewhere inside.
    report(exif::read(exifTiff(false)).model == QLatin1String("Canon EOS 80D"), QStringLiteral("EXIF: from a raw (TIFF) file"));
    report(exif::read(QByteArray("\x89PNG\r\n\x1a\n....eXIf", 16) + exifTiff(true)).iso == 100, QStringLiteral("EXIF: from a PNG's eXIf chunk"));
    report(exif::read(QByteArray(40, 'x') + QByteArray("Exif\0\0", 6) + exifTiff(false)).iso == 100, QStringLiteral("EXIF: found in a HEIC or WebP"));
    report(exif::read(QByteArray(20, 'x') + QByteArray("CMT1", 4) + exifTiff(true)).make == QLatin1String("Canon"), QStringLiteral("EXIF: from a CR3's CMT1 box"));
    // Folders pointing at each other, cut short anywhere, damaged anyhow: no hang, no crash.
    const exif::Info looped = exif::read(jpegWith(exifTiff(true, true)));
    report(looped.make == QLatin1String("Canon") && looped.iso == 0, QStringLiteral("EXIF: folders pointing back at each other are read once"));
    const QByteArray good = jpegWith(exifTiff(true));
    for (int cut = 0; cut < good.size(); ++cut)
        exif::read(good.left(cut));
    QRandomGenerator random(12345);
    for (int round = 0; round < 3000; ++round) {
        QByteArray bad = good;
        for (int k = 0, n = 1 + int(random.bounded(8)); k < n; ++k)
            bad[int(random.bounded(quint32(bad.size())))] = char(random.bounded(256));
        exif::describe(exif::read(bad));
    }
    report(true, QStringLiteral("EXIF: cut short at every byte and damaged 3000 ways, read without a crash"));
    report(exif::read(QByteArray("\xFF\xD8\xFF\xE0\x00\x10JFIF", 10)).empty(), QStringLiteral("EXIF: a photo without any says nothing"));

    // Where, which way, the satellites' time and the serial number, and nothing said about editing
    // for a photo straight from the camera.
    const exif::Info camera = exif::read(jpegWith(exifTiff(false)));
    report(camera.serial == QLatin1String("093021001234") && camera.hasDirection && qFuzzyCompare(camera.direction, 47.0) && !camera.directionMagnetic
               && camera.satelliteTime == QDateTime(QDate(2024, 7, 14), QTime(11, 22, 4), QTimeZone::UTC),
           QStringLiteral("EXIF: the camera's serial number, which way it faced, the satellites' time"));
    report(said.contains(QStringLiteral("Where\tLondon, England, United Kingdom")) && said.contains(QStringLiteral("Facing\tnorth-east (47°)"))
               && said.contains(QStringLiteral("Camera serial\t093021001234")) && exif::editSigns(camera).isEmpty() && !all.contains(QLatin1String("Changed since")),
           QStringLiteral("EXIF: where it was taken, by name, without going online; nothing said about editing"), all);
    // The camera's clock without a time zone: the satellites' time says which it was on.
    QByteArray noZone = exifTiff(true);
    noZone.replace("+02:00", "      ");
    const QString clock = exif::describe(exif::read(jpegWith(noZone))).filter(QStringLiteral("Satellite time")).join(QString());
    report(clock.contains(QLatin1String("on UTC+02:00")), QStringLiteral("EXIF: the satellites' time gives the time zone the camera's clock was on"), clock);

    // Edited: Photoshop's XMP, with its history, and the "changed" time after the "taken" one.
    QByteArray edited = exifTiff(true);
    edited.replace("2024:07:14 13:20:00", "2024:07:16 09:30:00");
    const QByteArray xmp = R"x(<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF><rdf:Description xmp:CreatorTool="Adobe Photoshop 25.0 (Windows)">)x"
                           R"x(<xmpMM:History><rdf:Seq><rdf:li stEvt:action="created" stEvt:when="2024-07-14T13:22:05+02:00"/>)x"
                           R"x(<rdf:li stEvt:action="saved" stEvt:when="2024-07-16T09:30:00+02:00" stEvt:softwareAgent="Adobe Photoshop 25.0 (Windows)"/>)x"
                           R"x(<rdf:li><stEvt:action>converted</stEvt:action><stEvt:softwareAgent>Adobe Photoshop 25.0 (Windows)</stEvt:softwareAgent></rdf:li>)x"
                           R"x(</rdf:Seq></xmpMM:History></rdf:Description></rdf:RDF></x:xmpmeta>)x";
    const QByteArray xmpSegment = QByteArray("http://ns.adobe.com/xap/1.0/\0", 29) + xmp;
    QByteArray withXmp = jpegWith(edited);
    const int xmpLength = int(xmpSegment.size()) + 2;
    withXmp.insert(withXmp.indexOf("\xFF\xDA"), QByteArray("\xFF\xE1", 2) + char(xmpLength >> 8) + char(xmpLength & 0xFF) + xmpSegment);
    const exif::Info touched = exif::read(withXmp);
    const QStringList signs = exif::editSigns(touched);
    report(touched.xmpTool.startsWith(QLatin1String("Adobe Photoshop")) && touched.history.size() == 3 && signs.size() == 3
               && signs.last().contains(QLatin1String("lists 2 change")),
           QStringLiteral("EXIF: an edited photo says so (changed after, an editing program, its history)"), signs.join(QStringLiteral(" | ")));
}

// Copies made just to look at a found file: the same bytes, under its own name, read-only, made
// once, and gone when they're done with. Programs and scripts are never opened.
void lookCopies()
{
    QByteArray drive(1 << 20, '\0');
    QByteArray photo = QByteArray("\xFF\xD8\xFF\xE0", 4) + QByteArray(300000, 'p');
    drive.replace(65536, photo.size(), photo);
    const auto source = lost::Source::fromMemory(drive);
    lost::Found f;
    f.name = QStringLiteral("Beach day.jpg");
    f.size = quint64(photo.size());
    f.extents = {{65536, f.size}};
    QString path, again, why;
    {
        lost::LookCopies copies{QString()}; // a disk image: no drive to keep off
        path = copies.make(source, f, 7, &why);
        again = copies.make(source, f, 7, &why);
        QFile copy(path);
        report(!path.isEmpty() && copy.open(QIODevice::ReadOnly) && copy.readAll() == photo && path.endsWith(QLatin1String("/7/Beach day.jpg")),
               QStringLiteral("look copies: the file's own bytes, under its own name"), path + why);
        report(again == path && !(QFileInfo(path).permissions() & QFileDevice::WriteOwner),
               QStringLiteral("look copies: made once, and read-only"));
        // The viewer's changed copy: the copy with the changed bytes put in, the copy itself untouched.
        QTemporaryDir out;
        const QString changed = out.filePath(QStringLiteral("Beach day (changed).jpg"));
        const QMap<quint64, quint8> changes = {{0, 0x00}, {200000, 'Q'}, {quint64(photo.size()) - 1, 'Z'}, {quint64(photo.size()) + 50, 'X'}};
        QFile written(changed), original(path);
        QByteArray expected = photo;
        expected[0] = '\0';
        expected[200000] = 'Q';
        expected[expected.size() - 1] = 'Z';
        report(lost::LookCopies::saveChanged(path, changes, changed, &why) && written.open(QIODevice::ReadOnly) && written.readAll() == expected
                   && original.open(QIODevice::ReadOnly) && original.readAll() == photo,
               QStringLiteral("look copies: a changed copy is saved with the changes in it, past its end left out"), why);
        lost::Found gone = f;
        gone.nameOnly = true;
        report(copies.make(source, gone, 8, &why).isEmpty() && why.contains(QLatin1String("Only its name")),
               QStringLiteral("look copies: nothing to copy when only a name is left"), why);
    }
    report(!path.isEmpty() && !QFileInfo::exists(path), QStringLiteral("look copies: gone when Find Lost Files closes"));
    const QStringList safe = {QStringLiteral("a.JPG"), QStringLiteral("letter.pdf"), QStringLiteral("song.flac"), QStringLiteral("film.mkv"),
                              QStringLiteral("notes.docx")};
    const QStringList unsafe = {QStringLiteral("invoice.pdf.exe"), QStringLiteral("setup.msi"), QStringLiteral("run.sh"), QStringLiteral("page.html"),
                                QStringLiteral("macro.docm"), QStringLiteral("README"), QStringLiteral("app.desktop"), QStringLiteral("x.svg"),
                                QStringLiteral("t.ps1"), QStringLiteral("shortcut.lnk")};
    bool right = true;
    for (const QString &name : safe)
        right = right && lost::LookCopies::safeToOpen(name);
    for (const QString &name : unsafe)
        right = right && !lost::LookCopies::safeToOpen(name);
    report(right, QStringLiteral("look copies: documents, pictures and films open in their own program; programs, scripts and unknowns don't"));
}

// The viewer's messages: what's sent comes out the same, whole or in pieces, and anything else
// is refused. And each kind of file is shown its own way.
void viewerMessages()
{
    viewer::Message m;
    m.type = viewer::Message::ShowFile;
    m.show.kind = viewer::Kind::Pdf;
    m.show.type = filetypes::typeForName(QStringLiteral("letter.pdf"));
    m.show.path = QStringLiteral("/tmp/x/3/letter.pdf");
    m.show.name = QStringLiteral("letter.pdf");
    m.show.where = QStringLiteral("3 of 12");
    m.show.facts = {QStringLiteral("Created\tyesterday"), QStringLiteral("Author\tDanny")};
    m.show.next = true;
    m.show.openable = true;
    viewer::Message ask;
    ask.type = viewer::Message::AskFor;
    ask.ask = viewer::Ask::Next;
    QByteArray stream = viewer::encode(m) + viewer::encode(ask);
    QByteArray piece;
    QVector<viewer::Message> got;
    bool bad = false;
    for (const char c : std::as_const(stream)) { // a byte at a time, the slowest a pipe can be
        piece += c;
        viewer::Message out;
        while (viewer::decode(piece, out, &bad))
            got << out;
    }
    report(got.size() == 2 && !bad && got[0].show.path == m.show.path && got[0].show.facts == m.show.facts && got[0].show.where == m.show.where
               && got[0].show.next && !got[0].show.previous && got[0].show.openable && got[0].show.kind == viewer::Kind::Pdf
               && got[1].ask == viewer::Ask::Next,
           QStringLiteral("viewer: its messages come out as they went in, even a byte at a time"));
    viewer::Message edited;
    edited.type = viewer::Message::Edited;
    edited.edits = {{4, 0x41}, {123456789012ull, 0x00}, {7, 0xFF}};
    QByteArray editedBytes = viewer::encode(edited);
    viewer::Message editedBack;
    bool editedBad = false;
    report(viewer::decode(editedBytes, editedBack, &editedBad) && !editedBad && editedBack.type == viewer::Message::Edited && editedBack.edits == edited.edits,
           QStringLiteral("viewer: changed bytes go back as they were changed"));
    QByteArray lying = viewer::encode(edited);
    lying.replace(12, 4, QByteArray("\xFF\xFF\xFF\xFF", 4)); // a made-up count
    report(!viewer::decode(lying, editedBack, &editedBad) || editedBack.edits.size() <= 3, QStringLiteral("viewer: a made-up count of changes asks for nothing"));
    // A picture made in the viewer: its bytes come back whole; one claiming to be bigger than the
    // most a picture may be is refused.
    viewer::Message picture;
    picture.type = viewer::Message::SavePicture;
    picture.pictureName = QStringLiteral("beach (enhanced).png");
    picture.picture = QByteArray("\x89PNG\r\n\x1a\n", 8) + QByteArray(100000, 'p');
    QByteArray pictureBytes = viewer::encode(picture);
    viewer::Message pictureBack;
    bool pictureBad = false;
    report(viewer::decode(pictureBytes, pictureBack, &pictureBad) && !pictureBad && pictureBack.pictureName == picture.pictureName
               && pictureBack.picture == picture.picture,
           QStringLiteral("viewer: a picture made in it comes back to be saved, whole"));
    QByteArray huge = viewer::encode(picture);
    huge.replace(8, 4, QByteArray("\x7F\xFF\xFF\xFF", 4)); // its length: 2 GB
    report(!viewer::decode(huge, pictureBack, &pictureBad) && pictureBad, QStringLiteral("viewer: a picture message claiming 2 GB is refused"));
    const char junkBytes[] = "\x00\x00\x00\x01 not a message at all";
    QByteArray junk(junkBytes, sizeof(junkBytes) - 1);
    viewer::Message out;
    report(!viewer::decode(junk, out, &bad) && bad, QStringLiteral("viewer: anything else is refused"));
    QRandomGenerator random(7);
    for (int round = 0; round < 2000; ++round) {
        QByteArray damaged = viewer::encode(m);
        damaged[int(random.bounded(quint32(damaged.size())))] = char(random.bounded(256));
        while (viewer::decode(damaged, out, &bad)) {}
    }
    report(true, QStringLiteral("viewer: 2000 damaged messages read without a crash"));
    // The preview helper's answers are read the same careful way: it handles hostile files too.
    preview::Result said;
    said.text = QStringLiteral("3 pages");
    said.facts = {QStringLiteral("Title\tBank letter"), QStringLiteral("Author\tDanny")};
    QByteArray answer;
    {
        QBuffer buffer(&answer);
        buffer.open(QIODevice::WriteOnly);
        preview::write(buffer, said);
    }
    for (int round = 0; round < 2000; ++round) {
        QByteArray damaged = answer;
        damaged[8 + int(random.bounded(quint32(damaged.size() - 8)))] = char(random.bounded(256));
        QBuffer buffer(&damaged);
        buffer.open(QIODevice::ReadOnly);
        preview::Result back;
        preview::read(buffer, back, 100);
    }
    report(true, QStringLiteral("previews: 2000 damaged answers from the helper read without a crash"));
    const auto kind = [](const char *name) { return viewer::kindFor(filetypes::typeForName(QString::fromLatin1(name)), QString::fromLatin1(name)); };
    report(kind("a.jpg") == viewer::Kind::Picture && kind("b.pdf") == viewer::Kind::Pdf && kind("c.mp4") == viewer::Kind::Film
               && kind("d.mp3") == viewer::Kind::Song && kind("e.docx") == viewer::Kind::Words && kind("f.zip") == viewer::Kind::Words
               && kind("notes.txt") == viewer::Kind::Words && kind("thing.bin") == viewer::Kind::Bytes,
           QStringLiteral("viewer: pictures, PDFs, films, songs, documents, archives, text and the rest each get their own view"));
}

// The kinds added in 0.5.2, against files made by the programs that make them for real
// (tests/data/lostfs/types/make.sh): each is found whole, to the byte, and shown: its words,
// its picture, its details.
QByteArray typesFile(const QString &name)
{
    const QString path = QStringLiteral(SOURCE_DIR "/tests/data/lostfs/types/") + name;
    if (name.endsWith(QLatin1String(".zst")))
        return unpackImage(path);
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

filetypes::Measured measured(const QByteArray &d)
{
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
        if (m.size > 0)
            return m;
    }
    return {};
}

preview::Result previewOf(const QByteArray &d, int type, int pixels = 256)
{
    preview::Request r;
    r.type = type;
    r.size = quint64(d.size());
    r.pixels = pixels;
    r.data = d;
    return preview::make(r);
}

void newKinds()
{
    struct Case {
        const char *file, *type;
    };
    // With the files' own names: a long "Lake trip.mkv" in the store, the real thing on a disk.
    const Case cases[] = {{"notes.doc", "doc"},       {"budget.xls.zst", "xls"}, {"slides.ppt.zst", "ppt"}, {"ferry.msg", "msg"},
                          {"macros.doc", "doc"},      {"picture.tif", "tif"},    {"trip.mkv", "mkv"},       {"trip.webm", "webm"},
                          {"song.flac", "flac"},      {"trip.wmv", "wmv"},       {"song.wma", "wma"},       {"notes.txt.gz", "gz"},
                          {"pictures.tar.gz", "gz"}};
    QHash<QString, QByteArray> files;
    for (const Case &c : cases) {
        const QByteArray d = typesFile(QString::fromLatin1(c.file));
        files.insert(QString::fromLatin1(c.file), d);
        const filetypes::Measured m = measured(d);
        const QString got = m.type >= 0 && m.type < filetypes::types().size() ? filetypes::types()[m.type].id : QStringLiteral("nothing");
        report(!d.isEmpty() && got == QLatin1String(c.type) && m.complete && m.size == quint64(d.size()),
               QStringLiteral("%1: found as %2, whole, to the byte").arg(QString::fromLatin1(c.file), got),
               QStringLiteral("%1 bytes of %2, %3").arg(m.size).arg(d.size()).arg(m.complete ? QStringLiteral("whole") : QStringLiteral("not whole")));
    }
    const auto type = [](const char *id) { return filetypes::typeForName(QStringLiteral("x.") + QString::fromLatin1(id)); };
    const auto fact = [](const QStringList &facts, const QString &label) {
        for (const QString &f : facts) {
            if (f.section(QLatin1Char('\t'), 0, 0) == label)
                return f.section(QLatin1Char('\t'), 1);
        }
        return QString();
    };

    // Word: the text from its pieces, past the plain-ASCII ones, and what it says about itself.
    const ole::File doc(files.value(QStringLiteral("notes.doc")));
    const QString words = ole::text(doc, 100000);
    report(words.startsWith(QLatin1String("Pack the blue tent and the camera.\nThe ferry leaves at 07:45 from pier 3, so don't be late."))
               && words.contains(QStringLiteral("Unicode check: café, naïve, Straße, 東京.")) && words.endsWith(QLatin1String("Line 120 of the packing list.")),
           QStringLiteral("doc: its words, every piece of them"), words.left(160));
    const preview::Result docPreview = previewOf(files.value(QStringLiteral("notes.doc")), type("doc"));
    report(fact(docPreview.facts, QStringLiteral("Title")) == QLatin1String("Holiday plans") && fact(docPreview.facts, QStringLiteral("Author")) == QLatin1String("Sam Tester")
               && fact(docPreview.facts, QStringLiteral("Created")).contains(QLatin1String("2024")) && docPreview.text.startsWith(QLatin1String("Pack the blue tent")),
           QStringLiteral("doc: its title, author and when it was made"), docPreview.facts.join(QStringLiteral(" | ")));
    report(!ole::hasMacros(doc) && ole::hasMacros(ole::File(files.value(QStringLiteral("macros.doc")))),
           QStringLiteral("doc: one with macros is told from one without"));

    // Excel: both sheets, every cell, the words from the shared table even where it runs on into
    // the records after it, and a formula's answer.
    const ole::File xls(files.value(QStringLiteral("budget.xls.zst")));
    const QString cells = ole::text(xls, 1000000);
    QString missing;
    for (int i = 1; i <= 800 && missing.isEmpty(); ++i) {
        const QString line = QStringLiteral("Place number %1 on the long road north\t%2\n").arg(i, 4, 10, QLatin1Char('0')).arg(i * 7);
        if (!(cells + QLatin1Char('\n')).contains(line))
            missing = line;
    }
    const bool every = missing.isEmpty();
    report(cells.startsWith(QStringLiteral("[Groceries]\nItem\tCost\nBread\t2.5\nMilk\t1.25\nCafé crème\t3.75\nTotal\t")) && cells.contains(QLatin1String("\n\n[Road]\n")) && every,
           QStringLiteral("xls: both sheets, all 805 rows, words split across records put back together"),
           every ? cells.left(200) : QStringLiteral("missing: ") + missing + QStringLiteral(" near: ") + cells.mid(cells.indexOf(missing.left(17)) - 60, 200));
    report(fact(ole::properties(xls), QStringLiteral("Title")) == QLatin1String("Budget"), QStringLiteral("xls: what it says about itself"));

    // PowerPoint: the slides' words, in order, without the master slides' "Click to edit".
    const ole::File ppt(files.value(QStringLiteral("slides.ppt.zst")));
    const QString slides = ole::text(ppt, 100000);
    report(slides.contains(QLatin1String("Summer trip")) && slides.indexOf(QLatin1String("Day one: the lake")) < slides.indexOf(QLatin1String("Day two: the mountains"))
               && slides.indexOf(QLatin1String("Day one")) >= 0 && !slides.contains(QLatin1String("Click to edit")),
           QStringLiteral("ppt: the slides' words, in order"), slides.left(200));
    report(fact(ole::properties(ppt), QStringLiteral("Title")) == QLatin1String("Trip slides") && !ole::hasMacros(ppt),
           QStringLiteral("ppt: its title, and no macros"));

    // Outlook: who, to whom, when, what about, what came with it, and the message.
    const ole::File msg(files.value(QStringLiteral("ferry.msg")));
    const QString email = ole::text(msg, 100000);
    const QStringList mailFacts = ole::properties(msg);
    report(email.startsWith(QLatin1String("From: Sam Tester <sam@example.com>\nTo: Alex Example\n")) && email.contains(QLatin1String("\nSubject: Ferry tickets\n"))
               && email.contains(QLatin1String("\nAttached: tickets.pdf\n")) && email.contains(QLatin1String("See you at pier 3.")) && email.endsWith(QLatin1String("Sam"))
               && fact(mailFacts, QStringLiteral("Sent")).contains(QLatin1String("2024")) && fact(mailFacts, QStringLiteral("Subject")) == QLatin1String("Ferry tickets"),
           QStringLiteral("msg: the email with its sender, recipient, date, subject and attachment"), email.left(200));
    // Locked: a password-protected Office 2007+ file is a compound file of two streams.
    report(ole::locked(ole::File(compoundFile(QStringLiteral("EncryptedPackage"), QByteArray(5000, 'e'), QStringLiteral("EncryptionInfo"), QByteArray(200, 'i'))))
               && !ole::locked(doc),
           QStringLiteral("office: a locked file is told apart"));

    // Pictures: a TIFF through Qt's reader; a raw photo through the JPEG the camera put in it; a
    // Photoshop file through our own reading of its merged picture, packed or not.
    const QImage tif = pictures::decode(files.value(QStringLiteral("picture.tif")), type("tif"), 0);
    report(tif.size() == QSize(96, 64) && qRed(tif.pixel(30, 30)) > 200 && qGreen(tif.pixel(72, 32)) > 200,
           QStringLiteral("tif: the picture"), QStringLiteral("%1x%2").arg(tif.width()).arg(tif.height()));
    const QImage nef = pictures::decode(tiff("NIKON CORPORATION"), type("nef"), 0);
    report(nef.size() == QSize(320, 200) && previewOf(tiff("NIKON CORPORATION"), type("nef"), 64).image.width() == 64,
           QStringLiteral("raw: the camera's own JPEG inside it, full size and as a thumbnail"));
    const QImage raw = pictures::decode(raf(), type("raf"), 0);
    report(raw.size() == QSize(320, 200), QStringLiteral("raf: the camera's own JPEG inside it"));
    const QImage flat = pictures::decode(psd(false), type("psd"), 0), packed = pictures::decode(psd(true), type("psd"), 0);
    report(flat.size() == QSize(4, 4) && flat.pixel(1, 1) == qRgb(0x77, 0x77, 0x77) && packed.size() == QSize(4, 4)
               && packed.pixel(0, 0) == qRgb(0x10, 0x10, 0x10) && packed.pixel(3, 2) == qRgb(0x40, 0x40, 0x40),
           QStringLiteral("psd: the merged picture, as it is and packed row by row"));
    QByteArray cut = psd(true);
    cut.chop(10);
    report(pictures::decode(cut, type("psd"), 0).height() == 2, QStringLiteral("psd: cut short, the rows that are there")); // blue's last 2 rows gone

    // Films and songs: how long, how big, what with, the tags and the cover.
    const auto media = [&files, &type](const char *file, const char *id) {
        return mediainfo::read(type(id), files.value(QString::fromLatin1(file)), quint64(files.value(QString::fromLatin1(file)).size()));
    };
    const mediainfo::Info mkv = media("trip.mkv", "mkv");
    report(mkv.width == 64 && mkv.height == 48 && mkv.codec == QLatin1String("MJPEG") && qAbs(mkv.durationMs - 2000) < 50 && mkv.title == QLatin1String("Lake trip")
               && mkv.artist == QLatin1String("Sam Tester") && !mkv.cover.isEmpty() && mkv.recorded.date() == QDate(2024, 7, 14),
           QStringLiteral("mkv: size, codec, length, title, artist, cover and date"), mediainfo::describe(mkv));
    const mediainfo::Info webm = media("trip.webm", "webm");
    report(webm.width == 64 && webm.codec == QLatin1String("VP9") && qAbs(webm.durationMs - 1000) < 50, QStringLiteral("webm: size, codec and length"),
           mediainfo::describe(webm));
    const mediainfo::Info flac = media("song.flac", "flac");
    report(flac.codec == QLatin1String("FLAC") && flac.sampleRate == 44100 && qAbs(flac.durationMs - 1500) < 30 && flac.title == QLatin1String("Lake trip")
               && flac.album == QLatin1String("Summer") && flac.year == QLatin1String("2024") && !QImage::fromData(flac.cover).isNull(),
           QStringLiteral("flac: length, rate, tags and cover"), mediainfo::describe(flac));
    const mediainfo::Info wmv = media("trip.wmv", "wmv");
    report(wmv.width == 64 && wmv.height == 48 && wmv.codec == QLatin1String("WMV2") && qAbs(wmv.durationMs - 2000) < 150 && wmv.title == QLatin1String("Lake trip")
               && wmv.artist == QLatin1String("Sam Tester"),
           QStringLiteral("wmv: size, codec, length, title and artist"), mediainfo::describe(wmv));
    const mediainfo::Info wma = media("song.wma", "wma");
    report(wma.codec == QLatin1String("WMA") && wma.sampleRate == 44100 && wma.album == QLatin1String("Summer") && wma.year == QLatin1String("2024"),
           QStringLiteral("wma: codec, rate, album and year"), mediainfo::describe(wma));

    // Archives: a .gz's one file, by the name it kept; what a .tar.gz holds; a RAR's files.
    const QVector<officetext::Entry> gz = officetext::list(files.value(QStringLiteral("notes.txt.gz")), 10);
    report(gz.size() == 1 && gz[0].path == QLatin1String("notes.txt") && gz[0].size == 20, QStringLiteral("gz: the file it packs, its name and size"));
    const QVector<officetext::Entry> tgz = officetext::list(files.value(QStringLiteral("pictures.tar.gz")), 10);
    report(tgz.size() == 2 && tgz[0].path == QLatin1String("cover.jpg") && tgz[1].path == QLatin1String("picture.tif"), QStringLiteral("tar.gz: what's in it"));
    const QVector<officetext::Entry> rar5List = officetext::list(rar5(), 10), rar4List = officetext::list(rar4(), 10);
    report(rar5List.size() == 1 && rar5List[0].path == QLatin1String("notes.txt") && rar5List[0].size == 2500 && rar4List.size() == 1
               && rar4List[0].path == QLatin1String("old.txt") && rar4List[0].size == 1800,
           QStringLiteral("rar: what's in a RAR 5 and a RAR 4 archive"),
           QStringLiteral("%1 %2 / %3 %4").arg(rar5List.value(0).path).arg(rar5List.value(0).size).arg(rar4List.value(0).path).arg(rar4List.value(0).size));

    // Text kinds: found whole when the rest of their last sector is zeros, as a drive leaves it.
    const Case textCases[] = {{"ferry.eml", "eml"},    {"Inbox.mbox", "mbox"}, {"notes.rtf", "rtf"}, {"notes.html", "html"},
                              {"drawing.svg", "svg"},  {"settings.xml", "xml"}, {"trip.ics", "ics"}, {"phone.vcf", "vcf"}};
    for (const Case &c : textCases) {
        const QByteArray d = typesFile(QString::fromLatin1(c.file));
        files.insert(QString::fromLatin1(c.file), d);
        const filetypes::Measured m = measured(d + QByteArray(512 - d.size() % 512, '\0'));
        const QString got = m.type >= 0 && m.type < filetypes::types().size() ? filetypes::types()[m.type].id : QStringLiteral("nothing");
        report(!d.isEmpty() && got == QLatin1String(c.type) && m.complete && m.size == quint64(d.size()),
               QStringLiteral("%1: found as %2, whole, to the byte").arg(QString::fromLatin1(c.file), got),
               QStringLiteral("%1 bytes of %2, %3").arg(m.size).arg(d.size()).arg(m.complete ? QStringLiteral("whole") : QStringLiteral("not whole")));
    }
    const QString eml = texts::text(type("eml"), files.value(QStringLiteral("ferry.eml")), 100000);
    const QStringList emlFacts = texts::properties(type("eml"), files.value(QStringLiteral("ferry.eml")));
    report(eml.startsWith(QStringLiteral("From: Sam Tëster <sam@example.com>\nTo: Alex Example <alex@example.com>\nSent: "))
               && eml.contains(QStringLiteral("\nSubject: Fähre: the tickets for Sunday\nAttached: Fähre tickets.pdf\n\nHi Alex,"))
               && eml.contains(QStringLiteral("Café at pier 3 at 07:30?")) && eml.endsWith(QLatin1String("Sam"))
               && fact(emlFacts, QStringLiteral("Sent")).contains(QLatin1String("2024")),
           QStringLiteral("eml: headers decoded, the readable part, the attachment's name"), eml.left(240));
    const QString box = texts::text(type("mbox"), files.value(QStringLiteral("Inbox.mbox")));
    report(fact(texts::properties(type("mbox"), files.value(QStringLiteral("Inbox.mbox"))), QStringLiteral("Messages")) == QLatin1String("3")
               && box.indexOf(QLatin1String("Lunch on Friday?")) < box.indexOf(QLatin1String("Photos from the lake"))
               && box.contains(QLatin1String("Re: Photos from the lake")),
           QStringLiteral("mbox: its messages, in order"), box);
    const QString rtfText = texts::text(type("rtf"), files.value(QStringLiteral("notes.rtf")), 100000);
    const QStringList rtfFacts = texts::properties(type("rtf"), files.value(QStringLiteral("notes.rtf")));
    report(rtfText.startsWith(QLatin1String("Pack the blue tent and the camera.\nThe ferry leaves at 07:45 from pier 3, so don't be late."))
               && rtfText.contains(QStringLiteral("Unicode check: café, naïve, Straße, 東京.")) && rtfText.endsWith(QLatin1String("Line 120 of the packing list."))
               && fact(rtfFacts, QStringLiteral("Title")) == QLatin1String("Holiday plans") && fact(rtfFacts, QStringLiteral("Author")) == QLatin1String("Sam Tester")
               && fact(rtfFacts, QStringLiteral("Created")).contains(QLatin1String("2024")),
           QStringLiteral("rtf: its words without the markup, its title, author and date"), rtfText.left(160) + QStringLiteral(" | ") + rtfFacts.join(QStringLiteral(" | ")));
    const QString page = texts::text(type("html"), files.value(QStringLiteral("notes.html")), 100000);
    report(page.contains(QLatin1String("Pack the blue tent and the camera.")) && page.contains(QStringLiteral("Straße, 東京"))
               && !page.contains(QLatin1Char('<')) && fact(texts::properties(type("html"), files.value(QStringLiteral("notes.html"))), QStringLiteral("Title")) == QLatin1String("Holiday plans"),
           QStringLiteral("html: its words and title"), page.left(160));
    const QString events = texts::text(type("ics"), files.value(QStringLiteral("trip.ics")));
    report(events.contains(QLatin1String("Ferry to the island (Pier 3)")) && events.contains(QLatin1String("Walk up the mountain, if it's dry"))
               && fact(texts::properties(type("ics"), files.value(QStringLiteral("trip.ics"))), QStringLiteral("Events")) == QLatin1String("2"),
           QStringLiteral("ics: the events"), events);
    const QString people = texts::text(type("vcf"), files.value(QStringLiteral("phone.vcf")));
    report(people == QStringLiteral("Alex Example · +1 555 0100 · alex@example.com\nJörg Müller · +49 555 0101")
               && fact(texts::properties(type("vcf"), files.value(QStringLiteral("phone.vcf"))), QStringLiteral("Contacts")) == QLatin1String("2"),
           QStringLiteral("vcf: names, numbers and addresses, old phones' encoded ones too"), people);
    if (QImageReader::supportedImageFormats().contains("svg")) {
        const QImage drawing = pictures::decode(files.value(QStringLiteral("drawing.svg")), type("svg"), 0);
        report(drawing.size() == QSize(96, 64) && qRed(drawing.pixel(30, 30)) > 200, QStringLiteral("svg: drawn"));
    } else {
        report(true, QStringLiteral("svg: skipped, Qt's SVG reader isn't installed here"));
    }

    // The viewer shows each in its own way.
    const auto kind = [](const char *name) { return viewer::kindFor(filetypes::typeForName(QString::fromLatin1(name)), QString::fromLatin1(name)); };
    report(kind("a.nef") == viewer::Kind::Picture && kind("b.psd") == viewer::Kind::Picture && kind("c.mkv") == viewer::Kind::Film
               && kind("d.flac") == viewer::Kind::Song && kind("e.doc") == viewer::Kind::Words && kind("f.msg") == viewer::Kind::Words
               && kind("g.rar") == viewer::Kind::Words,
           QStringLiteral("viewer: the new kinds each get their view"));

    // Damaged ones never crash any of it.
    QRandomGenerator rng(2026);
    int runs = 0;
    for (const Case &c : cases) {
        const QByteArray good = files.value(QString::fromLatin1(c.file));
        for (int round = 0; round < 60; ++round, ++runs) {
            QByteArray bad = good;
            for (int k = 0, n = 1 + int(rng.bounded(16)); k < n; ++k)
                bad[int(rng.bounded(quint32(bad.size())))] = char(rng.bounded(256));
            if (round % 3 == 0)
                bad.truncate(qsizetype(rng.bounded(quint32(bad.size()))));
            const ole::File f(bad);
            ole::text(f, 20000);
            ole::properties(f);
            previewOf(bad, type(c.type), 64);
            measured(bad);
        }
    }
    for (const Case &c : textCases) {
        const QByteArray good = files.value(QString::fromLatin1(c.file));
        for (int round = 0; round < 80; ++round, ++runs) {
            QByteArray bad = good;
            for (int k = 0, n = 1 + int(rng.bounded(16)); k < n; ++k)
                bad[int(rng.bounded(quint32(bad.size())))] = char(round % 2 ? rng.bounded(256) : "{}\\<>=?:\n\r-"[rng.bounded(12)]);
            if (round % 3 == 0)
                bad.truncate(qsizetype(rng.bounded(quint32(bad.size()))));
            texts::text(type(c.type), bad, 20000);
            texts::properties(type(c.type), bad);
            previewOf(bad, type(c.type), 64);
            measured(bad + QByteArray(512, '\0'));
        }
    }
    for (int round = 0; round < 300; ++round, ++runs) {
        QByteArray bad = round % 2 ? psd(true) : tiff("SONY");
        for (int k = 0, n = 1 + int(rng.bounded(8)); k < n; ++k)
            bad[int(rng.bounded(quint32(bad.size())))] = char(rng.bounded(256));
        pictures::decode(bad, type(round % 2 ? "psd" : "arw"), 64);
    }
    report(true, QStringLiteral("new kinds: %1 damaged files read without a crash").arg(runs));
}

// Files found by their contents are named by what's inside them, and keep the date they carry.
void namesFromContents()
{
    const auto named = [](const QByteArray &data, const char *id) {
        lost::Found f;
        f.type = filetypes::typeForName(QStringLiteral("x.") + QString::fromLatin1(id));
        f.size = quint64(data.size());
        f.extents = {{0, f.size}};
        f.name = lost::carvedName(f.type, 1);
        lost::nameFromContents(f, *lost::Source::fromMemory(data));
        return f;
    };
    const auto at = [](int y, int mo, int d, int h, int mi, int s, const QTimeZone &zone) {
        return QDateTime(QDate(y, mo, d), QTime(h, mi, s), zone).toMSecsSinceEpoch();
    };
    struct Case {
        const char *file, *id, *name;
    };
    const Case cases[] = {{"notes.doc", "doc", "Holiday plans.doc"},
                          {"ferry.msg", "msg", "Ferry tickets.msg"},
                          {"song.flac", "flac", "Sam Tester - Lake trip.flac"},
                          {"trip.mkv", "mkv", "Sam Tester - Lake trip.mkv"},
                          {"trip.wmv", "wmv", "Sam Tester - Lake trip.wmv"},
                          {"ferry.eml", "eml", "Fähre - the tickets for Sunday.eml"},
                          {"notes.rtf", "rtf", "Holiday plans.rtf"},
                          {"notes.html", "html", "Holiday plans.html"},
                          {"budget.xls.zst", "xls", "Budget.xls"},
                          {"slides.ppt.zst", "ppt", "Trip slides.ppt"}};
    for (const Case &c : cases) {
        const lost::Found f = named(typesFile(QString::fromLatin1(c.file)), c.id);
        report(f.name == QString::fromUtf8(c.name), QStringLiteral("names: %1 is called \"%2\"").arg(QString::fromLatin1(c.file), QString::fromUtf8(c.name)), f.name);
    }
    report(named(typesFile(QStringLiteral("notes.doc")), "doc").inside == at(2024, 7, 14, 13, 22, 5, QTimeZone::UTC)
               && named(typesFile(QStringLiteral("ferry.msg")), "msg").inside == at(2024, 7, 14, 13, 22, 5, QTimeZone::UTC)
               && named(typesFile(QStringLiteral("trip.mkv")), "mkv").inside == at(2024, 7, 14, 13, 22, 5, QTimeZone::UTC),
           QStringLiteral("names: the date inside a document, an email and a film is kept"));
    const lost::Found photo = named(jpegWith(exifTiff(true)), "jpg");
    report(photo.name.endsWith(QLatin1String(" Canon EOS 80D.jpg")) && photo.name.startsWith(QLatin1String("20")) && photo.inside != 0,
           QStringLiteral("names: a photo by when it was taken and the camera"), photo.name);
    const lost::Found plain = named(picture("JPG"), "jpg");
    report(plain.name == lost::carvedName(plain.type, 1) && plain.inside == 0, QStringLiteral("names: one that says nothing keeps its made-up name"), plain.name);
    const QByteArray titled = "%PDF-1.4\n1 0 obj\n<< /Title (Holiday \\(plans\\) for 14/07) /CreationDate (D:20240714132205+02'00') >>\nendobj\n"
                              "trailer\n<< /Info 1 0 R >>\n%%EOF\n";
    const lost::Found pdfNamed = named(titled, "pdf");
    report(pdfNamed.name == QLatin1String("Holiday (plans) for 14-07.pdf") && pdfNamed.inside == at(2024, 7, 14, 11, 22, 5, QTimeZone::UTC),
           QStringLiteral("names: a PDF by its title, its date with its time zone"), pdfNamed.name);
    const QByteArray wide = "%PDF-1.7\ntrailer << /Title <FEFF004C00E4006B0065> >>\n%%EOF\n";
    report(named(wide, "pdf").name == QStringLiteral("Läke.pdf"), QStringLiteral("names: a PDF title in UTF-16"));
    const QByteArray core = "<?xml version=\"1.0\"?><cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
                            "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\"><dc:title>Budget 2024</dc:title>"
                            "<dcterms:created>2024-07-14T13:22:05Z</dcterms:created></cp:coreProperties>";
    for (const bool packed : {false, true}) {
        const QByteArray file = zip({{"[Content_Types].xml", "<Types/>"}, {"word/document.xml", QByteArray(3000, 'w')}, {"docProps/core.xml", core}}, packed);
        report(named(file, "docx").name == QLatin1String("Budget 2024.docx"),
               QStringLiteral("names: an Office file by the title in its summary (%1)").arg(packed ? QStringLiteral("packed") : QStringLiteral("stored")));
    }
    // Damaged ones never crash it.
    QRandomGenerator rng(77);
    int runs = 0;
    for (const Case &c : cases) {
        const QByteArray good = typesFile(QString::fromLatin1(c.file));
        for (int round = 0; round < 40; ++round, ++runs) {
            QByteArray bad = good;
            for (int k = 0, n = 1 + int(rng.bounded(16)); k < n; ++k)
                bad[int(rng.bounded(quint32(bad.size())))] = char(rng.bounded(256));
            if (round % 3 == 0)
                bad.truncate(qsizetype(rng.bounded(quint32(bad.size()))) + 1);
            named(bad, c.id);
        }
    }
    report(true, QStringLiteral("names: %1 damaged files named without a crash").arg(runs));
}

// The names of the parts that don't check out.
QStringList problemsIn(const QVector<structure::Part> &parts)
{
    QStringList out;
    for (const structure::Part &p : parts) {
        if (!p.problem.isEmpty())
            out << p.name;
    }
    return out;
}

// What a file is made of, part by part, and where it's damaged.
void fileStructure()
{
    const auto type = [](const char *id) { return filetypes::typeForName(QStringLiteral("x.") + QString::fromLatin1(id)); };
    const auto names = [](const QVector<structure::Part> &parts) {
        QStringList out;
        for (const structure::Part &p : parts)
            out << QString(p.depth, QLatin1Char('>')) + p.name;
        return out.join(QLatin1Char(' '));
    };
    const auto problems = [](const QVector<structure::Part> &parts) {
        QStringList out;
        for (const structure::Part &p : parts) {
            if (!p.problem.isEmpty())
                out << p.name;
        }
        return out;
    };
    const QByteArray jpg = picture("JPG");
    const QVector<structure::Part> jpgParts = structure::parts(type("jpg"), jpg);
    QString size;
    for (const structure::Part &p : jpgParts)
        size += p.name == QLatin1String("SOF0") ? p.detail : QString();
    report(names(jpgParts).startsWith(QLatin1String("SOI ")) && names(jpgParts).contains(QLatin1String("SOS >Picture data EOI"))
               && size.startsWith(QStringLiteral("320 × 200")) && problems(jpgParts).isEmpty(),
           QStringLiteral("structure: a JPEG's markers, its size and its picture data"), names(jpgParts) + QStringLiteral(" | ") + size + QStringLiteral(" | ")
               + problems(jpgParts).join(QLatin1Char(' ')));
    report(problems(structure::parts(type("jpg"), jpg.left(jpg.size() - 300))) == QStringList{QStringLiteral("Picture data"), QStringLiteral("EOI")},
           QStringLiteral("structure: a JPEG cut short says so"), problems(structure::parts(type("jpg"), jpg.left(jpg.size() - 300))).join(QLatin1Char(' ')));
    QByteArray png = picture("PNG");
    report(problems(structure::parts(type("png"), png)).isEmpty() && names(structure::parts(type("png"), png)).startsWith(QLatin1String("Signature IHDR")),
           QStringLiteral("structure: a PNG's chunks, each checksum right"));
    png[png.indexOf("IDAT") + 40] = char(png[png.indexOf("IDAT") + 40] ^ 0x55);
    report(problems(structure::parts(type("png"), png)) == QStringList{QStringLiteral("IDAT")}, QStringLiteral("structure: a damaged PNG chunk is pointed at"));
    report(names(structure::parts(type("gif"), gif())).endsWith(QLatin1String("Picture 1 End"))
               && names(structure::parts(type("bmp"), picture("BMP"))).startsWith(QLatin1String("File header Picture header")),
           QStringLiteral("structure: GIF and BMP"), names(structure::parts(type("gif"), gif())));
    const QString mkv = names(structure::parts(type("mkv"), typesFile(QStringLiteral("trip.mkv"))));
    report(mkv.startsWith(QLatin1String("EBML header Segment >SeekHead")) && mkv.contains(QLatin1String(">Info >Tracks >>TrackEntry"))
               && mkv.contains(QLatin1String(">Cluster")),
           QStringLiteral("structure: a Matroska film's elements"), mkv.left(120));
    const QString flacParts = names(structure::parts(type("flac"), typesFile(QStringLiteral("song.flac"))));
    report(flacParts.startsWith(QLatin1String("fLaC STREAMINFO")) && flacParts.contains(QLatin1String("PICTURE")) && flacParts.endsWith(QLatin1String("Sound")),
           QStringLiteral("structure: a FLAC's blocks"), flacParts);
    const QString wmvParts = names(structure::parts(type("wmv"), typesFile(QStringLiteral("trip.wmv"))));
    report(wmvParts.startsWith(QLatin1String("Header >File properties")) && wmvParts.contains(QLatin1String(" Data")), QStringLiteral("structure: a WMV's objects"),
           wmvParts);
    QByteArray office = zip({{"[Content_Types].xml", "<Types/>"}, {"word/document.xml", QByteArray(3000, 'w')}});
    report(names(structure::parts(type("docx"), office)) == QLatin1String("[Content_Types].xml word/document.xml Directory End of the directory")
               && problems(structure::parts(type("docx"), office)).isEmpty(),
           QStringLiteral("structure: a ZIP's entries, each checked against its checksum"), names(structure::parts(type("docx"), office)));
    office[office.indexOf("wwww") + 100] = 'x';
    report(problems(structure::parts(type("docx"), office)) == QStringList{QStringLiteral("word/document.xml")}, QStringLiteral("structure: a damaged entry is pointed at"));
    const QByteArray zipped = zip({{"a.txt", QByteArray(5000, 'a')}}, true);
    report(problems(structure::parts(type("zip"), zipped)).isEmpty(), QStringLiteral("structure: a packed entry unpacks and checks out"));
    const QByteArray pdfFile = pdf();
    const QString pdfParts = names(structure::parts(type("pdf"), pdfFile));
    report(pdfParts.startsWith(QLatin1String("Header Object 1 0")) && pdfParts.contains(QLatin1String("Trailer startxref")) && problems(structure::parts(type("pdf"), pdfFile)).isEmpty(),
           QStringLiteral("structure: a PDF's objects, its table and trailer"), pdfParts);
    report(problems(structure::parts(type("pdf"), QByteArray("%PDF-1.4\n%moved\n") + pdfFile.mid(9))) == QStringList{QStringLiteral("startxref")},
           QStringLiteral("structure: a PDF whose table points to the wrong place says so"));
    const QString tif = names(structure::parts(type("tif"), typesFile(QStringLiteral("picture.tif"))));
    report(tif.startsWith(QLatin1String("Header Folder 1 >Picture data")), QStringLiteral("structure: a TIFF's folders and picture data"), tif);
    const QString docParts = names(structure::parts(type("doc"), typesFile(QStringLiteral("notes.doc"))));
    report(docParts.startsWith(QLatin1String("Header")) && docParts.contains(QLatin1String("WordDocument")) && docParts.contains(QLatin1String("1Table")),
           QStringLiteral("structure: a Word 97 file's streams"), docParts);
    QByteArray gz = typesFile(QStringLiteral("pictures.tar.gz"));
    report(problems(structure::parts(type("gz"), gz)).isEmpty(), QStringLiteral("structure: a gzip part checks out"));
    gz[gz.size() - 6] = char(gz[gz.size() - 6] ^ 1);
    report(problems(structure::parts(type("gz"), gz)).size() == 1, QStringLiteral("structure: a damaged gzip part is pointed at"));
    report(problems(structure::parts(type("rar"), rar5())).isEmpty() && problems(structure::parts(type("rar"), rar4())).isEmpty()
               && names(structure::parts(type("rar"), rar5())).endsWith(QLatin1String("End")),
           QStringLiteral("structure: RAR blocks, their checksums right"), names(structure::parts(type("rar"), rar5())));
    report(names(structure::parts(type("psd"), psd(true))).startsWith(QLatin1String("Header Colours Resources Layers Picture data")),
           QStringLiteral("structure: a Photoshop file's sections"));
    const QString mp4Parts = names(structure::parts(type("mp4"), mp4()));
    report(mp4Parts.startsWith(QLatin1String("ftyp")) && mp4Parts.contains(QLatin1String("moov >mvhd")) && mp4Parts.contains(QLatin1String("mdat")),
           QStringLiteral("structure: an MP4's boxes"), mp4Parts);
    QByteArray noIndex = mp4();
    const qsizetype moov = noIndex.indexOf("moov") - 4;
    noIndex.remove(moov, qFromBigEndian<quint32>(noIndex.constData() + moov));
    report(problems(structure::parts(type("mp4"), noIndex)) == QStringList{QStringLiteral("moov")}, QStringLiteral("structure: an MP4 without its index says so"));
    report(problems(structure::parts(type("ogg"), ogg())).isEmpty() && problems(structure::parts(type("7z"), sevenZip())).isEmpty(),
           QStringLiteral("structure: Ogg pages and 7z headers check out"));
    // Damaged ones never crash it.
    QRandomGenerator rng(4242);
    const QVector<QPair<const char *, QByteArray>> kinds = {
        {"jpg", picture("JPG")}, {"png", picture("PNG")}, {"gif", gif()}, {"bmp", picture("BMP")}, {"mkv", typesFile(QStringLiteral("trip.mkv"))},
        {"flac", typesFile(QStringLiteral("song.flac"))}, {"wmv", typesFile(QStringLiteral("trip.wmv"))}, {"docx", zip({{"a.xml", QByteArray(3000, 'a')}}, true)},
        {"pdf", pdf()}, {"tif", typesFile(QStringLiteral("picture.tif"))}, {"doc", typesFile(QStringLiteral("notes.doc"))}, {"gz", typesFile(QStringLiteral("pictures.tar.gz"))},
        {"rar", rar5()}, {"rar", rar4()}, {"psd", psd(true)}, {"mp4", mp4()}, {"ogg", ogg()}, {"7z", sevenZip()}, {"sqlite", sqlite()}, {"mp3", mp3()}, {"wav", wav()}};
    int runs = 0;
    for (const auto &[id, good] : kinds) {
        if (good.isEmpty()) {
            report(false, QStringLiteral("structure: a %1 to damage").arg(QString::fromLatin1(id)));
            continue;
        }
        for (int round = 0; round < 150; ++round, ++runs) {
            QByteArray bad = good;
            for (int k = 0, n = 1 + int(rng.bounded(16)); k < n; ++k)
                bad[int(rng.bounded(quint32(bad.size())))] = char(rng.bounded(256));
            if (round % 3 == 0)
                bad.truncate(qsizetype(rng.bounded(quint32(bad.size()))) + 1);
            structure::parts(type(id), bad);
        }
    }
    report(true, QStringLiteral("structure: %1 damaged files taken apart without a crash").arg(runs));
}

// --- Repairs: each kind broken the way it breaks for real, repaired on a copy, and checked by
// something else that reads it (Qt's decoders, libarchive, poppler, zlib, sqlite3), and films
// frame by frame against the original.

QByteArray repairFile(const QString &name)
{
    QFile f(QStringLiteral(SOURCE_DIR "/tests/data/lostfs/repair/") + name);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

struct FilmTable {
    QVector<quint32> sizes;
    QVector<quint64> offsets; // of each frame (worked out from the chunks' with stsc)
    QVector<qint64> shown;    // when each is shown, after it's decoded (ctts), when they're reordered
    quint64 soundSamples = 0;
};

// A film's picture (and sound) tables, read plainly: enough for the test to compare frames.
FilmTable filmTable(const QByteArray &d)
{
    FilmTable t;
    QVector<quint64> chunks;
    QVector<QPair<quint32, quint32>> runs; // stsc: from which chunk on, how many frames each
    const auto be32 = [&d](qsizetype at) { return at + 4 <= d.size() ? qFromBigEndian<quint32>(d.constData() + at) : 0u; };
    const auto be64 = [&d](qsizetype at) { return at + 8 <= d.size() ? qFromBigEndian<quint64>(d.constData() + at) : 0ull; };
    std::function<void(qsizetype, qsizetype, QByteArray)> walk = [&](qsizetype from, qsizetype to, QByteArray handler) {
        for (qsizetype at = from; at + 8 <= to;) {
            const qsizetype size = qsizetype(be32(at));
            const QByteArray type = d.mid(at + 4, 4);
            if (size < 8 || at + size > to)
                return;
            if (type == "moov" || type == "trak" || type == "mdia" || type == "minf" || type == "stbl") {
                // The handler (vide, soun) is known before the tables in its trak.
                if (type == "trak") {
                    const qsizetype hdlr = d.indexOf("hdlr", at);
                    handler = hdlr > 0 && hdlr < at + size ? d.mid(hdlr + 12, 4) : QByteArray();
                }
                walk(at + 8, at + size, handler);
            } else if (type == "stsz" && handler == "vide") {
                for (quint32 i = 0; i < be32(at + 16); ++i)
                    t.sizes << be32(at + 20 + 4 * qsizetype(i));
            } else if (type == "stsz" && handler == "soun") {
                t.soundSamples += be32(at + 16);
            } else if ((type == "stco" || type == "co64") && handler == "vide") {
                for (quint32 i = 0; i < be32(at + 12); ++i)
                    chunks << (type == "co64" ? be64(at + 16 + 8 * qsizetype(i)) : be32(at + 16 + 4 * qsizetype(i)));
            } else if (type == "ctts" && handler == "vide") {
                for (quint32 i = 0; i < be32(at + 12); ++i)
                    for (quint32 k = 0; k < be32(at + 16 + 8 * qsizetype(i)) && t.shown.size() < 1000000; ++k)
                        t.shown << qint32(be32(at + 20 + 8 * qsizetype(i)));
            } else if (type == "stsc" && handler == "vide") {
                for (quint32 i = 0; i < be32(at + 12); ++i)
                    runs << qMakePair(be32(at + 16 + 12 * qsizetype(i)), be32(at + 20 + 12 * qsizetype(i)));
            }
            at += size;
        }
    };
    walk(0, d.size(), {});
    // Each chunk holds so many frames, one after another.
    qsizetype frame = 0;
    for (qsizetype c = 0; c < chunks.size(); ++c) {
        quint32 perChunk = 1;
        for (const auto &[first, count] : std::as_const(runs)) {
            if (quint32(c + 1) >= first)
                perChunk = count;
        }
        quint64 at = chunks[c];
        for (quint32 k = 0; k < perChunk && frame < t.sizes.size(); ++k, ++frame) {
            t.offsets << at;
            at += t.sizes[frame];
        }
    }
    return t;
}

struct Repaired {
    repair::Result result;
    QByteArray data;
};

Repaired repaired(const char *id, const QByteArray &broken, const QByteArray &reference = {}, bool keep = true)
{
    QTemporaryDir dir;
    const QString in = dir.filePath(QStringLiteral("in")), out = dir.filePath(QStringLiteral("out")), ref = dir.filePath(QStringLiteral("ref"));
    QFile f(in);
    if (!f.open(QIODevice::WriteOnly) || f.write(broken) != broken.size())
        return {};
    f.close();
    if (!reference.isEmpty()) {
        QFile r(ref);
        if (!r.open(QIODevice::WriteOnly) || r.write(reference) != reference.size())
            return {};
    }
    Repaired got;
    got.result = repair::run(filetypes::typeForName(QStringLiteral("x.") + QString::fromLatin1(id)), in, out, reference.isEmpty() ? QString() : ref);
    QFile o(out);
    if (o.open(QIODevice::ReadOnly))
        got.data = o.readAll();
    if (keep && qEnvironmentVariableIsSet("DISKANVIL_REPAIR_OUT") && !got.data.isEmpty()) { // to check by hand (ffprobe, pdfinfo...)
        QFile keep(qEnvironmentVariable("DISKANVIL_REPAIR_OUT") + QStringLiteral("/repaired-") + QString::fromLatin1(id) + QLatin1Char('-')
                   + QString::number(qHash(broken), 16) + QLatin1Char('.') + QString::fromLatin1(id));
        if (keep.open(QIODevice::WriteOnly))
            keep.write(got.data);
    }
    return got;
}

void repairs()
{
    // PNG: a wrong checksum and its end cut off.
    QByteArray png = picture("PNG");
    const qsizetype idat = png.indexOf("IDAT");
    png[idat - 4 + 8 + qFromBigEndian<quint32>(png.constData() + idat - 4) + 1] ^= 0x10; // its CRC
    png.chop(12); // IEND
    const Repaired pngFixed = repaired("png", png);
    report(pngFixed.result.ok && !QImage::fromData(pngFixed.data, "PNG").isNull() && QImage::fromData(png, "PNG").isNull(),
           QStringLiteral("repair: a PNG with a wrong checksum and no end opens again"), pngFixed.result.done.join(QStringLiteral("; ")) + pngFixed.result.why);

    // JPEG: cut short; and its start damaged, borrowed from another photo of the same size and quality.
    const QByteArray jpg = picture("JPG");
    const Repaired cut = repaired("jpg", jpg.left(jpg.size() - 400));
    report(cut.result.ok && cut.data.endsWith("\xFF\xD9") && !QImage::fromData(cut.data, "JPEG").isNull(),
           QStringLiteral("repair: a JPEG cut short gets its end back and opens"), cut.result.done.join(QStringLiteral("; ")) + cut.result.why);
    QImage other(320, 200, QImage::Format_RGB32);
    other.fill(QColor(0x2b, 0xee, 0x8a));
    QByteArray otherJpg;
    {
        QBuffer buffer(&otherJpg);
        buffer.open(QIODevice::WriteOnly);
        other.save(&buffer, "JPG", 85); // the same camera: the same size and quality
    }
    QByteArray startless = jpg;
    const qsizetype sos = startless.indexOf("\xFF\xDA");
    for (qsizetype i = 0; i < sos; ++i)
        startless[i] = char(i * 13); // everything before the picture data: gone
    const Repaired noRef = repaired("jpg", startless);
    const Repaired borrowed = repaired("jpg", startless, otherJpg);
    const qsizetype lentSos = otherJpg.indexOf("\xFF\xDA");
    qsizetype firstDifference = 0;
    while (firstDifference < std::min(sos, lentSos) && jpg[firstDifference] == otherJpg[firstDifference])
        ++firstDifference;
    report(!noRef.result.ok && noRef.result.needsReference && borrowed.result.ok
               && QImage::fromData(borrowed.data, "JPEG").convertToFormat(QImage::Format_RGB32) == QImage::fromData(jpg, "JPEG").convertToFormat(QImage::Format_RGB32),
           QStringLiteral("repair: a JPEG whose start is lost asks for another photo, and with its start shows exactly as it was"),
           borrowed.result.done.join(QStringLiteral("; ")) + borrowed.result.why
               + QStringLiteral(" | headers %1 and %2 bytes, the same to byte %3").arg(sos).arg(lentSos).arg(firstDifference));

    // ZIP (a Word file): its directory cut off; and one whose entries give their sizes only after their data.
    const QList<QPair<QByteArray, QByteArray>> members = {{"[Content_Types].xml", "<Types/>"}, {"word/document.xml", QByteArray(9000, 'w')},
                                                          {"docProps/core.xml", "<cp:coreProperties/>"}};
    QByteArray office = zip(members, true);
    office.truncate(office.indexOf("PK\x01\x02"));
    const Repaired officeFixed = repaired("docx", office);
    QStringList listed;
    for (const officetext::Entry &e : officetext::list(officeFixed.data, 10))
        listed << e.path;
    report(officeFixed.result.ok && listed == QStringList{QStringLiteral("[Content_Types].xml"), QStringLiteral("word/document.xml"), QStringLiteral("docProps/core.xml")}
               && problemsIn(structure::parts(filetypes::typeForName(QStringLiteral("x.docx")), officeFixed.data)).isEmpty(),
           QStringLiteral("repair: a Word file whose list of what's inside was lost gets it back, every entry checking out"),
           officeFixed.result.done.join(QStringLiteral("; ")) + officeFixed.result.why + QStringLiteral(" | ") + listed.join(QLatin1Char(',')));
    QByteArray streamed;
    for (const auto &[name, content] : members) {
        QByteArray packed(int(compressBound(uLong(content.size()))) + 64, '\0');
        z_stream z{};
        deflateInit2(&z, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(content.constData()));
        z.avail_in = uInt(content.size());
        z.next_out = reinterpret_cast<Bytef *>(packed.data());
        z.avail_out = uInt(packed.size());
        deflate(&z, Z_FINISH);
        packed.truncate(qsizetype(z.total_out));
        deflateEnd(&z);
        const quint32 crc = filetypes::crc32(reinterpret_cast<const uchar *>(content.constData()), int(content.size()));
        streamed += le(0x04034b50, 4) + le(20, 2) + le(8, 2) + le(8, 2) + le(0, 4) + le(0, 4) + le(0, 4) + le(0, 4) + le(quint64(name.size()), 2) + le(0, 2) + name
                  + packed + le(0x08074b50, 4) + le(crc, 4) + le(quint64(packed.size()), 4) + le(quint64(content.size()), 4);
    }
    const Repaired streamFixed = repaired("docx", streamed);
    report(streamFixed.result.ok && officetext::list(streamFixed.data, 10).size() == 3
               && problemsIn(structure::parts(filetypes::typeForName(QStringLiteral("x.docx")), streamFixed.data)).isEmpty(),
           QStringLiteral("repair: a ZIP whose sizes come after its data is rebuilt too"), streamFixed.result.done.join(QStringLiteral("; ")) + streamFixed.result.why);

    // PDF: its table pointing to the wrong place; and one whose objects are packed in object streams.
    QByteArray shifted = QByteArray("%PDF-1.4\n%moved by an editor\n") + pdf().mid(9);
    const Repaired pdfFixed = repaired("pdf", shifted);
    preview::Request page;
    page.type = filetypes::typeForName(QStringLiteral("x.pdf"));
    page.pixels = 200;
    page.data = pdfFixed.data;
    page.size = quint64(page.data.size());
    const bool pdfOpens = !preview::make(page).image.isNull();
    QByteArray packedPdf = repairFile(QStringLiteral("packed.pdf"));
    const qsizetype startxref = packedPdf.lastIndexOf("startxref");
    packedPdf = packedPdf.left(startxref) + "startxref\n12\n%%EOF\n"; // points nowhere now
    const Repaired packedFixed = repaired("pdf", packedPdf);
    page.data = packedFixed.data;
    page.size = quint64(page.data.size());
    const preview::Result packedPage = preview::make(page);
    report(pdfFixed.result.ok && pdfOpens, QStringLiteral("repair: a PDF whose table points to the wrong place opens again"),
           pdfFixed.result.done.join(QStringLiteral("; ")) + pdfFixed.result.why);
    report(packedFixed.result.ok && !packedPage.image.isNull() && packedPage.text.contains(QLatin1String("2 page")),
           QStringLiteral("repair: a PDF with its objects packed in object streams gets a new table and opens, both pages"),
           packedFixed.result.done.join(QStringLiteral("; ")) + packedFixed.result.why + QStringLiteral(" | ") + packedPage.text);

    // gzip: cut short, what's there kept.
    const QByteArray big = QByteArray(200000, 'g') + QByteArray(200000, 'z');
    QByteArray gz;
    {
        z_stream z{};
        deflateInit2(&z, 6, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY);
        QByteArray out(int(compressBound(uLong(big.size()))) + 64, '\0');
        z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(big.constData()));
        z.avail_in = uInt(big.size());
        z.next_out = reinterpret_cast<Bytef *>(out.data());
        z.avail_out = uInt(out.size());
        deflate(&z, Z_FINISH);
        gz = out.left(qsizetype(z.total_out));
        deflateEnd(&z);
    }
    const Repaired gzFixed = repaired("gz", gz.left(gz.size() / 2));
    QByteArray unpacked(big.size(), '\0');
    uLongf unpackedSize = uLongf(unpacked.size());
    {
        z_stream z{};
        inflateInit2(&z, MAX_WBITS + 16);
        z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(gzFixed.data.constData()));
        z.avail_in = uInt(gzFixed.data.size());
        z.next_out = reinterpret_cast<Bytef *>(unpacked.data());
        z.avail_out = uInt(unpacked.size());
        const int result = inflate(&z, Z_FINISH);
        unpackedSize = result == Z_STREAM_END ? z.total_out : 0;
        inflateEnd(&z);
    }
    report(gzFixed.result.ok && unpackedSize > 1000 && big.startsWith(unpacked.left(qsizetype(unpackedSize))),
           QStringLiteral("repair: a gzip file cut short keeps what still unpacks, whole and checking out"), gzFixed.result.done.join(QStringLiteral("; ")) + gzFixed.result.why);

    // WAV: a recording cut off, its sizes still saying the whole length.
    const QByteArray wave = wav();
    const QByteArray cutWave = wave.left(wave.size() - 1000);
    const Repaired waveFixed = repaired("wav", cutWave);
    const qsizetype data = waveFixed.data.indexOf("data");
    report(waveFixed.result.ok && qFromLittleEndian<quint32>(waveFixed.data.constData() + 4) == quint32(cutWave.size() - 8)
               && qFromLittleEndian<quint32>(waveFixed.data.constData() + data + 4) == quint32(cutWave.size() - data - 8),
           QStringLiteral("repair: a WAV recording that was cut off gets its sizes right"), waveFixed.result.done.join(QStringLiteral("; ")) + waveFixed.result.why);

    // SQLite: a damaged page, recovered with sqlite3 (when it's there).
    if (host::programExists(QStringLiteral("sqlite3"))) {
        QTemporaryDir dir;
        const QString db = dir.filePath(QStringLiteral("notes.db"));
        QProcess::execute(QStringLiteral("sqlite3"), {db, QStringLiteral("CREATE TABLE notes(t TEXT); WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<2000) "
                                                                        "INSERT INTO notes SELECT 'note number ' || i FROM n;")});
        QFile file(db);
        QByteArray bytes = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        for (qsizetype i = 4096 * 3; i < 4096 * 3 + 200; ++i)
            bytes[i] = char(0xEE); // a page damaged
        const Repaired dbFixed = repaired("sqlite", bytes);
        QTemporaryDir check;
        QFile out(check.filePath(QStringLiteral("fixed.db")));
        if (out.open(QIODevice::WriteOnly))
            out.write(dbFixed.data);
        out.close();
        QProcess count;
        count.start(QStringLiteral("sqlite3"), {out.fileName(), QStringLiteral("SELECT count(*) FROM notes;")});
        count.waitForFinished(10000);
        const int rows = count.readAllStandardOutput().trimmed().toInt();
        report(dbFixed.result.ok && rows > 1500 && rows < 2000, QStringLiteral("repair: a damaged SQLite database's other rows are recovered (sqlite3)"),
               QStringLiteral("%1 rows; ").arg(rows) + dbFixed.result.done.join(QStringLiteral("; ")) + dbFixed.result.why);
    } else {
        report(true, QStringLiteral("repair: SQLite skipped, sqlite3 isn't here"));
    }

    // Films: the index cut off where the recording stopped, rebuilt from a clip that plays. Every
    // frame is checked against the original: the same size, the same bytes where it says.
    struct Film {
        const char *file, *reference, *id;
        bool sound;
    };
    const Film films[] = {{"h264-pcm.mov", "h264-pcm-ref.mov", "mov", true},
                          {"h264-aac.mp4", "h264-aac-ref.mp4", "mp4", false},
                          {"mjpeg.mov", "mjpeg-ref.mov", "mov", true},
                          {"h265.mp4", "h265-ref.mp4", "mp4", false}};
    for (const Film &film : films) {
        const QByteArray whole = repairFile(QString::fromLatin1(film.file)), reference = repairFile(QString::fromLatin1(film.reference));
        if (whole.isEmpty()) {
            report(QString::fromLatin1(film.file).startsWith(QLatin1String("h265")), QStringLiteral("repair: %1 to test with").arg(QString::fromLatin1(film.file)));
            continue;
        }
        const FilmTable original = filmTable(whole);
        const qsizetype moov = whole.lastIndexOf("moov") - 4;
        for (const bool unfinished : {false, true}) {
            // As a recording that stopped: no index; unfinished, the data box's size not written
            // and the last frame cut short too.
            QByteArray broken = whole.left(moov);
            if (unfinished) {
                const qsizetype mdat = broken.indexOf("mdat") - 4;
                qToBigEndian(quint32(0), broken.data() + mdat);
                broken.chop(original.sizes.last() / 2);
            }
            const Repaired fixed = repaired(film.id, broken, reference);
            const FilmTable rebuilt = filmTable(fixed.data);
            // The frames still whole in what's left (with sound between them, the end cut may be only sound).
            qsizetype want = 0;
            while (want < original.sizes.size() && original.offsets.value(want) + original.sizes[want] <= quint64(broken.size()))
                ++want;
            bool same = fixed.result.ok && rebuilt.sizes.size() == want && rebuilt.offsets.size() == want;
            QString differs;
            for (qsizetype i = 0; same && i < want; ++i) {
                same = rebuilt.sizes[i] == original.sizes[i]
                    && fixed.data.mid(qsizetype(rebuilt.offsets[i]), qsizetype(rebuilt.sizes[i])) == whole.mid(qsizetype(original.offsets[i]), qsizetype(original.sizes[i]));
                if (!same)
                    differs = QStringLiteral("frame %1: %2 bytes, should be %3; ").arg(i).arg(rebuilt.sizes[i]).arg(original.sizes[i]);
            }
            const bool sound = film.sound ? rebuilt.soundSamples > original.soundSamples * 9 / 10 : rebuilt.soundSamples == 0;
            // Frames stored out of the order they're shown in (B-frames) are shown in the same order again.
            bool order = original.shown.isEmpty() == rebuilt.shown.isEmpty();
            for (qsizetype i = 0; order && i < std::min(want, rebuilt.shown.size()); ++i)
                order = rebuilt.shown[i] - rebuilt.shown[0] == original.shown[i] - original.shown[0];
            if (!order)
                differs += QStringLiteral("shown in another order; ");
            report(same && sound && order,
                   QStringLiteral("repair: %1%2, its index rebuilt: every frame where it was%3")
                       .arg(QString::fromLatin1(film.file), unfinished ? QStringLiteral(" (unfinished)") : QString(),
                            film.sound ? QStringLiteral(", and its sound") : QString()),
                   differs + QStringLiteral("%1 of %2 frames, sound %3 of %4; ").arg(rebuilt.sizes.size()).arg(original.sizes.size()).arg(rebuilt.soundSamples).arg(original.soundSamples)
                       + fixed.result.done.join(QStringLiteral("; ")) + fixed.result.why);
        }
    }
    const Repaired noClip = repaired("mp4", repairFile(QStringLiteral("h264-aac.mp4")).left(10000));
    report(!noClip.result.ok && noClip.result.needsReference, QStringLiteral("repair: a film asks for a clip that plays first"));
    const Repaired wrongClip = repaired("mov", repairFile(QStringLiteral("h264-pcm.mov")).left(20000), repairFile(QStringLiteral("mjpeg-ref.mov")));
    report(!wrongClip.result.ok && wrongClip.result.why.contains(QLatin1String("No frames")), QStringLiteral("repair: a clip from another camera finds no frames, and says so"),
           wrongClip.result.why);

    // Damaged ones never crash any of it.
    QRandomGenerator rng(9090);
    const QVector<QPair<const char *, QByteArray>> kinds = {{"png", picture("PNG")}, {"jpg", picture("JPG")}, {"docx", zip(members, true)}, {"pdf", pdf()},
                                                            {"pdf", repairFile(QStringLiteral("packed.pdf"))}, {"gz", gz}, {"wav", wav()},
                                                            {"mov", repairFile(QStringLiteral("h264-pcm.mov"))}, {"mp4", repairFile(QStringLiteral("h264-aac.mp4"))}};
    int runs = 0;
    for (const auto &[id, good] : kinds) {
        const bool film = QByteArray(id) == "mov" || QByteArray(id) == "mp4";
        const QByteArray reference = film ? repairFile(QByteArray(id) == "mov" ? QStringLiteral("h264-pcm-ref.mov") : QStringLiteral("h264-aac-ref.mp4")) : otherJpg;
        for (int round = 0; round < (film ? 30 : 60); ++round, ++runs) {
            QByteArray bad = good;
            for (int k = 0, n = 1 + int(rng.bounded(24)); k < n; ++k)
                bad[int(rng.bounded(quint32(bad.size())))] = char(rng.bounded(256));
            if (round % 3 == 0)
                bad.truncate(qsizetype(rng.bounded(quint32(bad.size()))) + 1);
            repaired(id, bad, round % 2 ? reference : QByteArray(), false);
        }
    }
    report(true, QStringLiteral("repair: %1 damaged files repaired or refused without a crash").arg(runs));
}

// Text found by words remembered from it, however it was kept.
void wordSearch()
{
    QByteArray drive = noise(1024 * 1024, 4242);
    const auto put = [&drive](qsizetype at, const QByteArray &bytes) { drive.replace(at, bytes.size(), bytes); };
    put(100000, QByteArray(1, '\0') + "Hi Alex,\nMeet me at the FERRY pier on Sunday at 07:30.\nSam" + QByteArray(1, '\0'));
    const QString wide = QStringLiteral("The quarterly report for Smith is late again.");
    put(300000, QByteArray(2, '\0') + QByteArray(reinterpret_cast<const char *>(wide.utf16()), wide.size() * 2) + QByteArray(2, '\0'));
    put(500000, QByteArray(1, '\0') + QByteArray("Budget: caf\xE9 cr\xE8me, 3.75") + QByteArray(1, '\0')); // Windows' western code page
    put(700000, QByteArray(1, '\0') + "a ferry ticket alone" + QByteArray(1, '\0'));
    put(900000, QByteArray(1, '\0') + "Sunday, much later" + QByteArray(1, '\0'));
    const auto source = lost::Source::fromMemory(drive);
    lost::Found known;
    known.name = QStringLiteral("letter.doc");
    known.extents = {{99000, 4000}};
    const auto search = [&](const QStringList &words) {
        QVector<lost::Found> got;
        lost::WordSearch s(source, words, {known});
        QObject::connect(&s, &lost::WordSearch::found, [&got](const QVector<lost::Found> &files) { got += files; });
        s.run();
        return got;
    };
    const auto text = [](const lost::Found &f) { return QString::fromUtf8(f.data.mid(3)); };
    const QVector<lost::Found> ferry = search({QStringLiteral("ferry"), QStringLiteral("sunday")});
    report(ferry.size() == 1 && text(ferry[0]) == QLatin1String("Hi Alex,\nMeet me at the FERRY pier on Sunday at 07:30.\nSam")
               && ferry[0].note.contains(QLatin1String("letter.doc")) && ferry[0].folder == QLatin1String("Found by their words"),
           QStringLiteral("words: found close together, capitals or not, with all the text around them, and the file it's in"),
           ferry.isEmpty() ? QString() : text(ferry[0]) + QStringLiteral(" | ") + ferry[0].note);
    const QVector<lost::Found> report1 = search({QStringLiteral("Quarterly"), QStringLiteral("smith")});
    report(report1.size() == 1 && text(report1[0]) == wide && report1[0].note.contains(QLatin1String("Windows")),
           QStringLiteral("words: in UTF-16, the way Windows and Word keep text"), report1.isEmpty() ? QString() : text(report1[0]));
    const QVector<lost::Found> cafe = search({QStringLiteral("café"), QStringLiteral("crème")});
    report(cafe.size() == 1 && text(cafe[0]) == QStringLiteral("Budget: café crème, 3.75"), QStringLiteral("words: in Windows' western code page"),
           cafe.isEmpty() ? QString() : text(cafe[0]));
    report(search({QStringLiteral("ferry"), QStringLiteral("smith")}).isEmpty() && search({QStringLiteral("ticket"), QStringLiteral("sunday")}).isEmpty(),
           QStringLiteral("words: far apart, they're not a place where all of them are"));
    report(search({QStringLiteral("ferry")}).size() == 2, QStringLiteral("words: one word, every place it is"));
}

// The small pictures Windows keeps of photos, out of its thumbnail caches.
void thumbnailCaches()
{
    const QByteArray cache = typesFile(QStringLiteral("thumbcache_256.db")), xp = typesFile(QStringLiteral("Thumbs.db"));
    const QVector<thumbcache::Thumb> modern = thumbcache::read(cache);
    bool open = modern.size() == 3;
    for (const thumbcache::Thumb &t : modern)
        open = open && !QImage::fromData(cache.mid(qsizetype(t.offset), qsizetype(t.size))).isNull();
    report(open && modern[0].kind == QLatin1String("jpg") && modern[1].kind == QLatin1String("png"),
           QStringLiteral("thumbnails: the pictures in a thumbcache file (Windows 10), the empty entry passed over"),
           QStringLiteral("%1 found").arg(modern.size()));
    const QVector<thumbcache::Thumb> old = thumbcache::read(xp);
    QStringList names;
    bool oldOpen = old.size() == 3;
    for (const thumbcache::Thumb &t : old) {
        names << t.name;
        oldOpen = oldOpen && !QImage::fromData(t.data).isNull();
    }
    names.sort();
    report(oldOpen && names == QStringList{QStringLiteral("beach.jpg"), QStringLiteral("dog.png"), QStringLiteral("party.jpg")},
           QStringLiteral("thumbnails: the pictures in an XP Thumbs.db, with the photos' names (one in stream \"01\")"), names.join(QLatin1Char(',')));
    report(thumbcache::isCache(QStringLiteral("thumbcache_1024.db")) && thumbcache::isCache(QStringLiteral("Thumbs.db"))
               && !thumbcache::isCache(QStringLiteral("thumbcache_1024.db.txt")) && !thumbcache::isCache(QStringLiteral("photos.db")),
           QStringLiteral("thumbnails: the caches are known by their names"));
    // On a drive, the cache in two pieces: each picture is read from where it is, across the gap too.
    QByteArray drive = noise(256 * 1024, 77);
    const qsizetype half = cache.size() / 2;
    drive.replace(4096, half, cache.left(half));
    drive.replace(100000, cache.size() - half, cache.mid(half));
    lost::Found file;
    file.name = QStringLiteral("thumbcache_256.db");
    file.folder = QStringLiteral("Users/Sam/AppData/Local/Microsoft/Windows/Explorer");
    file.size = quint64(cache.size());
    file.extents = {{4096, quint64(half)}, {100000, quint64(cache.size() - half)}};
    const auto source = lost::Source::fromMemory(drive);
    const QVector<lost::Found> found = lost::thumbnailsIn(source, file);
    bool read = found.size() == 3;
    for (const lost::Found &f : found)
        read = read && !QImage::fromData(lost::FileReader(source, f).head(qint64(f.size))).isNull() && f.folder == QLatin1String("Small pictures Windows kept");
    report(read && found[0].note.contains(QLatin1String("Explorer/thumbcache_256.db")),
           QStringLiteral("thumbnails: listed as files of their own, read from the cache's pieces on the drive"),
           found.isEmpty() ? QString() : found[0].name + QStringLiteral(" | ") + found[0].note);
    lost::Found xpFile = file;
    xpFile.name = QStringLiteral("Thumbs.db");
    xpFile.size = quint64(xp.size());
    xpFile.data = xp;
    xpFile.extents.clear();
    const QVector<lost::Found> named = lost::thumbnailsIn(source, xpFile);
    QStringList namedFiles;
    for (const lost::Found &f : named)
        namedFiles << f.name;
    namedFiles.sort();
    report(namedFiles == QStringList{QStringLiteral("beach (small copy).jpg"), QStringLiteral("dog (small copy).jpg"), QStringLiteral("party (small copy).jpg")},
           QStringLiteral("thumbnails: an XP cache's pictures named after their photos"), namedFiles.join(QLatin1Char(',')));
    QRandomGenerator rng(321);
    for (int round = 0; round < 400; ++round) {
        QByteArray bad = round % 2 ? cache : xp;
        for (int k = 0, n = 1 + int(rng.bounded(16)); k < n; ++k)
            bad[int(rng.bounded(quint32(bad.size())))] = char(rng.bounded(256));
        if (round % 3 == 0)
            bad.truncate(qsizetype(rng.bounded(quint32(bad.size()))) + 1);
        thumbcache::read(bad);
    }
    report(true, QStringLiteral("thumbnails: 400 damaged caches read without a crash"));
}

// Signs of ransomware among found files: its family, its endings, its notes, the deleted originals.
void ransomwareSigns()
{
    const auto file = [](const QString &folder, const QString &name, bool deleted = false) {
        lost::Found f;
        f.folder = folder;
        f.name = name;
        f.deleted = deleted;
        return f;
    };
    QVector<lost::Found> locky;
    for (int i = 0; i < 10; ++i) {
        locky << file(QStringLiteral("Docs"), QStringLiteral("report%1.docx.locky").arg(i));
        locky << file(QStringLiteral("Docs"), QStringLiteral("report%1.docx").arg(i), true); // the original, deleted
    }
    locky << file(QStringLiteral("Docs"), QStringLiteral("_Locky_recover_instructions.txt"));
    const ransomware::Finding a = ransomware::look(locky);
    report(a.found() && a.family == QLatin1String("Locky") && a.ending == QLatin1String(".locky") && a.locked == 10 && a.deletedOriginals == 10
               && a.notes.size() == 1 && a.decryptor.isEmpty(),
           QStringLiteral("ransomware: Locky, its files, its note, and the originals it deleted"), QStringLiteral("%1 %2 %3").arg(a.family).arg(a.locked).arg(a.deletedOriginals));
    QVector<lost::Found> djvu;
    for (int i = 0; i < 25; ++i)
        djvu << file(QStringLiteral("Pictures"), QStringLiteral("IMG_%1.jpg.qweq").arg(i));
    djvu << file(QStringLiteral("Pictures"), QStringLiteral("_readme.txt"));
    const ransomware::Finding b = ransomware::look(djvu);
    report(b.family == QLatin1String("STOP/Djvu") && b.ending == QLatin1String(".qweq") && b.locked == 25 && !b.decryptor.isEmpty(),
           QStringLiteral("ransomware: STOP/Djvu, known by its note though its ending is new"), b.family + QLatin1Char(' ') + b.decryptor);
    QVector<lost::Found> dharma;
    for (int i = 0; i < 5; ++i)
        dharma << file(QStringLiteral("Share"), QStringLiteral("budget%1.xlsx.id-1E857D00.[restore@mail.example].wallet").arg(i));
    report(ransomware::look(dharma).family == QLatin1String("Dharma (CrySiS)"), QStringLiteral("ransomware: Dharma, its id and address before its ending"));
    QVector<lost::Found> unknown;
    for (int i = 0; i < 25; ++i)
        unknown << file(QStringLiteral("Work"), QStringLiteral("plan%1.pdf.zzqx").arg(i));
    unknown << file(QStringLiteral("Work"), QStringLiteral("HOW_TO_DECRYPT_FILES.txt"));
    const ransomware::Finding c = ransomware::look(unknown);
    report(c.found() && c.family.isEmpty() && c.ending == QLatin1String(".zzqx") && c.notes.size() == 1,
           QStringLiteral("ransomware: one not in the table, by its pattern and note"));
    QVector<lost::Found> ordinary;
    for (int i = 0; i < 40; ++i) {
        ordinary << file(QStringLiteral("Pictures"), QStringLiteral("IMG_%1.jpg").arg(i)) << file(QStringLiteral("Pictures"), QStringLiteral("IMG_%1.jpg.bak").arg(i))
                 << file(QStringLiteral("Downloads"), QStringLiteral("film%1.mp4.part").arg(i));
    }
    ordinary << file(QStringLiteral("Program"), QStringLiteral("README.txt")) << file(QStringLiteral("x"), QStringLiteral("backup.tar.gz"))
             << file(QStringLiteral("x"), QStringLiteral("a.docx.zzqx")); // one alone is no pattern
    const ransomware::Finding quiet = ransomware::look(ordinary);
    report(!quiet.found(), QStringLiteral("ransomware: backups, downloads and ordinary files say nothing"),
           QStringLiteral("%1 %2 %3").arg(quiet.ending).arg(quiet.locked).arg(quiet.notes.join(QLatin1Char(','))));
}

// A failing drive read through into a Rescue Copy image: each part read from the drive once.
void readThrough()
{
    // A drive that counts what's asked of it, with a bad spot.
    struct Drive : lost::Layer {
        QByteArray data;
        lost::Extent bad;
        mutable QMutex lock;
        quint64 asked = 0;
        quint64 size() const override { return quint64(data.size()); }
        bool read(quint64 offset, char *buf, quint64 len) override
        {
            QMutexLocker l(&lock);
            asked += len;
            if (offset < bad.end() && offset + len > bad.start)
                return false;
            memcpy(buf, data.constData() + offset, len);
            return true;
        }
    };
    const QByteArray bytes = noise(4 * 1024 * 1024, 99);
    auto make = [&bytes]() {
        auto d = std::make_unique<Drive>();
        d->data = bytes;
        d->bad = {2 * 1024 * 1024 + 4096, 512};
        return d;
    };
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("drive.img"));
    std::unique_ptr<Drive> first = make();
    Drive *drive = first.get();
    QString why;
    std::unique_ptr<lost::ReadThrough> through = lost::ReadThrough::open(lost::Source::fromLayer(std::move(first)), image, &why);
    lost::ReadThrough *layer = through.get();
    const auto source = lost::Source::fromLayer(std::move(through));
    const auto readBack = [&source](quint64 at, quint64 len) {
        QByteArray out(qsizetype(len), Qt::Uninitialized);
        source->read(at, out.data(), len);
        return out;
    };
    const bool same = readBack(0, 200000) == bytes.left(200000) && readBack(1024 * 1024 + 7, 300000) == bytes.mid(1024 * 1024 + 7, 300000);
    const quint64 askedOnce = drive->asked;
    const bool again = readBack(0, 200000) == bytes.left(200000) && readBack(1024 * 1024 + 7, 300000) == bytes.mid(1024 * 1024 + 7, 300000);
    report(layer && same && again && drive->asked == askedOnce, QStringLiteral("read through: the same bytes, and the drive asked once (the second time from the image)"),
           why + QStringLiteral(" %1 then %2 bytes asked").arg(askedOnce).arg(drive->asked));
    QFile file(image);
    const QByteArray copied = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    report(copied.size() == bytes.size() && copied.left(200000) == bytes.left(200000) && copied.mid(1024 * 1024 + 7, 300000) == bytes.mid(1024 * 1024 + 7, 300000),
           QStringLiteral("read through: what was read is in the image, where it is on the drive"));
    // The bad spot: zeros there (the 64 KiB around it, as the drive gives it), marked for Rescue
    // Copy to try again, and not asked again.
    const quint64 badBlock = 2 * 1024 * 1024;
    const QByteArray around = readBack(badBlock - 4096, 65536 + 8192);
    const quint64 askedBad = drive->asked;
    readBack(badBlock - 4096, 65536 + 8192);
    QString problem;
    RescueMap map;
    const bool loaded = layer->saveMap(&problem) && map.load(layer->mapPath(), quint64(bytes.size()), &problem);
    report(loaded && around.left(4096) == bytes.mid(qsizetype(badBlock) - 4096, 4096) && around.mid(4096, 65536) == QByteArray(65536, '\0')
               && around.right(4096) == bytes.mid(qsizetype(badBlock) + 65536, 4096) && drive->asked == askedBad
               && map.statusAt(badBlock + 4096) == RescueMap::NonTrimmed && map.statusAt(0) == RescueMap::Finished && map.statusAt(3 * 1024 * 1024) == RescueMap::NonTried,
           QStringLiteral("read through: a bad spot is zeros, marked in the map for Rescue Copy, and not asked again"), problem);
    // Another time: it carries on from the map, the drive not asked for what's in the image.
    std::unique_ptr<Drive> second = make();
    Drive *drive2 = second.get();
    const auto again2 = lost::Source::fromLayer(lost::ReadThrough::open(lost::Source::fromLayer(std::move(second)), image, &why));
    QByteArray part(200000, Qt::Uninitialized);
    again2->read(0, part.data(), quint64(part.size()));
    report(part == bytes.left(200000) && drive2->asked == 0, QStringLiteral("read through: another time, it carries on from the image and its map"),
           QStringLiteral("%1 bytes asked").arg(drive2->asked));
}

// btrfs: files deleted since are listed from an older copy of the tree, and copied back
// (btrfs-progs, run as programs of their own).
void btrfsRestore()
{
    report(btrfsrestore::pathRegex({QStringLiteral("/Notes/a.b"), QStringLiteral("/Pictures/2024/x(1).jpg")})
               == QLatin1String("^/(|Notes|Notes/a\\.b|Pictures|Pictures/2024|Pictures/2024/x\\(1\\)\\.jpg)$"),
           QStringLiteral("btrfs: only the ticked files, and the folders on the way to them"),
           btrfsrestore::pathRegex({QStringLiteral("/Notes/a.b"), QStringLiteral("/Pictures/2024/x(1).jpg")}));
    if (!btrfsrestore::available()) {
        report(true, QStringLiteral("btrfs: the rest skipped, btrfs-progs isn't here"));
        return;
    }
    QTemporaryDir dir;
    const QString image = dir.filePath(QStringLiteral("btrfs.img"));
    QFile out(image);
    const QByteArray bytes = unpackImage(QStringLiteral(SOURCE_DIR "/tests/data/lostfs/btrfs.img.zst"));
    if (bytes.isEmpty() || !out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size()) {
        report(false, QStringLiteral("btrfs: unpack the test image"));
        return;
    }
    out.close();
    QString error;
    const auto source = lost::Source::fromImage(image, &error);
    const QVector<lost::Extent> volumes = btrfsrestore::volumes(*source);
    report(volumes.size() == 1 && volumes[0].start == 0, QStringLiteral("btrfs: the file system is found"));
    const auto run = [](const QStringList &command) {
        QProcess p;
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(command.first(), command.mid(1));
        p.waitForFinished(60000);
        return QString::fromUtf8(p.readAll());
    };
    const QVector<btrfsrestore::Tree> trees = btrfsrestore::parseTrees(run(btrfsrestore::findTreesCommand(image)));
    report(trees.size() >= 2 && trees[0].at == 0 && trees[1].generation < trees[0].generation && trees[1].at > 0,
           QStringLiteral("btrfs: the tree in use, and older ones, newest first"), QStringLiteral("%1 trees").arg(trees.size()));
    if (trees.size() < 2)
        return;
    const QString target = dir.filePath(QStringLiteral("out"));
    QDir().mkpath(target);
    const QStringList now = btrfsrestore::parseRestored(run(btrfsrestore::restoreCommand(image, 0, target, true)), target);
    const QStringList before = btrfsrestore::parseRestored(run(btrfsrestore::restoreCommand(image, trees[1].at, target, true)), target);
    QStringList gone;
    for (const QString &path : before) {
        if (!now.contains(path))
            gone << path;
    }
    gone.sort();
    report(now.contains(QStringLiteral("/Notes/shopping.txt")) && !now.contains(QStringLiteral("/Notes/ferry.txt"))
               && gone == QStringList{QStringLiteral("/Notes/ferry.txt"), QStringLiteral("/Pictures/2024/beach2.jpg"), QStringLiteral("/Pictures/2024/beach4.jpg")},
           QStringLiteral("btrfs: the older tree lists the files deleted since"), gone.join(QLatin1Char(',')));
    run(btrfsrestore::restoreCommand(image, trees[1].at, target, false, {QStringLiteral("/Notes/ferry.txt"), QStringLiteral("/Pictures/2024/beach2.jpg")}));
    QFile ferry(target + QStringLiteral("/Notes/ferry.txt")), beach(target + QStringLiteral("/Pictures/2024/beach2.jpg"));
    const QByteArray note = ferry.open(QIODevice::ReadOnly) ? ferry.readAll() : QByteArray();
    const QByteArray photo = beach.open(QIODevice::ReadOnly) ? beach.readAll() : QByteArray();
    report(note == QByteArray("Pier 3 at 07:30.\n").repeated(50) && photo.size() == 22006 && photo.startsWith("\xFF\xD8") && photo.endsWith("\xFF\xD9")
               && !QFileInfo::exists(target + QStringLiteral("/Notes/shopping.txt")),
           QStringLiteral("btrfs: just the ticked deleted files are copied back, whole"), QStringLiteral("%1 %2").arg(note.size()).arg(photo.size()));
}

// Files Windows compressed, on a volume Windows made (tests/data/wof): compact.exe's four ways and
// NTFS's own compression, two of them deleted after. Each comes back whole, unpacked by our own
// code; and damaged packed data never takes it past what it was given.
void windowsCompressed()
{
    auto source = lost::Source::fromMemory(unpackImage(QStringLiteral(SOURCE_DIR "/tests/data/wof/wof.img.zst")));
    const QVector<lost::Found> found = quickScan(source);
    const QByteArray text = wofText(), mixed = wofMixed();
    using F = unpack::Format;
    struct Want {
        const char *path;
        F format;
        bool mixed;
        bool deleted;
    };
    const QVector<Want> wants = {
        {"Windows/System32/drivers/anvil.sys", F::Xpress4k, false, false}, {"Program Files/Tool/tool.exe", F::Xpress4k, false, false},
        {"Mixed/mixed4k.bin", F::Xpress4k, true, false},                   {"Windows/System32/lzx.dll", F::Lzx, false, false},
        {"Mixed/mixedlzx.bin", F::Lzx, true, false},                       {"Windows/System32/x8.dll", F::Xpress8k, false, false},
        {"Windows/System32/x16.dll", F::Xpress16k, false, false},          {"Docs/notes.txt", F::Lznt1, false, false},
        {"Docs/mixed.bin", F::Lznt1, true, false},                         {"Gone/gone.txt", F::Lznt1, false, true},
        {"Gone/gone-lzx.dll", F::Lzx, true, true},                         {"plain.txt", F::None, false, false},
    };
    QStringList wrong;
    QVector<const lost::Found *> samples; // one of each way, for the damage below
    for (const Want &w : wants) {
        const QString path = QStringLiteral("WOFTEST/") + QLatin1String(w.path);
        const lost::Found *match = nullptr;
        for (const lost::Found &f : found) {
            if (f.folder + QLatin1Char('/') + f.name == path)
                match = &f;
        }
        const QByteArray &expect = w.mixed ? mixed : text;
        if (!match) {
            wrong << path + QStringLiteral(" isn't listed");
            continue;
        }
        const QByteArray got = lost::FileReader(source, *match).head(qint64(match->size));
        if (match->packing.format != w.format || match->deleted != w.deleted || match->size != quint64(expect.size()) || got != expect
            || match->condition != lost::Condition::Good || (w.format != F::None) != match->note.contains(QLatin1String("compressed")))
            wrong << QStringLiteral("%1: format %2, %3 bytes, %4, condition %5, %6").arg(path).arg(int(match->packing.format)).arg(got.size())
                         .arg(got == expect ? QStringLiteral("the same") : QStringLiteral("not the same")).arg(int(match->condition)).arg(match->note);
        if (w.mixed)
            samples << match;
    }
    report(wrong.isEmpty(), QStringLiteral("compressed by Windows: each way comes back whole, deleted ones too (XPRESS 4K, 8K, 16K, LZX, LZNT1)"),
           wrong.join(QStringLiteral("; ")));

    // Reading from the middle, across chunks and units, the way a preview or a save does.
    bool middle = true;
    for (const lost::Found *f : std::as_const(samples)) {
        lost::FileReader reader(source, *f);
        for (const quint64 at : {quint64(1), quint64(4095), quint64(32767), quint64(65535), quint64(70001), quint64(150000)}) {
            QByteArray part(7000, Qt::Uninitialized);
            reader.read(at, part.data(), part.size());
            middle = middle && part == mixed.mid(qsizetype(at), part.size());
        }
    }
    report(samples.size() == 4 && middle, QStringLiteral("compressed by Windows: any part of one reads right on its own"));

    // Damaged: the packed bytes of each file, changed here and there, cut short, or random.
    QRandomGenerator random(1234);
    int rounds = 0;
    for (const lost::Found *f : std::as_const(samples)) {
        QByteArray stored;
        for (const lost::Extent &e : f->extents) {
            if (e.zeros())
                continue;
            QByteArray part(qsizetype(e.length), Qt::Uninitialized);
            source->read(e.start, part.data(), e.length);
            stored += part;
        }
        const qsizetype outSize = f->packing.format == F::Lznt1 ? f->packing.unit : unpack::chunkSize(f->packing.format);
        QByteArray out(outSize, Qt::Uninitialized);
        for (int round = 0; round < 1500; ++round, ++rounds) {
            const qsizetype from = random.bounded(int(stored.size()));
            QByteArray in = stored.mid(from, random.bounded(1, 40000));
            for (int flips = random.bounded(1, 6); flips > 0 && !in.isEmpty(); --flips)
                in[random.bounded(int(in.size()))] = char(random.bounded(256));
            if (round % 50 == 0) {
                in.resize(random.bounded(1, 3000));
                for (char &c : in)
                    c = char(random.bounded(256));
            }
            const auto *p = reinterpret_cast<const uchar *>(in.constData());
            auto *o = reinterpret_cast<uchar *>(out.data());
            switch (f->packing.format) {
            case F::Lznt1:
                unpack::lznt1(p, in.size(), o, out.size());
                break;
            case F::Lzx:
                unpack::lzx(p, in.size(), o, out.size());
                break;
            default:
                unpack::xpress(p, in.size(), o, out.size());
                break;
            }
        }
        // A stream that's only partly there: the file reads, with the missing part failing.
        lost::Found cut = *f;
        quint64 keep = 0;
        for (const lost::Extent &e : std::as_const(cut.extents))
            keep += e.length;
        cut.extents = lost::slice(cut.extents, 0, keep / 2);
        QByteArray all(qsizetype(cut.size), Qt::Uninitialized);
        lost::FileReader(source, cut).read(0, all.data(), all.size());
    }
    report(rounds == 6000, QStringLiteral("compressed by Windows: damaged packed data unpacks to no more than it should"), QString::number(rounds));
}

// A report of what was found: every file's details, its fingerprints, a place for a photo; as a
// web page and as CSV a spreadsheet can't be tricked by.
void foundReport()
{
    QVector<lost::Found> files(3);
    files[0].name = QStringLiteral("=cmd|' /C calc'!A0.txt"); // a "formula" a spreadsheet would run
    files[0].type = filetypes::typeForName(QStringLiteral("x.txt"));
    files[0].data = "hello";
    files[0].size = 5;
    files[0].note = QStringLiteral("<b>not bold</b>, \"quoted\"");
    files[1].name = QStringLiteral("leeds.jpg");
    files[1].type = filetypes::typeForName(files[1].name);
    files[1].data = jpegWith(exifTiff(true));
    files[1].size = quint64(files[1].data.size());
    files[1].deleted = true;
    files[1].deletedAt = QDateTime(QDate(2024, 7, 20), QTime(10, 0), QTimeZone::UTC).toMSecsSinceEpoch();
    files[2].name = QStringLiteral("gone.doc");
    files[2].nameOnly = true;
    const auto source = lost::Source::fromMemory(QByteArray(512, '\0'));
    const QVector<foundreport::Row> rows = foundreport::collect(source, files, {0, 1, 2}, true);
    report(rows.size() == 3 && rows[0].prints.done && rows[0].prints.sha256 == QCryptographicHash::hash("hello", QCryptographicHash::Sha256).toHex()
               && rows[0].prints.md5 == "5d41402abc4b2a76b9719d911017c592" && rows[1].spot.known && rows[1].place.startsWith(QLatin1String("London"))
               && !rows[2].prints.done,
           QStringLiteral("report: fingerprints of each file, where a photo was taken, nothing for a name alone"));
    foundreport::About about;
    about.title = QStringLiteral("Test stick");
    about.made = QDateTime(QDate(2026, 10, 12), QTime(12, 0));
    about.version = QStringLiteral("0.5.2");
    const QString html = foundreport::html(about, files, rows);
    const QByteArray csv = foundreport::csv(files, rows);
    report(html.contains(QLatin1String("&lt;b&gt;not bold&lt;/b&gt;")) && !html.contains(QLatin1String("<b>not bold")) && html.contains(QLatin1String("2cf24dba5fb0a30e"))
               && html.contains(QLatin1String("2024-07-20T")) && html.contains(QLatin1String("GeoNames")),
           QStringLiteral("report: the web page has every file, escaped"));
    report(csv.startsWith("\xEF\xBB\xBF") && csv.contains("\"'=cmd|' /C calc'!A0.txt\"") == false && csv.contains("'=cmd|")
               && csv.contains("\"<b>not bold</b>, \"\"quoted\"\"\"") && csv.contains(",-0.127500,") && csv.count('\n') == 4,
           QStringLiteral("report: CSV, quoted, and a cell starting with = can't run as a formula"), QString::fromUtf8(csv.left(400)));
    int stopsAt = 0;
    foundreport::collect(source, files, {0, 1, 2}, true, [&stopsAt](int done, int) { stopsAt = done; return done < 1; });
    report(stopsAt == 1, QStringLiteral("report: making one can be stopped"));
}

// The viewer's tools: blur taken back out gets closer to the sharp picture; the rest do what
// they say.
void enhanceTools()
{
    // A sharp picture with fine detail (stripes and squares), then blurred the two ways.
    QImage sharp(160, 120, QImage::Format_ARGB32);
    for (int y = 0; y < sharp.height(); ++y) {
        for (int x = 0; x < sharp.width(); ++x) {
            const bool on = ((x / 6) + (y / 6)) % 2 == 0 || (x % 17 == 3);
            sharp.setPixel(x, y, on ? qRgb(230, 220, 210) : qRgb(30, 40, 50));
        }
    }
    const auto difference = [](const QImage &a, const QImage &b) {
        double sum = 0;
        for (int y = 8; y < a.height() - 8; ++y) {
            for (int x = 8; x < a.width() - 8; ++x)
                sum += std::abs(qGray(a.pixel(x, y)) - qGray(b.pixel(x, y)));
        }
        return sum / ((a.width() - 16) * (a.height() - 16));
    };
    // Out of focus: a Gaussian blur (sigma 2), the kind the tool takes out.
    QImage soft(sharp.size(), QImage::Format_ARGB32);
    {
        double k[13], sum = 0;
        for (int i = -6; i <= 6; ++i)
            sum += k[i + 6] = std::exp(-(i * i) / 8.0);
        const auto at = [&sharp](int x, int y) { return qGray(sharp.pixel(std::clamp(x, 0, sharp.width() - 1), std::clamp(y, 0, sharp.height() - 1))); };
        for (int y = 0; y < sharp.height(); ++y) {
            for (int x = 0; x < sharp.width(); ++x) {
                double v = 0;
                for (int j = -6; j <= 6; ++j) {
                    for (int i = -6; i <= 6; ++i)
                        v += k[i + 6] * k[j + 6] * at(x + i, y + j);
                }
                const int g = qRound(v / (sum * sum));
                soft.setPixel(x, y, qRgb(g, g, g));
            }
        }
    }
    enhance::Settings focus;
    focus.focus = 4;
    focus.rounds = 20;
    const QImage refocused = enhance::apply(soft, focus);
    QImage greyStill = sharp;
    for (int y = 0; y < greyStill.height(); ++y) {
        for (int x = 0; x < greyStill.width(); ++x) {
            const int g = qGray(sharp.pixel(x, y));
            greyStill.setPixel(x, y, qRgb(g, g, g));
        }
    }
    const double before = difference(soft, greyStill), after = difference(refocused, greyStill);
    report(!refocused.isNull() && after < before * 0.85, QStringLiteral("enhance: out-of-focus blur taken out, closer to the sharp picture"),
           QStringLiteral("off by %1 before, %2 after").arg(before, 0, 'f', 1).arg(after, 0, 'f', 1));
    // A lens out of focus: each point a disc. Taken out with Wiener (one step) and with
    // Richardson-Lucy told it's a disc, both nearer the sharp picture.
    QImage disc(sharp.size(), QImage::Format_ARGB32);
    {
        const int r = 4;
        const auto at = [&greyStill](int x, int y) { return qGray(greyStill.pixel(std::clamp(x, 0, greyStill.width() - 1), std::clamp(y, 0, greyStill.height() - 1))); };
        for (int y = 0; y < sharp.height(); ++y)
            for (int x = 0; x < sharp.width(); ++x) {
                double v = 0;
                int n = 0;
                for (int j2 = -r; j2 <= r; ++j2)
                    for (int i2 = -r; i2 <= r; ++i2)
                        if (i2 * i2 + j2 * j2 <= r * r) {
                            v += at(x + i2, y + j2);
                            ++n;
                        }
                const int g = qRound(v / n);
                disc.setPixel(x, y, qRgb(g, g, g));
            }
    }
    enhance::Settings wiener;
    wiener.focus = 4;
    wiener.focusDisc = true;
    wiener.wiener = true;
    wiener.noise = 20;
    enhance::Settings rlDisc = wiener;
    rlDisc.wiener = false;
    rlDisc.rounds = 25;
    const double discBefore = difference(disc, greyStill), wienerAfter = difference(enhance::apply(disc, wiener), greyStill),
                 rlAfter = difference(enhance::apply(disc, rlDisc), greyStill);
    report(wienerAfter < discBefore * 0.85 && rlAfter < discBefore * 0.9, QStringLiteral("enhance: a lens's out-of-focus disc taken out, by Wiener and by Richardson-Lucy"),
           QStringLiteral("off by %1 blurred, %2 Wiener, %3 Richardson-Lucy").arg(discBefore, 0, 'f', 1).arg(wienerAfter, 0, 'f', 1).arg(rlAfter, 0, 'f', 1));
    // Non-local means: grain taken out, the edges kept.
    {
        QImage noisy(96, 64, QImage::Format_RGB32);
        QRandomGenerator grain(9);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 96; ++x) {
                const int v = std::clamp((x < 48 ? 70 : 180) + int(grain.bounded(41)) - 20, 0, 255);
                noisy.setPixel(x, y, qRgb(v, v, v));
            }
        enhance::Settings nlm;
        nlm.nlm = 60;
        const QImage clean = enhance::apply(noisy, nlm);
        const auto spread = [](const QImage &im) {
            double sum = 0, sq = 0;
            int n = 0;
            for (int y = 8; y < 56; ++y)
                for (int x = 6; x < 40; ++x, ++n) {
                    const int v = qGray(im.pixel(x, y));
                    sum += v;
                    sq += v * v;
                }
            return std::sqrt(sq / n - (sum / n) * (sum / n));
        };
        const int edge = qGray(clean.pixel(50, 30)) - qGray(clean.pixel(45, 30));
        report(spread(clean) < spread(noisy) / 2.5 && edge > 90, QStringLiteral("enhance: non-local means takes the grain out, the edge stays"),
               QStringLiteral("grain %1 to %2, edge %3").arg(spread(noisy), 0, 'f', 1).arg(spread(clean), 0, 'f', 1).arg(edge));
    }
    // Moved sideways while it was taken: each pixel the average of 9 in a row.
    QImage moved(sharp.size(), QImage::Format_ARGB32);
    for (int y = 0; y < sharp.height(); ++y) {
        for (int x = 0; x < sharp.width(); ++x) {
            int sum = 0;
            for (int i = -4; i <= 4; ++i)
                sum += qGray(sharp.pixel(std::clamp(x + i, 0, sharp.width() - 1), y));
            moved.setPixel(x, y, qRgb(sum / 9, sum / 9, sum / 9));
        }
    }
    enhance::Settings motion;
    motion.motion = 9;
    motion.motionAngle = 0;
    motion.rounds = 25;
    const QImage steadied = enhance::apply(moved, motion);
    QImage greySharp = sharp;
    for (int y = 0; y < greySharp.height(); ++y) {
        for (int x = 0; x < greySharp.width(); ++x) {
            const int g = qGray(sharp.pixel(x, y));
            greySharp.setPixel(x, y, qRgb(g, g, g));
        }
    }
    const double movedOff = difference(moved, greySharp), steadiedOff = difference(steadied, greySharp);
    report(!steadied.isNull() && steadiedOff < movedOff * 0.8, QStringLiteral("enhance: movement blur taken out, closer to the still picture"),
           QStringLiteral("off by %1 before, %2 after").arg(movedOff, 0, 'f', 1).arg(steadiedOff, 0, 'f', 1));
    // The simple ones.
    enhance::Settings light;
    light.brightness = 50;
    enhance::Settings flip;
    flip.invert = true;
    flip.grey = true;
    QImage dark(4, 4, QImage::Format_ARGB32);
    dark.fill(qRgb(40, 40, 40));
    const QImage brighter = enhance::apply(dark, light), flipped = enhance::apply(dark, flip);
    report(qGray(brighter.pixel(1, 1)) > 80 && qGray(flipped.pixel(1, 1)) == 215 && enhance::apply(dark, enhance::Settings()) == dark,
           QStringLiteral("enhance: brighter, inverted, and nothing done when nothing's asked"));
    const QImage big = enhance::enlarge(sharp, 3);
    report(big.size() == sharp.size() * 3 && qGray(big.pixel(1, 1)) == qGray(sharp.pixel(0, 0)) && enhance::enlarge(sharp, 1) == sharp,
           QStringLiteral("enhance: enlarged three times over, the same picture bigger"));
    int stops = 0;
    const QImage stopped = enhance::apply(soft, focus, [&stops](int) { return ++stops < 3; });
    report(stopped.isNull(), QStringLiteral("enhance: it can be stopped part way"));
    report(focus.describe().join(QStringLiteral("; ")).contains(QLatin1String("4 px")), QStringLiteral("enhance: what was done, in words"),
           focus.describe().join(QStringLiteral("; ")));

    // The tools from the forensic video world: local contrast, smoothing that keeps edges,
    // deinterlacing, straightening, and frames stacked.
    {
        // A dark picture with faint detail in it: local contrast brings it out more than it was.
        QImage dim(128, 128, QImage::Format_RGB32);
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x)
                dim.setPixel(x, y, x < 64 ? qRgb(10 + y / 3 + (x / 4 % 2) * 6, 10 + y / 3 + (x / 4 % 2) * 6, 10 + y / 3 + (x / 4 % 2) * 6) : qRgb(220, 220, 220));
        enhance::Settings local;
        local.localContrast = 60;
        const QImage spread = enhance::apply(dim, local);
        const int before = qGray(dim.pixel(5, 60)) - qGray(dim.pixel(1, 60)), after = qGray(spread.pixel(5, 60)) - qGray(spread.pixel(1, 60));
        report(after > before * 2, QStringLiteral("enhance: local contrast brings out faint detail in the dark part"),
               QStringLiteral("a step of %1 became %2").arg(before).arg(after));
    }
    {
        // Grain on two flat halves: smoothed, while the edge between them stays a sharp step.
        QImage grainy(96, 64, QImage::Format_RGB32);
        QRandomGenerator noise(7);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 96; ++x) {
                const int v = std::clamp((x < 48 ? 60 : 190) + int(noise.bounded(31)) - 15, 0, 255);
                grainy.setPixel(x, y, qRgb(v, v, v));
            }
        enhance::Settings keep;
        keep.smooth = 70;
        const QImage smooth = enhance::apply(grainy, keep);
        const auto spread = [](const QImage &im, int x0) {
            double sum = 0, sq = 0;
            int n = 0;
            for (int y = 8; y < 56; ++y)
                for (int x = x0; x < x0 + 30; ++x, ++n) {
                    const int v = qGray(im.pixel(x, y));
                    sum += v;
                    sq += v * v;
                }
            return std::sqrt(sq / n - (sum / n) * (sum / n));
        };
        const int edge = qGray(smooth.pixel(49, 32)) - qGray(smooth.pixel(46, 32));
        report(spread(smooth, 5) < spread(grainy, 5) / 2 && edge > 100, QStringLiteral("enhance: grain smoothed, the edge kept sharp"),
               QStringLiteral("grain %1 to %2, edge step %3").arg(spread(grainy, 5), 0, 'f', 1).arg(spread(smooth, 5), 0, 'f', 1).arg(edge));
    }
    {
        // Comb lines: the odd lines from a moment later, made again from the even ones.
        QImage comb(32, 32, QImage::Format_RGB32);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
                comb.setPixel(x, y, (y % 2 ? x + 4 : x) < 16 ? qRgb(0, 0, 0) : qRgb(255, 255, 255));
        enhance::Settings both;
        both.deinterlace = true;
        const QImage one = enhance::apply(comb, both);
        bool straight = true;
        for (int y = 1; y < 31; ++y)
            straight = straight && qGray(one.pixel(17, y)) == 255 && qGray(one.pixel(14, y)) == 0;
        report(straight, QStringLiteral("enhance: deinterlacing takes the comb lines out"));
    }
    {
        // A plate seen at an angle: its four corners picked, it comes back square-on.
        QImage plate(200, 60, QImage::Format_RGB32);
        plate.fill(Qt::white);
        {
            QPainter p(&plate);
            p.fillRect(20, 15, 30, 30, Qt::black);
            p.fillRect(150, 15, 30, 30, Qt::black);
        }
        const QPolygonF corners{QPointF(40, 30), QPointF(250, 60), QPointF(240, 150), QPointF(30, 110)};
        QTransform to;
        QTransform::quadToQuad(QPolygonF{QPointF(0, 0), QPointF(200, 0), QPointF(200, 60), QPointF(0, 60)}, corners, to);
        QImage seen(300, 200, QImage::Format_RGB32);
        seen.fill(Qt::gray);
        {
            QPainter p(&seen);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.setTransform(to);
            p.drawImage(0, 0, plate);
        }
        const QImage flat = enhance::straighten(seen, corners);
        const bool square = flat.width() > 190 && flat.height() > 60 && qGray(flat.pixel(flat.width() * 35 / 200, flat.height() / 2)) < 80
                         && qGray(flat.pixel(flat.width() * 100 / 200, flat.height() / 2)) > 180
                         && qGray(flat.pixel(flat.width() * 165 / 200, flat.height() / 2)) < 80;
        report(square, QStringLiteral("enhance: a plate seen at an angle is straightened from its four corners"),
               QStringLiteral("%1x%2").arg(flat.width()).arg(flat.height()));
    }
    {
        // Frames of the same thing, each moved a little and noisy: stacked they line up, the
        // noise evens out, and at twice the size they hold more than one frame made bigger.
        QImage truth(128, 96, QImage::Format_RGB32);
        QRandomGenerator dots(3);
        for (int y = 0; y < 96; ++y)
            for (int x = 0; x < 128; ++x) {
                truth.setPixel(x, y, qRgb(120, 120, 120));
            }
        // Blocks of all sizes, nowhere the same: nothing that lines up wrongly by repeating.
        for (int i = 0; i < 60; ++i) {
            const int bx = int(dots.bounded(120)), by = int(dots.bounded(88)), bw = 2 + int(dots.bounded(14)), bh = 2 + int(dots.bounded(14));
            const int v = 40 + int(dots.bounded(200));
            for (int y = by; y < std::min(96, by + bh); ++y)
                for (int x = bx; x < std::min(128, bx + bw); ++x)
                    truth.setPixel(x, y, qRgb(v, v, v));
        }
        QVector<QImage> frames;
        QVector<QPointF> moves;
        QImage noiseless(64, 48, QImage::Format_RGB32); // the first frame as it would be with no grain
        QRandomGenerator jitter(11);
        for (int i = 0; i < 12; ++i) {
            const QPointF move(jitter.bounded(6.0) - 3, jitter.bounded(6.0) - 3);
            moves << move;
            // Filmed at half the detail, moved, with grain.
            // Smoothly between the scene's points (as a lens does), so a move of any fraction is just that.
            const auto scene = [&truth](double tx, double ty) {
                const int ix = int(std::floor(tx)), iy = int(std::floor(ty));
                const double fx = tx - ix, fy = ty - iy;
                const auto at = [&truth](int x, int y) { return qGray(truth.pixel(std::clamp(x, 0, 127), std::clamp(y, 0, 95))); };
                return (at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx) * (1 - fy) + (at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx) * fy;
            };
            QImage frame(64, 48, QImage::Format_RGB32);
            for (int y = 0; y < 48; ++y)
                for (int x = 0; x < 64; ++x) {
                    double sum = 0;
                    for (int j = 0; j < 2; ++j)
                        for (int k = 0; k < 2; ++k)
                            sum += scene(x * 2 + k + move.x() * 2, y * 2 + j + move.y() * 2);
                    const int clean = std::clamp(int(sum / 4), 0, 255);
                    const int v = std::clamp(clean + int(jitter.bounded(41)) - 20, 0, 255);
                    frame.setPixel(x, y, qRgb(v, v, v));
                    if (i == 0)
                        noiseless.setPixel(x, y, qRgb(clean, clean, clean));
                }
            frames << frame;
        }
        QVector<QPointF> found;
        const QImage stacked = enhance::stack(frames, 0, 2, enhance::Combine::Average, &found);
        const QImage same = enhance::stack(frames, 0, 1, enhance::Combine::Average);
        const QImage &clean = noiseless;
        const auto off = [&clean](const QImage &im) {
            double sum = 0;
            int n = 0;
            for (int y = 8; y < 40; ++y)
                for (int x = 8; x < 56; ++x, ++n)
                    sum += std::abs(qGray(im.pixel(x, y)) - qGray(clean.pixel(x, y)));
            return sum / n;
        };
        bool lined = found.size() == 12;
        for (int i = 1; i < found.size() && lined; ++i)
            lined = std::abs(found[i].x() - (moves[0].x() - moves[i].x())) < 0.4 && std::abs(found[i].y() - (moves[0].y() - moves[i].y())) < 0.4;
        QStringList pairs;
        for (int i = 1; i < std::min(4, int(found.size())); ++i)
            pairs << QStringLiteral("%1,%2 for %3,%4").arg(found[i].x(), 0, 'f', 2).arg(found[i].y(), 0, 'f', 2)
                         .arg(moves[0].x() - moves[i].x(), 0, 'f', 2).arg(moves[0].y() - moves[i].y(), 0, 'f', 2);
        report(lined, QStringLiteral("enhance: stacked frames are lined up to a fraction of a pixel"), pairs.join(QStringLiteral("; ")));
        report(stacked.size() == QSize(128, 96) && off(same) < off(frames[0]) * 0.5, QStringLiteral("enhance: stacked, the grain evens out: far nearer the frame without it than one frame is"),
               QStringLiteral("grain of %1 in one frame, %2 stacked").arg(off(frames[0]), 0, 'f', 1).arg(off(same), 0, 'f', 1));
    }

    {
        // JPEG's squares: flat 8 by 8 squares a little apart are evened out; a real edge stays.
        QImage squares(64, 16, QImage::Format_RGB32);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 64; ++x) {
                const int v = x < 32 ? 100 + (x / 8) * 6 : 230; // squares 6 apart, then a real edge at 32
                squares.setPixel(x, y, qRgb(v, v, v));
            }
        enhance::Settings blocks;
        blocks.deblock = 80;
        const QImage even = enhance::apply(squares, blocks);
        const int stepBefore = qGray(squares.pixel(8, 4)) - qGray(squares.pixel(7, 4)), stepAfter = qGray(even.pixel(8, 4)) - qGray(even.pixel(7, 4));
        const int edgeAfter = qGray(even.pixel(32, 4)) - qGray(even.pixel(31, 4));
        report(stepAfter < stepBefore && edgeAfter > 90, QStringLiteral("enhance: JPEG's squares are evened out, a real edge is kept"),
               QStringLiteral("square step %1 to %2, edge %3").arg(stepBefore).arg(stepAfter).arg(edgeAfter));
        // Made bigger to any size smoothly: no squares, and the same picture.
        QImage tiny(4, 4, QImage::Format_RGB32);
        tiny.fill(qRgb(10, 10, 10));
        tiny.setPixel(1, 1, qRgb(250, 250, 250));
        const QImage big = enhance::resize(tiny, QSize(40, 40));
        report(big.size() == QSize(40, 40) && qGray(big.pixel(15, 15)) > 200 && qGray(big.pixel(20, 15)) > qGray(big.pixel(30, 15))
                   && qGray(big.pixel(20, 15)) < qGray(big.pixel(15, 15)),
               QStringLiteral("enhance: a tiny picture made bigger to any size, smoothly (no squares)"));
        report(enhance::enlarge(tiny, 8).size() == QSize(32, 32), QStringLiteral("enhance: saved up to eight times as big"));
    }

    {
        // Played right: video's 16-235 to 0-255; one field, lines doubled.
        QImage video(16, 8, QImage::Format_RGB32);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 16; ++x)
                video.setPixel(x, y, x < 8 ? qRgb(16, 16, 16) : qRgb(235, 235, 235));
        enhance::Settings range;
        range.fullRange = true;
        const QImage full = enhance::apply(video, range);
        report(qGray(full.pixel(2, 2)) <= 1 && qGray(full.pixel(12, 2)) >= 254, QStringLiteral("enhance: video levels 16-235 stretched to the full range"));
        QImage interlaced(8, 8, QImage::Format_RGB32);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                interlaced.setPixel(x, y, y % 2 ? qRgb(0, 0, 0) : qRgb(200, 200, 200));
        enhance::Settings first;
        first.field = 1;
        const QImage one = enhance::apply(interlaced, first);
        bool doubled = true;
        for (int y = 0; y < 8; ++y)
            doubled = doubled && qGray(one.pixel(3, y)) == 200;
        report(doubled, QStringLiteral("enhance: one field on its own, its lines doubled"));
    }
    {
        // Uneven light: a dark half with faint detail and a bright half; brought up, detail kept.
        QImage lit(128, 64, QImage::Format_RGB32);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 128; ++x) {
                const int base = x < 64 ? 18 : 220, detail = (x / 4 % 2) * (x < 64 ? 6 : 20);
                lit.setPixel(x, y, qRgb(base + detail, base + detail, base + detail));
            }
        for (const auto &[name, setting] : {std::pair{QStringLiteral("uneven light (homomorphic)"), 0}, std::pair{QStringLiteral("Retinex"), 1}}) {
            enhance::Settings s;
            (setting == 0 ? s.homomorphic : s.retinex) = 80;
            const QImage out = enhance::apply(lit, s);
            const int darkBefore = qGray(lit.pixel(20, 30)), darkAfter = qGray(out.pixel(20, 30));
            const int stepBefore = qGray(lit.pixel(4, 30)) - qGray(lit.pixel(0, 30)), stepAfter = qGray(out.pixel(4, 30)) - qGray(out.pixel(0, 30));
            report(darkAfter > darkBefore + 10 && stepAfter > stepBefore, QStringLiteral("enhance: %1 brings the dark half up, its detail clearer").arg(name),
                   QStringLiteral("dark %1 to %2, detail step %3 to %4").arg(darkBefore).arg(darkAfter).arg(stepBefore).arg(stepAfter));
        }
    }
    {
        // Channels: red text on a grey that's the same brightness shows only in some.
        QImage sign(32, 16, QImage::Format_RGB32);
        sign.fill(qRgb(120, 120, 120));
        for (int x = 8; x < 24; ++x)
            for (int y = 5; y < 11; ++y)
                sign.setPixel(x, y, qRgb(200, 85, 85)); // about as bright as the grey
        enhance::Settings red;
        red.channel = enhance::Settings::Red;
        enhance::Settings most;
        most.channel = enhance::Settings::MostContrast;
        enhance::Settings bright;
        bright.channel = enhance::Settings::Brightness;
        const QImage r = enhance::apply(sign, red), m = enhance::apply(sign, most), y = enhance::apply(sign, bright);
        const int redGap = std::abs(qGray(r.pixel(15, 7)) - qGray(r.pixel(2, 2))), mostGap = std::abs(qGray(m.pixel(15, 7)) - qGray(m.pixel(2, 2)));
        report(redGap > 200 && mostGap > 200 && qRed(r.pixel(2, 2)) == qGreen(r.pixel(2, 2)),
               QStringLiteral("enhance: one channel (red, or the most contrast) shows what the colour hides"), QStringLiteral("red %1, most %2").arg(redGap).arg(mostGap));
        report(enhance::Settings().isDefault() && !red.isDefault() && red.describe().join(QLatin1Char(' ')).contains(QLatin1String("red")),
               QStringLiteral("enhance: the channel is said in what's saved"));
    }
    {
        // Robust stacking: one frame with a passer-by across it is left out by the middle value.
        QImage scene(48, 32, QImage::Format_RGB32);
        QRandomGenerator dots(5);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 48; ++x) {
                const int v = 60 + int(dots.bounded(140));
                scene.setPixel(x, y, qRgb(v, v, v));
            }
        QVector<QImage> frames(7, scene);
        QImage passer = scene;
        for (int y = 10; y < 22; ++y)
            for (int x = 20; x < 28; ++x)
                passer.setPixel(x, y, qRgb(255, 0, 0));
        frames[3] = passer;
        const QImage mean = enhance::stack(frames, 0, 1, enhance::Combine::Average);
        const QImage middle = enhance::stack(frames, 0, 1, enhance::Combine::Middle);
        const QImage clipped = enhance::stack(frames, 0, 1, enhance::Combine::AverageWithoutOutliers);
        const auto redness = [](const QImage &im) { return qRed(im.pixel(24, 16)) - qGreen(im.pixel(24, 16)); };
        report(redness(mean) > 20 && std::abs(redness(middle)) <= 2 && std::abs(redness(clipped)) <= 4,
               QStringLiteral("enhance: a passer-by in one frame smears the average, the middle value and the average without outliers leave it out"),
               QStringLiteral("red over green: average %1, middle %2, without outliers %3").arg(redness(mean)).arg(redness(middle)).arg(redness(clipped)));
        QImage bright(4, 1, QImage::Format_RGB32);
        bright.setPixel(0, 0, qRgb(255, 255, 255));
        bright.setPixel(1, 0, qRgb(0, 0, 0));
        bright.setPixel(2, 0, qRgb(128, 128, 128));
        bright.setPixel(3, 0, qRgb(255, 40, 40));
        const QImage over = enhance::clippedOverlay(bright);
        report(qAlpha(over.pixel(0, 0)) > 0 && qAlpha(over.pixel(1, 0)) > 0 && qAlpha(over.pixel(2, 0)) == 0 && qAlpha(over.pixel(3, 0)) > 0,
               QStringLiteral("enhance: what's blown out or pure black is marked, the rest isn't"));
    }

    {
        // A wide lens bends a straight line; three points on it give the strength back.
        const QSize pic(800, 600);
        const int truth = 55;
        const auto bend = [&](QPointF u) { // the bent point whose unbending is u
            QPointF d = u;
            for (int it = 0; it < 30; ++it) {
                const QPointF back = enhance::unbend(d, pic, truth);
                d += u - back;
            }
            return d;
        };
        QVector<QPointF> line;
        for (int i = 0; i <= 8; ++i)
            line << bend(QPointF(60 + i * 85, 80 + i * 10));
        const int found = enhance::fisheyeFromLine(line, pic);
        report(std::abs(found - truth) <= 3, QStringLiteral("enhance: a bent straight line gives the lens's bend back"), QStringLiteral("%1 for %2").arg(found).arg(truth));
        enhance::Settings unfish;
        unfish.fisheye = 40;
        QImage grid(80, 60, QImage::Format_RGB32);
        grid.fill(Qt::white);
        report(enhance::apply(grid, unfish).size() == grid.size() && unfish.describe().join(QLatin1Char(' ')).contains(QLatin1String("lens")),
               QStringLiteral("enhance: the bend taken out of a picture, and said"));
    }
    {
        // Frames: a sharp one scores above a blurred one; corners followed through moving frames.
        QImage sharp(160, 120, QImage::Format_RGB32);
        QRandomGenerator blocks(21);
        sharp.fill(qRgb(90, 90, 90));
        {
            QPainter p(&sharp);
            for (int i = 0; i < 40; ++i)
                p.fillRect(int(blocks.bounded(150)), int(blocks.bounded(110)), 4 + int(blocks.bounded(16)), 4 + int(blocks.bounded(16)),
                           QColor::fromHsv(int(blocks.bounded(360)), 200, 60 + int(blocks.bounded(190))));
        }
        enhance::Settings blur;
        blur.smooth = 100;
        const QImage soft = enhance::apply(sharp, blur);
        report(enhance::sharpness(sharp) > enhance::sharpness(soft) * 1.5, QStringLiteral("enhance: a sharp frame scores above a blurred one"));
        QVector<QImage> frames;
        QVector<QPointF> moves = {QPointF(0, 0), QPointF(1.4, 0.6), QPointF(2.7, 1.1), QPointF(3.9, 2.3), QPointF(5.2, 2.8)};
        for (const QPointF &m : moves) {
            // Moved by exactly m (between pixels, smoothly): what the frame shows at x is the scene at x - m.
            QImage f(160, 120, QImage::Format_RGB32);
            for (int y = 0; y < 120; ++y)
                for (int x = 0; x < 160; ++x) {
                    const double sx = x - m.x(), sy = y - m.y();
                    const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                    const double tx = sx - ix, ty = sy - iy;
                    const auto at = [&sharp](int px, int py, int ch) {
                        const QRgb c = sharp.pixel(std::clamp(px, 0, 159), std::clamp(py, 0, 119));
                        return double(ch == 0 ? qRed(c) : ch == 1 ? qGreen(c) : qBlue(c));
                    };
                    int rgb[3];
                    for (int ch = 0; ch < 3; ++ch)
                        rgb[ch] = int(std::lround((at(ix, iy, ch) * (1 - tx) + at(ix + 1, iy, ch) * tx) * (1 - ty) + (at(ix, iy + 1, ch) * (1 - tx) + at(ix + 1, iy + 1, ch) * tx) * ty));
                    f.setPixel(x, y, qRgb(rgb[0], rgb[1], rgb[2]));
                }
            frames << f;
        }
        const QPolygonF corners{QPointF(40, 30), QPointF(120, 32), QPointF(118, 90), QPointF(42, 88)};
        const QVector<QPolygonF> followed = enhance::followCorners(frames, 0, corners);
        double worst = 0;
        for (int i = 0; i < frames.size(); ++i)
            for (int k = 0; k < 4; ++k)
                worst = std::max(worst, std::hypot(followed[i][k].x() - (corners[k].x() + moves[i].x()), followed[i][k].y() - (corners[k].y() + moves[i].y())));
        report(worst < 0.5, QStringLiteral("enhance: a flat thing's corners followed from frame to frame"), QStringLiteral("off by %1 px at worst").arg(worst, 0, 'f', 2));
        report(enhance::straighten(frames[2], followed[2], QSize(80, 58)).size() == QSize(80, 58), QStringLiteral("enhance: each frame straightened to the same size"));
    }
    if (jpegexact::available()) {
        // A picture squeezed hard into a JPEG: decoded again within what the file stores, nearer
        // the original than an ordinary decode, and its squares' edges weaker.
        QImage scene(128, 96, QImage::Format_RGB32);
        for (int y = 0; y < 96; ++y)
            for (int x = 0; x < 128; ++x) {
                const double v = 128 + 70 * std::sin(x / 9.0) * std::cos(y / 13.0) + (x > 64 ? 30 : -30);
                scene.setPixel(x, y, qRgb(std::clamp(int(v), 0, 255), std::clamp(int(v * 0.8), 0, 255), std::clamp(int(255 - v), 0, 255)));
            }
        QByteArray squeezed;
        QBuffer buffer(&squeezed);
        buffer.open(QIODevice::WriteOnly);
        scene.save(&buffer, "JPEG", 8);
        QImage ordinary;
        ordinary.loadFromData(squeezed, "JPEG");
        QString why;
        const QImage again = jpegexact::decode(squeezed, 30, &why);
        const auto off = [&scene](const QImage &im) {
            double sum = 0;
            for (int y = 0; y < 96; ++y)
                for (int x = 0; x < 128; ++x)
                    sum += std::abs(qGray(im.pixel(x, y)) - qGray(scene.pixel(x, y)));
            return sum / (128 * 96);
        };
        const auto edges = [](const QImage &im) { // steps across the squares' edges, against steps inside them
            double across = 0, inside = 0;
            for (int y = 0; y < 96; ++y)
                for (int x = 1; x < 128; ++x) {
                    const double d = std::abs(qGray(im.pixel(x, y)) - qGray(im.pixel(x - 1, y)));
                    (x % 8 == 0 ? across : inside) += d;
                }
            return (across / (96 * 15)) / std::max(0.01, inside / (96 * (127 - 15)));
        };
        report(!again.isNull() && again.size() == scene.size() && off(again) < off(ordinary) && edges(again) < edges(ordinary) * 0.8,
               QStringLiteral("jpeg decoded again within what it stores: nearer the original, its squares fainter"),
               why + QStringLiteral(" off by %1 ordinary, %2 again; edge ratio %3 to %4").arg(off(ordinary), 0, 'f', 2).arg(off(again), 0, 'f', 2)
                         .arg(edges(ordinary), 0, 'f', 2).arg(edges(again), 0, 'f', 2));
        report(jpegexact::decode("not a jpeg", 10, &why).isNull() && !why.isEmpty(), QStringLiteral("jpeg decoded again: what isn't one is refused, saying why"));
    }

    // Profiles: DiskAnvil's own, and files people save and pass on.
    const QVector<enhance::Profile> own = enhance::builtInProfiles();
    bool ownFine = own.size() >= 6;
    for (const enhance::Profile &p : own) {
        enhance::Profile back;
        QString why;
        ownFine = ownFine && p.builtIn && !p.settings.isDefault() && !p.description.isEmpty() && enhance::profileFromFile(enhance::profileToFile(p), &back, &why)
               && back.name == p.name && back.settings.describe() == p.settings.describe();
    }
    report(ownFine, QStringLiteral("enhance profiles: DiskAnvil's own each do something, and come back the same from their files"));
    QTemporaryDir profiles;
    enhance::Profile mine;
    mine.name = QStringLiteral("Night: car park / gate 2");
    mine.settings.shadows = 70;
    mine.settings.motion = 12;
    mine.settings.motionAngle = 45;
    QString why;
    const QString file = enhance::saveProfile(mine, &why, profiles.path());
    report(!file.isEmpty() && QFileInfo(file).fileName() == QLatin1String("Night_ car park _ gate 2.anvil-enhance")
               && enhance::savedProfiles(profiles.path()).size() == 1 && enhance::savedProfiles(profiles.path()).first().settings.motionAngle == 45,
           QStringLiteral("enhance profiles: saved under a name that's safe for any drive, and read back"), QFileInfo(file).fileName() + why);
    mine.settings.shadows = 10;
    enhance::saveProfile(mine, &why, profiles.path());
    report(enhance::savedProfiles(profiles.path()).size() == 1 && enhance::savedProfiles(profiles.path()).first().settings.shadows == 10,
           QStringLiteral("enhance profiles: saving one of the same name replaces it"));
    // What comes from someone else is checked: only the settings, each kept to its range.
    enhance::Profile wild;
    const QByteArray odd = R"({"diskanvil-enhance-profile": 1, "name": "  Too\nmuch\u0007  ", "description": "x",
        "settings": {"brightness": 9999, "sharpen": -50, "focus": 99, "denoise": 7, "grey": true, "run": "rm -rf /"}})";
    report(enhance::profileFromFile(odd, &wild, &why) && wild.name == QLatin1String("Too much") && wild.settings.brightness == 100 && wild.settings.sharpen == 0
               && wild.settings.focus == 15 && wild.settings.denoise == 2 && wild.settings.grey,
           QStringLiteral("enhance profiles: one from elsewhere is kept to what the tools can do"), wild.name);
    const bool refused = !enhance::profileFromFile("{\"name\": \"x\"}", &wild, &why) && !enhance::profileFromFile("garbage", &wild, &why)
                      && !enhance::profileFromFile(R"({"diskanvil-enhance-profile": 2, "name": "x"})", &wild, &why) && why.contains(QLatin1String("newer"))
                      && !enhance::profileFromFile(R"({"diskanvil-enhance-profile": 1, "name": ""})", &wild, &why)
                      && !enhance::profileFromFile(QByteArray(70000, ' '), &wild, &why);
    report(refused, QStringLiteral("enhance profiles: what isn't one (or is from a newer DiskAnvil, or too big) is refused, saying why"));
    QFile junk(profiles.filePath(QStringLiteral("junk.anvil-enhance")));
    if (junk.open(QIODevice::WriteOnly))
        junk.write("not json");
    junk.close();
    report(enhance::savedProfiles(profiles.path()).size() == 1 && enhance::removeProfile(QStringLiteral("night: CAR park / gate 2"), profiles.path())
               && enhance::savedProfiles(profiles.path()).isEmpty() && junk.exists(),
           QStringLiteral("enhance profiles: a broken file in the folder is passed over, and one is removed by its name"));
}

// Where a photo was taken, in words and on a map, from what DiskAnvil carries (no internet).
void placeNames()
{
    const places::Place leeds = places::nearest(53.79648, -1.54785);
    report(leeds.name == QLatin1String("Leeds") && leeds.country == QLatin1String("United Kingdom") && leeds.region == QLatin1String("England")
               && leeds.kilometres < 2 && places::describe(leeds) == QLatin1String("Leeds, England, United Kingdom"),
           QStringLiteral("places: a spot in a city is named after it"), places::describe(leeds));
    const places::Place outside = places::nearest(-33.95, 151.35); // off Sydney's coast
    report(outside.valid() && places::describe(outside).startsWith(QLatin1String("near ")) && outside.country == QLatin1String("Australia"),
           QStringLiteral("places: a spot out of town is \"near\" the nearest one, with how far"), places::describe(outside));
    report(!places::nearest(-48.88, -123.39).valid() && !places::nearest(91, 0).valid() && !places::nearest(qQNaN(), 0).valid(),
           QStringLiteral("places: the middle of the ocean, or no place at all, isn't named"));
    report(places::nearest(64.84, -147.72).name == QLatin1String("Fairbanks") && places::nearest(-0.18, -78.47).countryCode == QLatin1String("EC")
               && places::nearest(35.68, 139.69).countryCode == QLatin1String("JP"),
           QStringLiteral("places: north, south, east and west of here"));
    const double london = places::kilometres(51.5074, -0.1278, 48.8566, 2.3522);
    report(london > 340 && london < 345, QStringLiteral("places: London to Paris is about 343 km"), QString::number(london));
    report(places::mapLink(53.8, -1.5).startsWith(QLatin1String("https://www.openstreetmap.org/?mlat=53.800000&mlon=-1.500000")),
           QStringLiteral("places: a link to the spot on OpenStreetMap"), places::mapLink(53.8, -1.5));
    qsizetype points = 0;
    for (const places::Ring &r : places::land())
        points += r.size();
    report(places::land().size() > 1000 && points > 30000 && places::borders().size() > 200, QStringLiteral("places: the world map's outlines load"),
           QStringLiteral("%1 rings, %2 points").arg(places::land().size()).arg(points));
    QByteArray bad("DAMAP1\0\0", 8);
    bad += QByteArray::fromHex("ffffff7f0100020003000400");
    report(places::readRings(bad).isEmpty() && places::readRings("nonsense").isEmpty(), QStringLiteral("places: a damaged map file reads as nothing"));
}

// Which files the bad spots hit: a rescue map with unread bytes inside one file, and in free space.
void hitFiles()
{
    RescueMap small(1000);
    small.set(0, 1000, RescueMap::Finished);
    small.set(100, 50, RescueMap::Bad);
    small.set(400, 10, RescueMap::NonTried);
    report(small.missing(0, 1000) == 60 && small.missing(120, 300) == 40 && small.missing(150, 250) == 0 && small.missing(990, 100) == 0,
           QStringLiteral("rescue map: the bytes not read in a stretch are counted"));
    const QString data = QStringLiteral(SOURCE_DIR "/tests/data/lostfs/");
    for (const QString &name : {QStringLiteral("fat32"), QStringLiteral("ntfs")}) {
        const QByteArray image = unpackImage(data + name + QStringLiteral(".img.zst"));
        auto source = lost::Source::fromMemory(image);
        const QVector<lost::Found> found = quickScan(source);
        const auto kept = std::find_if(found.cbegin(), found.cend(), [](const lost::Found &f) { return f.name == QLatin1String("photo.jpg") && !f.deleted && !f.extents.isEmpty(); });
        if (kept == found.cend()) {
            report(false, QStringLiteral("%1: a file to hit").arg(name));
            continue;
        }
        RescueMap map(quint64(image.size()));
        map.set(0, map.size(), RescueMap::Finished);
        std::atomic<bool> cancel{false};
        report(hitfiles::find(*source, map, cancel).hits.isEmpty(), QStringLiteral("%1: a copy read whole: no file hit").arg(name));
        // 1000 bytes in the middle of photo.jpg, and 4 KB in the image's last stretch (free space).
        const lost::Extent first = kept->extents.first();
        map.set(first.start + 512, 1000, RescueMap::Bad);
        map.set(map.size() - 8192, 4096, RescueMap::Bad);
        const hitfiles::Report r = hitfiles::find(*source, map, cancel);
        const bool onlyIt = std::all_of(r.hits.cbegin(), r.hits.cend(), [](const hitfiles::Hit &h) { return h.deleted || h.path.endsWith(QLatin1String("/photo.jpg")); });
        const auto hit = std::find_if(r.hits.cbegin(), r.hits.cend(), [](const hitfiles::Hit &h) { return h.path.endsWith(QLatin1String("/photo.jpg")); });
        report(hit != r.hits.cend() && hit->missing == 1000 && onlyIt && r.inFiles == 1000 && r.missing == 5096 && r.volumes.size() == 1,
               QStringLiteral("%1: the file with unread bytes is named, with how much, and free space hits nothing").arg(name),
               hit == r.hits.cend() ? QStringLiteral("not found") : QStringLiteral("%1, %2 of %3 in files").arg(hit->missing).arg(r.inFiles).arg(r.missing));
        const QString text = hitfiles::asText(r, QStringLiteral("copy.img"));
        report(text.contains(QLatin1String("photo.jpg")) && text.contains(QLatin1String("came off whole: 1")), QStringLiteral("%1: the list as text").arg(name));
    }
    // Not a file system it can read: said, not guessed.
    auto blank = lost::Source::fromMemory(QByteArray(1 << 20, '\0'));
    RescueMap map(1 << 20);
    map.set(0, 1 << 20, RescueMap::Finished);
    map.set(4096, 4096, RescueMap::Bad);
    std::atomic<bool> cancel{false};
    const hitfiles::Report none = hitfiles::find(*blank, map, cancel);
    report(none.hits.isEmpty() && none.unread.size() == 1 && none.missing == 4096, QStringLiteral("hit files: no file system it can read is said so"), none.unread.value(0));
}

void lostFilesTests()
{
    hitFiles();
    previewQueue();
    viewerMessages();
    lookCopies();
    photoFacts();
    journal();
    QTemporaryDir dir;
    finding(dir.path());
    damage();
    names();
    fuzz();
    fileSystems(dir.path());
    fileSystemFuzz();
    shadowCopies();
    shadowCopyFuzz();
    virtualDisks();
    virtualDiskFuzz();
    mediaAndOffice();
    newKinds();
    namesFromContents();
    fileStructure();
    repairs();
    wordSearch();
    thumbnailCaches();
    ransomwareSigns();
    readThrough();
    btrfsRestore();
    windowsCompressed();
    placeNames();
    foundReport();
    enhanceTools();
}
