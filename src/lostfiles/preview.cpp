// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "preview.h"

#include "exif.h"
#include "filetypes.h"
#include "format.h"
#include "mediainfo.h"
#include "officetext.h"
#include "ole.h"
#include "repair.h"
#include "pictures.h"
#include "textkinds.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDataStream>
#include <QDeadlineTimer>
#include <QFile>
#include <QImageReader>
#include <QLocale>
#include <QProcess>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QTemporaryFile>

#include <algorithm>

#ifdef DISKANVIL_PDF
#include <poppler-qt6.h>
#endif

#include <climits>
#include <memory>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#else
#include <sys/resource.h>
#endif

namespace preview {

namespace {

constexpr quint32 kMagic = 0x44465056; // "DFPV"
constexpr qint64 kMostMessage = kMostPdfData + 1024 * 1024;

QString tr(const char *text)
{
    return QCoreApplication::translate("preview", text);
}

// A film's first frame, from ffmpeg when it's installed (never on DiskAnvil Live: too big).
QImage frame(const QByteArray &data, int pixels)
{
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty())
        return {};
    QTemporaryFile file;
    if (!file.open() || file.write(data) != data.size())
        return {};
    file.flush();
    for (const char *at : {"1", "0"}) {
        QProcess p;
        p.start(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-ss"), QString::fromLatin1(at), QStringLiteral("-i"),
                         file.fileName(), QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-vf"),
                         QStringLiteral("scale='min(%1,iw)':-2").arg(pixels), QStringLiteral("-f"), QStringLiteral("image2pipe"),
                         QStringLiteral("-vcodec"), QStringLiteral("png"), QStringLiteral("-")});
        if (!p.waitForFinished(8000)) {
            p.kill();
            p.waitForFinished(1000);
            continue;
        }
        QImage image;
        if (image.loadFromData(p.readAllStandardOutput(), "PNG"))
            return image;
    }
    return {};
}

// Plain text, when the bytes look like it: valid UTF-8 (or Latin-1) without control characters.
QString textOf(const QByteArray &data, int most)
{
    const QByteArray head = data.left(most * 2);
    if (head.isEmpty() || head.contains('\0'))
        return {};
    QStringDecoder utf8(QStringDecoder::Utf8);
    QString text = utf8(head);
    if (utf8.hasError())
        text = QString::fromLatin1(head);
    int odd = 0;
    for (const QChar c : std::as_const(text))
        odd += c.unicode() < 0x20 && c != QLatin1Char('\n') && c != QLatin1Char('\r') && c != QLatin1Char('\t');
    if (odd > text.size() / 50)
        return {};
    return text.left(most);
}

// A plain file (the helper's input) blocks in read() and says 0 only at its end; a process
// (the window's side) says 0 when nothing has come yet, and is waited for until the deadline.
bool readExactly(QIODevice &from, char *to, qint64 n, const QDeadlineTimer &deadline)
{
    while (n > 0) {
        const qint64 got = from.read(to, n);
        if (got < 0)
            return false;
        if (got == 0 && !from.waitForReadyRead(int(std::min<qint64>(deadline.remainingTime(), INT_MAX))))
            return false;
        to += got;
        n -= got;
    }
    return true;
}

bool readMessage(QIODevice &from, QByteArray &body, int msecs)
{
    const QDeadlineTimer deadline(msecs < 0 ? QDeadlineTimer::Forever : QDeadlineTimer(msecs));
    quint32 header[2];
    if (!readExactly(from, reinterpret_cast<char *>(header), sizeof header, deadline))
        return false;
    if (header[0] != kMagic || header[1] > kMostMessage)
        return false;
    body.resize(qsizetype(header[1]));
    return readExactly(from, body.data(), body.size(), deadline);
}

void writeMessage(QIODevice &to, const QByteArray &body)
{
    const quint32 header[2] = {kMagic, quint32(body.size())};
    to.write(reinterpret_cast<const char *>(header), sizeof header);
    to.write(body);
}

} // namespace

void writeStrings(QDataStream &to, const QStringList &list)
{
    to << quint32(list.size());
    for (const QString &item : list)
        to << item;
}

bool readStrings(QDataStream &from, QStringList &list, quint32 most)
{
    list.clear();
    quint32 n = 0;
    from >> n;
    if (n > most) {
        from.setStatus(QDataStream::ReadCorruptData);
        return false;
    }
    for (quint32 i = 0; i < n && from.status() == QDataStream::Ok; ++i) {
        QString item;
        from >> item; // Qt reads a string's length in steps, so a damaged one can't ask for gigabytes
        list << item;
    }
    return from.status() == QDataStream::Ok;
}

