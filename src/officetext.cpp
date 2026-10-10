// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "officetext.h"

#include "filetypes.h"

#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>

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

QVector<Entry> list(const QByteArray &data, int most, bool *complete)
{
    QVector<Entry> out;
    if (complete)
        *complete = false;
    Archive a = open(data);
    if (!a)
        return out;
    archive_entry *entry = nullptr;
    int result = ARCHIVE_OK;
    while (out.size() < most && (result = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK) {
        Entry e;
        e.path = entryPath(entry);
        e.folder = archive_entry_filetype(entry) == AE_IFDIR || e.path.endsWith(QLatin1Char('/'));
        e.size = archive_entry_size_is_set(entry) ? quint64(std::max<la_int64_t>(0, archive_entry_size(entry))) : 0;
        out.push_back(e);
    }
    if (complete)
        *complete = result == ARCHIVE_EOF;
    return out;
}

} // namespace officetext
