// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "officetext.h"

#include "filetypes.h"

#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QLocale>
#include <QXmlStreamReader>
#include <QtEndian>

#include <archive.h>
#include <archive_entry.h>

#include <functional>
#include <memory>

namespace officetext {

namespace {

constexpr qint64 kMostPerMember = 16 * 1024 * 1024; // of one XML file inside, unpacked

using Archive = std::unique_ptr<archive, decltype(&archive_read_free)>;

Archive open(const QByteArray &data)
{
    Archive a(archive_read_new(), &archive_read_free);
    archive_read_support_format_zip(a.get());
    archive_read_support_format_7zip(a.get());
    archive_read_support_format_rar(a.get());
    archive_read_support_format_rar5(a.get());
    archive_read_support_format_tar(a.get()); // inside a .gz (.tar.gz)
    archive_read_support_filter_all(a.get());
    if (archive_read_open_memory(a.get(), data.constData(), size_t(data.size())) != ARCHIVE_OK)
        return Archive(nullptr, &archive_read_free);
    return a;
}

QString entryPath(archive_entry *entry)
{
    const char *utf8 = archive_entry_pathname_utf8(entry);
    return utf8 ? QString::fromUtf8(utf8) : QString::fromLocal8Bit(archive_entry_pathname(entry));
}

// The members whose names `want` accepts, unpacked (each up to the limit), by name.
QMap<QString, QByteArray> members(const QByteArray &data, const std::function<bool(const QString &)> &want, int most)
{
    QMap<QString, QByteArray> out;
    Archive a = open(data);
    if (!a)
        return out;
    archive_entry *entry = nullptr;
    for (int n = 0; n < 100000 && int(out.size()) < most && archive_read_next_header(a.get(), &entry) == ARCHIVE_OK; ++n) {
        const QString path = entryPath(entry);
        if (!want(path))
            continue;
        QByteArray content;
        char buffer[65536];
        for (la_ssize_t got; (got = archive_read_data(a.get(), buffer, sizeof buffer)) > 0;) {
            content.append(buffer, got);
            if (content.size() >= kMostPerMember)
                break;
        }
        out.insert(path, content);
    }
    return out;
}

// The characters inside `inText` elements, a new line after each `lineAfter` one.
QString xmlText(const QByteArray &xml, const QSet<QString> &inText, const QSet<QString> &lineAfter, int most)
{
    QXmlStreamReader r(xml);
    r.setNamespaceProcessing(true);
    QString out;
    int depth = 0;
    while (!r.atEnd() && out.size() < most) {
        switch (r.readNext()) {
        case QXmlStreamReader::StartElement: {
            const QString name = r.name().toString();
            if (inText.contains(name))
                ++depth;
            else if (depth > 0 && name == QLatin1String("tab"))
                out += QLatin1Char('\t');
            else if (depth > 0 && name == QLatin1String("s"))
                out += QLatin1Char(' ');
            else if (depth > 0 && (name == QLatin1String("br") || name == QLatin1String("line-break")))
                out += QLatin1Char('\n');
            break;
        }
        case QXmlStreamReader::EndElement: {
            const QString name = r.name().toString();
            if (inText.contains(name))
                depth = std::max(0, depth - 1);
            if (lineAfter.contains(name) && !out.endsWith(QLatin1Char('\n')) && !out.isEmpty())
                out += QLatin1Char('\n');
            break;
        }
        case QXmlStreamReader::Characters:
            if (depth > 0)
                out += r.text();
            break;
        default:
            break;
        }
    }
    static const QRegularExpression blankLines(QStringLiteral("\n{3,}"));
    out.replace(blankLines, QStringLiteral("\n\n"));
    return out.trimmed().left(most);
}

} // namespace

QString text(int type, const QByteArray &data, int most)
{
    if (type < 0 || type >= filetypes::types().size())
        return {};
    const QString id = filetypes::types()[type].id;
    if (id == QLatin1String("docx")) {
        const auto m = members(data, [](const QString &p) { return p == QLatin1String("word/document.xml"); }, 1);
        return m.isEmpty() ? QString() : xmlText(m.first(), {QStringLiteral("t")}, {QStringLiteral("p")}, most);
    }
    if (id == QLatin1String("xlsx")) {
        // The text in the cells (numbers live in the sheets themselves, and aren't much to read).
        const auto m = members(data, [](const QString &p) { return p == QLatin1String("xl/sharedStrings.xml"); }, 1);
        return m.isEmpty() ? QString() : xmlText(m.first(), {QStringLiteral("t")}, {QStringLiteral("si")}, most);
    }
    if (id == QLatin1String("pptx")) {
        static const QRegularExpression slide(QStringLiteral("^ppt/slides/slide(\\d+)\\.xml$"));
        const auto m = members(data, [](const QString &p) { return slide.match(p).hasMatch(); }, 200);
        QMap<int, QString> slides; // in slide order, not archive order
        for (auto it = m.cbegin(); it != m.cend(); ++it)
            slides.insert(slide.match(it.key()).captured(1).toInt(), xmlText(it.value(), {QStringLiteral("t")}, {QStringLiteral("p")}, most));
        QString out;
        for (auto it = slides.cbegin(); it != slides.cend() && out.size() < most; ++it)
            out += it.value() + QStringLiteral("\n\n");
        return out.trimmed().left(most);
    }
    if (id == QLatin1String("odt") || id == QLatin1String("ods") || id == QLatin1String("odp")) {
        const auto m = members(data, [](const QString &p) { return p == QLatin1String("content.xml"); }, 1);
        return m.isEmpty() ? QString() : xmlText(m.first(), {QStringLiteral("p"), QStringLiteral("h")}, {QStringLiteral("p"), QStringLiteral("h")}, most);
    }
    return {};
}

QStringList properties(int type, const QByteArray &data)
{
    if (type < 0 || type >= filetypes::types().size())
        return {};
    const QString id = filetypes::types()[type].id;
    const bool ooxml = id == QLatin1String("docx") || id == QLatin1String("xlsx") || id == QLatin1String("pptx");
    const bool odf = id == QLatin1String("odt") || id == QLatin1String("ods") || id == QLatin1String("odp");
    if (!ooxml && !odf)
        return {};
    const auto m = members(
        data,
        [ooxml](const QString &p) {
            return ooxml ? p == QLatin1String("docProps/core.xml") || p == QLatin1String("docProps/app.xml") : p == QLatin1String("meta.xml");
        },
        2);
    // Each element's text by its name without the namespace, the first of each.
    QHash<QString, QString> v;
    for (const QByteArray &xml : m) {
        QXmlStreamReader r(xml);
        r.setNamespaceProcessing(true);
        QString current;
        while (!r.atEnd()) {
            switch (r.readNext()) {
            case QXmlStreamReader::StartElement:
                current = r.name().toString();
                if (current == QLatin1String("document-statistic")) {
                    for (const QXmlStreamAttribute &a : r.attributes())
                        v.insert(a.name().toString(), a.value().toString());
                }
                break;
            case QXmlStreamReader::Characters:
                if (!current.isEmpty() && !v.contains(current) && !r.isWhitespace())
                    v.insert(current, r.text().toString().simplified().left(200));
                break;
            case QXmlStreamReader::EndElement:
                current.clear();
                break;
            default:
                break;
            }
        }
    }
    QStringList out;
    const auto add = [&out](const char *label, const QString &value) {
        if (!value.trimmed().isEmpty())
            out << QCoreApplication::translate("officetext", label) + QLatin1Char('\t') + value.trimmed();
    };
    // "2024-07-14T13:22:05Z" (Word, in UTC) or without a zone (OpenDocument): shown in local time.
    const auto when = [](const QString &iso) {
        const QDateTime t = QDateTime::fromString(iso, Qt::ISODateWithMs).isValid() ? QDateTime::fromString(iso, Qt::ISODateWithMs)
                                                                                     : QDateTime::fromString(iso.left(19), Qt::ISODate);
        if (!t.isValid() || t.date().year() <= 1900)
            return QString();
        const QDateTime local = t.toLocalTime();
        return QLocale().toString(local.date(), QLocale::LongFormat) + QStringLiteral(", ") + QLocale().toString(local.time(), QLocale::ShortFormat);
    };
    // A length of time: Office's in minutes, OpenDocument's as "PT2H5M10S".
    const auto spent = [](qint64 minutes) {
        if (minutes <= 0 || minutes > 10LL * 365 * 24 * 60)
            return QString();
        return minutes < 60 ? QCoreApplication::translate("officetext", "%1 min").arg(minutes)
                            : QCoreApplication::translate("officetext", "%1 h %2 min").arg(minutes / 60).arg(minutes % 60);
    };
    const auto isoMinutes = [](const QString &d) {
        static const QRegularExpression parts(QStringLiteral("^P(?:(\\d+)D)?T?(?:(\\d+)H)?(?:(\\d+)M)?(?:(\\d+)(?:\\.\\d+)?S)?$"));
        const QRegularExpressionMatch m = parts.match(d);
        return m.hasMatch() ? m.captured(1).toLongLong() * 1440 + m.captured(2).toLongLong() * 60 + m.captured(3).toLongLong()
                                  + (m.captured(4).toLongLong() + 59) / 60
                            : 0LL;
    };
    add(QT_TRANSLATE_NOOP("officetext", "Title"), v.value(QStringLiteral("title")));
    add(QT_TRANSLATE_NOOP("officetext", "Subject"), v.value(QStringLiteral("subject")));
    add(QT_TRANSLATE_NOOP("officetext", "Keywords"), v.value(ooxml ? QStringLiteral("keywords") : QStringLiteral("keyword")));
    add(QT_TRANSLATE_NOOP("officetext", "Comments"), v.value(QStringLiteral("description")));
    if (ooxml) {
        add(QT_TRANSLATE_NOOP("officetext", "Author"), v.value(QStringLiteral("creator")));
        add(QT_TRANSLATE_NOOP("officetext", "Last changed by"), v.value(QStringLiteral("lastModifiedBy")));
        add(QT_TRANSLATE_NOOP("officetext", "Created"), when(v.value(QStringLiteral("created"))));
        add(QT_TRANSLATE_NOOP("officetext", "Changed"), when(v.value(QStringLiteral("modified"))));
        add(QT_TRANSLATE_NOOP("officetext", "Last printed"), when(v.value(QStringLiteral("lastPrinted"))));
        if (v.value(QStringLiteral("revision")).toInt() > 0)
            add(QT_TRANSLATE_NOOP("officetext", "Times saved"), v.value(QStringLiteral("revision")));
        add(QT_TRANSLATE_NOOP("officetext", "Editing time"), spent(v.value(QStringLiteral("TotalTime")).toLongLong()));
        add(QT_TRANSLATE_NOOP("officetext", "Template"), v.value(QStringLiteral("Template")));
        add(QT_TRANSLATE_NOOP("officetext", "Manager"), v.value(QStringLiteral("Manager")));
        add(QT_TRANSLATE_NOOP("officetext", "Category"), v.value(QStringLiteral("category")));
        add(QT_TRANSLATE_NOOP("officetext", "Made with"),
            (v.value(QStringLiteral("Application")) + QLatin1Char(' ') + v.value(QStringLiteral("AppVersion"))).trimmed());
        add(QT_TRANSLATE_NOOP("officetext", "Company"), v.value(QStringLiteral("Company")));
        add(QT_TRANSLATE_NOOP("officetext", "Pages"), v.value(QStringLiteral("Pages")));
        add(QT_TRANSLATE_NOOP("officetext", "Words"), v.value(QStringLiteral("Words")));
        add(QT_TRANSLATE_NOOP("officetext", "Slides"), v.value(QStringLiteral("Slides")));
    } else {
        add(QT_TRANSLATE_NOOP("officetext", "Author"), v.value(QStringLiteral("initial-creator")));
        add(QT_TRANSLATE_NOOP("officetext", "Last changed by"), v.value(QStringLiteral("creator")));
        add(QT_TRANSLATE_NOOP("officetext", "Created"), when(v.value(QStringLiteral("creation-date"))));
        add(QT_TRANSLATE_NOOP("officetext", "Changed"), when(v.value(QStringLiteral("date"))));
        add(QT_TRANSLATE_NOOP("officetext", "Last printed"), when(v.value(QStringLiteral("print-date"))));
        add(QT_TRANSLATE_NOOP("officetext", "Printed by"), v.value(QStringLiteral("printed-by")));
        if (v.value(QStringLiteral("editing-cycles")).toInt() > 0)
            add(QT_TRANSLATE_NOOP("officetext", "Times saved"), v.value(QStringLiteral("editing-cycles")));
        add(QT_TRANSLATE_NOOP("officetext", "Editing time"), spent(isoMinutes(v.value(QStringLiteral("editing-duration")))));
        add(QT_TRANSLATE_NOOP("officetext", "Made with"), v.value(QStringLiteral("generator")).section(QLatin1Char('$'), 0, 0));
        add(QT_TRANSLATE_NOOP("officetext", "Pages"), v.value(QStringLiteral("page-count")));
        add(QT_TRANSLATE_NOOP("officetext", "Words"), v.value(QStringLiteral("word-count")));
    }
    return out;
}

QVector<Entry> list(const QByteArray &data, int most, bool *complete)
{
    QVector<Entry> out;
    if (complete)
        *complete = false;
    Archive a = open(data);
    archive_entry *entry = nullptr;
    int result = ARCHIVE_FATAL;
    while (a && out.size() < most && (result = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK) {
        Entry e;
        e.path = entryPath(entry);
        e.folder = archive_entry_filetype(entry) == AE_IFDIR || e.path.endsWith(QLatin1Char('/'));
        e.size = archive_entry_size_is_set(entry) ? quint64(std::max<la_int64_t>(0, archive_entry_size(entry))) : 0;
        out.push_back(e);
    }
    if (complete)
        *complete = result == ARCHIVE_EOF;
    // A .gz that isn't a .tar.gz packs one file: its name is in the gzip header, and its size in
    // the last four bytes (when the whole file is there).
    const auto *b = reinterpret_cast<const uchar *>(data.constData());
    if (out.isEmpty() && data.size() > 18 && b[0] == 0x1F && b[1] == 0x8B && b[2] == 8) {
        Entry e;
        if (b[3] & 8) {
            qsizetype at = 10 + ((b[3] & 4) ? 2 + (b[10] | b[11] << 8) : 0);
            const qsizetype zero = at < data.size() ? data.indexOf('\0', at) : -1;
            if (zero > at && zero - at < 1024)
                e.path = QString::fromLatin1(data.mid(at, zero - at)); // ISO 8859-1, as gzip says
        }
        if (e.path.isEmpty())
            e.path = QCoreApplication::translate("officetext", "(one file, its name not kept)");
        e.size = qFromLittleEndian<quint32>(b + data.size() - 4);
        out.push_back(e);
        if (complete)
            *complete = true;
    }
    return out;
}

} // namespace officetext