void write(QIODevice &to, const Request &r)
{
    QByteArray body;
    QDataStream s(&body, QIODevice::WriteOnly);
    s << qint32(r.type) << r.name << r.size << qint32(r.pixels) << r.data << r.moov;
    writeMessage(to, body);
}

bool read(QIODevice &from, Request &r, int msecs)
{
    QByteArray body;
    if (!readMessage(from, body, msecs))
        return false;
    QDataStream s(body);
    qint32 type = -1, pixels = 0;
    s >> type >> r.name >> r.size >> pixels >> r.data >> r.moov;
    r.type = type;
    r.pixels = std::clamp(pixels, 16, 2048);
    return s.status() == QDataStream::Ok;
}

void write(QIODevice &to, const Result &r)
{
    QByteArray body;
    QDataStream s(&body, QIODevice::WriteOnly);
    QByteArray png;
    if (!r.image.isNull()) {
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        r.image.save(&buffer, "PNG");
    }
    s << png << r.text;
    writeStrings(s, r.facts);
    writeMessage(to, body);
}

bool read(QIODevice &from, Result &r, int msecs)
{
    QByteArray body;
    if (!readMessage(from, body, msecs))
        return false;
    QDataStream s(body);
    QByteArray png;
    s >> png >> r.text;
    readStrings(s, r.facts);
    if (!png.isEmpty())
        r.image.loadFromData(png, "PNG");
    return s.status() == QDataStream::Ok;
}

Result make(const Request &r)
{
    Result out;
    const filetypes::Type *type = r.type >= 0 && r.type < filetypes::types().size() ? &filetypes::types()[r.type] : nullptr;
    const QString id = type ? type->id : QString();
    const bool partial = quint64(r.data.size()) < r.size;

    if (type && type->category == filetypes::Category::Picture) {
        out.image = pictures::decode(r.data, r.type, r.pixels);
        // When, with what and where it was taken, as the camera wrote it down.
        out.facts = exif::describe(exif::read(r.data));
        if (std::none_of(out.facts.cbegin(), out.facts.cend(), [](const QString &f) { return f.startsWith(exif::sizeLabel() + QLatin1Char('\t')); })) {
            const QSize size = pictures::size(r.data, r.type);
            if (size.isValid())
                out.facts << exif::sizeLabel() + QLatin1Char('\t') + tr("%1 × %2 pixels").arg(size.width()).arg(size.height());
        }
        return out;
    }
#ifdef DISKANVIL_PDF
    if (id == QLatin1String("pdf")) {
        std::unique_ptr<Poppler::Document> document = Poppler::Document::loadFromData(r.data);
        if (document && document->isLocked()) {
            out.text = tr("It's locked with a password, so its pages can't be shown.");
            out.facts << tr("Locked") + QLatin1Char('\t') + tr("It needs a password to open");
            return out;
        }
        if (!document)
            return out;
        document->setRenderHint(Poppler::Document::Antialiasing);
        document->setRenderHint(Poppler::Document::TextAntialiasing);
        if (std::unique_ptr<Poppler::Page> page = document->page(0)) {
            const QSizeF points = page->pageSizeF();
            const double longest = std::max(points.width(), points.height());
            const double dpi = longest > 0 ? std::min(150.0, r.pixels * 72.0 / longest) : 72.0;
            out.image = page->renderToImage(dpi, dpi);
        }
        const QString title = document->info(QStringLiteral("Title")).simplified();
        out.text = (title.isEmpty() ? QString() : title + QLatin1Char('\n')) + QCoreApplication::translate("preview", "%n page(s)", nullptr, document->numPages());
        // What it says about itself: who made it, with what, when.
        const auto fact = [&out](const QString &label, const QString &value) {
            if (!value.simplified().isEmpty())
                out.facts << label + QLatin1Char('\t') + value.simplified().left(200);
        };
        const auto when = [](const QDateTime &t) {
            if (!t.isValid() || t.date().year() <= 1900)
                return QString();
            const QDateTime local = t.toLocalTime();
            return QLocale().toString(local.date(), QLocale::LongFormat) + QStringLiteral(", ") + QLocale().toString(local.time(), QLocale::ShortFormat);
        };
        fact(tr("Title"), title);
        fact(tr("Author"), document->info(QStringLiteral("Author")));
        fact(tr("Subject"), document->info(QStringLiteral("Subject")));
        fact(tr("Created"), when(document->creationDate()));
        fact(tr("Changed"), when(document->modificationDate()));
        fact(tr("Made with"), document->info(QStringLiteral("Creator")));
        fact(tr("Turned into a PDF by"), document->info(QStringLiteral("Producer")));
        fact(tr("Pages"), QString::number(document->numPages()));
        // Saved again since it was made: the earlier versions are still inside it.
        if (quint64(r.data.size()) == r.size) {
            const int saves = int(repair::pdfVersions(r.data).size()) - 1;
            if (saves > 0)
                fact(tr("Saved again"), QCoreApplication::translate("preview", "%n time(s) after it was made. The earlier versions are still inside it "
                                                                                "(right-click it: Earlier Versions Inside It)", nullptr, saves));
        }
        if (document->isEncrypted())
            fact(tr("Locked"), tr("It has a password for changing or printing it"));
        return out;
    }
#endif
    if (id == QLatin1String("doc") || id == QLatin1String("xls") || id == QLatin1String("ppt") || id == QLatin1String("msg")
        || id == QLatin1String("ole")) {
        const ole::File file(r.data);
        out.text = ole::text(file);
        out.facts = ole::properties(file);
        if (out.text.isEmpty() && ole::locked(file))
            out.text = tr("It's locked with a password, so its words can't be shown.");
        return out;
    }
    if (QStringList{QStringLiteral("eml"), QStringLiteral("mbox"), QStringLiteral("rtf"), QStringLiteral("html"), QStringLiteral("xml"),
                    QStringLiteral("ics"), QStringLiteral("vcf")}
            .contains(id)) {
        out.text = texts::text(r.type, r.data);
        out.facts = texts::properties(r.type, r.data);
        return out;
    }
    if (type && type->category == filetypes::Category::Document) {
        out.text = officetext::text(r.type, r.data);
        out.facts = officetext::properties(r.type, r.data);
        if (!out.text.isEmpty() || id != QLatin1String("epub"))
            return out;
    }
    if (type && (type->category == filetypes::Category::Archive || id == QLatin1String("epub"))) {
        bool complete = false;
        const QVector<officetext::Entry> entries = officetext::list(r.data, 300, &complete);
        if (entries.isEmpty())
            return out;
        QStringList lines;
        for (const officetext::Entry &e : entries)
            lines << (e.folder ? e.path : QStringLiteral("%1  (%2)").arg(e.path, formatSize(e.size)));
        out.text = tr("What's in it:") + QLatin1Char('\n') + lines.join(QLatin1Char('\n'));
        if (!complete)
            out.text += QLatin1Char('\n') + (partial ? tr("…and more (only its start was looked at)") : tr("…and more, or it's damaged from here on"));
        return out;
    }
    if (type && (type->category == filetypes::Category::Music || type->category == filetypes::Category::Video)) {
        mediainfo::Info info = mediainfo::read(r.type, r.data, r.size);
        if (!r.moov.isEmpty()) {
            // Its moov box was further on: everything but the cover is in there.
            const QByteArray cover = info.cover;
            info = mediainfo::read(r.type, r.moov, r.size);
            if (info.cover.isEmpty())
                info.cover = cover;
        }
        out.text = mediainfo::describe(info);
        out.facts += mediainfo::facts(info);
        if (!info.cover.isEmpty())
            out.image = pictures::decode(info.cover, -1, r.pixels);
        if (out.image.isNull() && type->category == filetypes::Category::Video)
            out.image = frame(r.data, r.pixels);
        return out;
    }
    if (!type)
        out.text = textOf(r.data, 4000);
    return out;
}

int runHelper()
{
#ifdef Q_OS_WIN
    _setmode(0, _O_BINARY);
    _setmode(1, _O_BINARY);
#else
    // A decoder gone wrong stops at these, not at the machine's limits.
    const rlimit memory {2048ull * 1024 * 1024, 2048ull * 1024 * 1024};
    setrlimit(RLIMIT_AS, &memory);
    const rlimit files {512ull * 1024 * 1024, 512ull * 1024 * 1024};
    setrlimit(RLIMIT_FSIZE, &files);
    const rlimit core {0, 0};
    setrlimit(RLIMIT_CORE, &core);
#endif
    QImageReader::setAllocationLimit(256);
    QFile in, out;
    if (!in.open(0, QIODevice::ReadOnly | QIODevice::Unbuffered) || !out.open(1, QIODevice::WriteOnly | QIODevice::Unbuffered))
        return 1;
    Request request;
    while (read(in, request)) {
        write(out, make(request));
        out.flush();
    }
    return 0;
}

} // namespace preview
