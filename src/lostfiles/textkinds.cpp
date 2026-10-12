// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "textkinds.h"

#include "filetypes.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QTimeZone>
#include <QVector>

#include <algorithm>

namespace texts {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("texts", text);
}

QString idOf(int type)
{
    return type >= 0 && type < filetypes::types().size() ? filetypes::types()[type].id : QString();
}

QString when(const QDateTime &t)
{
    if (!t.isValid())
        return {};
    const QDateTime local = t.toLocalTime();
    return QLocale().toString(local.date(), QLocale::LongFormat) + QStringLiteral(", ") + QLocale().toString(local.time(), QLocale::ShortFormat);
}

// Bytes in a named character set; UTF-8 when it's not known, and Latin-1 when they aren't UTF-8.
QString decode(const QByteArray &bytes, QByteArray charset)
{
    charset = charset.trimmed().toLower();
    if (charset.isEmpty() || charset == "us-ascii")
        charset = "utf-8";
    QStringDecoder named(charset.constData());
    if (named.isValid()) {
        const QString out = named(bytes);
        if (!named.hasError())
            return out;
    }
    QStringDecoder utf8(QStringDecoder::Utf8);
    const QString out = utf8(bytes);
    if (!utf8.hasError())
        return out;
    QStringDecoder western("windows-1252");
    return western.isValid() ? QString(western(bytes)) : QString::fromLatin1(bytes);
}

QString plainLines(QString text, int most)
{
    text.replace(QLatin1String("\r\n"), QLatin1String("\n")).replace(QLatin1Char('\r'), QLatin1Char('\n'));
    static const QRegularExpression blankLines(QStringLiteral("\n[ \t]*\n([ \t]*\n)+"));
    text.replace(blankLines, QStringLiteral("\n\n"));
    return text.trimmed().left(most);
}

// --- Emails (RFC 5322 and MIME)

int hexValue(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

QByteArray quotedPrintable(const QByteArray &in, bool underscores)
{
    QByteArray out;
    out.reserve(in.size());
    for (qsizetype i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '=' && i + 2 < in.size() && hexValue(in[i + 1]) >= 0 && hexValue(in[i + 2]) >= 0) {
            out += char(hexValue(in[i + 1]) << 4 | hexValue(in[i + 2]));
            i += 2;
        } else if (c == '=' && i + 1 < in.size() && (in[i + 1] == '\n' || in[i + 1] == '\r')) { // a line carried on
            i += in[i + 1] == '\r' && i + 2 < in.size() && in[i + 2] == '\n' ? 2 : 1;
        } else {
            out += underscores && c == '_' ? ' ' : c;
        }
    }
    return out;
}

// "=?UTF-8?B?...?=" and "=?iso-8859-1?Q?...?=" in headers (RFC 2047), the rest as UTF-8.
QString headerText(const QByteArray &value)
{
    static const QRegularExpression word(QStringLiteral("=\\?([^?\\s]+)\\?([BbQq])\\?([^?\\s]*)\\?="));
    const QString raw = decode(value, "utf-8");
    QString out;
    qsizetype at = 0;
    bool lastWasEncoded = false;
    for (auto it = word.globalMatch(raw); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const QString between = raw.mid(at, m.capturedStart() - at);
        if (!(lastWasEncoded && between.trimmed().isEmpty())) // spaces between two encoded words go
            out += between;
        const QByteArray data = m.captured(3).toLatin1();
        const QByteArray bytes = m.captured(2).toUpper() == QLatin1String("B") ? QByteArray::fromBase64(data) : quotedPrintable(data, true);
        out += decode(bytes, m.captured(1).section(QLatin1Char('*'), 0, 0).toLatin1());
        at = m.capturedEnd();
        lastWasEncoded = true;
    }
    out += raw.mid(at);
    return out.simplified();
}

struct Headers {
    QVector<QPair<QByteArray, QByteArray>> list; // names in lower case, values unfolded

    QByteArray value(const char *name) const
    {
        for (const auto &h : list) {
            if (h.first == name)
                return h.second;
        }
        return {};
    }
};

// Splits a message or part into its headers and its body.
Headers split(const QByteArray &message, QByteArray *body)
{
    qsizetype end = message.indexOf("\r\n\r\n");
    qsizetype bodyAt = end + 4;
    const qsizetype lf = message.indexOf("\n\n");
    if (lf >= 0 && (end < 0 || lf < end)) {
        end = lf;
        bodyAt = lf + 2;
    }
    if (end < 0) {
        end = message.size();
        bodyAt = message.size();
    }
    Headers h;
    for (const QByteArray &rawLine : message.left(end).split('\n')) {
        QByteArray line = rawLine;
        if (line.endsWith('\r'))
            line.chop(1);
        if ((line.startsWith(' ') || line.startsWith('\t')) && !h.list.isEmpty()) {
            h.list.last().second += ' ' + line.trimmed();
        } else if (const qsizetype colon = line.indexOf(':'); colon > 0 && h.list.size() < 1000) {
            h.list.push_back({line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed()});
        }
    }
    if (body)
        *body = message.mid(bodyAt);
    return h;
}

// A parameter of a header: `boundary` in multipart/mixed; boundary="xyz". Handles RFC 2231's
// filename*=UTF-8''name%20here too.
QString parameter(const QByteArray &value, const QByteArray &name)
{
    for (const QByteArray &rawPart : value.split(';')) {
        const QByteArray part = rawPart.trimmed();
        const qsizetype eq = part.indexOf('=');
        if (eq < 0)
            continue;
        const QByteArray key = part.left(eq).trimmed().toLower();
        QByteArray v = part.mid(eq + 1).trimmed();
        if (v.size() >= 2 && v.startsWith('"') && v.endsWith('"'))
            v = v.mid(1, v.size() - 2);
        if (key == name)
            return headerText(v);
        if (key == name + '*') {
            const QList<QByteArray> pieces = v.split('\'');
            return decode(QByteArray::fromPercentEncoding(pieces.last()), pieces.size() == 3 ? pieces.first() : QByteArray("utf-8"));
        }
    }
    return {};
}

struct Mail {
    QString from, to, cc, subject;
    QDateTime sent;
    QString plain, html;
    QStringList attached;
};

void walkPart(const QByteArray &part, int depth, Mail &m)
{
    QByteArray body;
    const Headers h = split(part, &body);
    const QByteArray type = h.value("content-type");
    const QByteArray kind = type.split(';').first().trimmed().toLower();
    if (kind.startsWith("multipart/") && depth < 8) {
        const QByteArray boundary = "--" + parameter(type, "boundary").toUtf8();
        if (boundary.size() <= 2)
            return;
        qsizetype at = body.indexOf(boundary);
        for (int parts = 0; at >= 0 && parts < 500; ++parts) {
            qsizetype start = body.indexOf('\n', at);
            if (start < 0 || body.mid(at + boundary.size(), 2) == "--")
                break;
            ++start;
            const qsizetype next = body.indexOf("\n" + boundary, start);
            walkPart(body.mid(start, next < 0 ? -1 : next - start), depth + 1, m);
            at = next < 0 ? -1 : next + 1;
        }
        return;
    }
    const QByteArray disposition = h.value("content-disposition");
    QString name = parameter(disposition, "filename");
    if (name.isEmpty())
        name = parameter(type, "name");
    const bool text = kind.isEmpty() || kind.startsWith("text/");
    if (disposition.trimmed().toLower().startsWith("attachment") || (!name.isEmpty() && !text) || kind == "message/rfc822") {
        m.attached << (name.isEmpty() ? (kind == "message/rfc822" ? tr("a forwarded email") : tr("an attachment")) : name);
        return;
    }
    const QByteArray encoding = h.value("content-transfer-encoding").trimmed().toLower();
    const QByteArray bytes = encoding == "base64" ? QByteArray::fromBase64(body) : encoding == "quoted-printable" ? quotedPrintable(body, false) : body;
    const QByteArray charset = parameter(type, "charset").toLatin1();
    if ((kind.isEmpty() || kind == "text/plain") && m.plain.isEmpty())
        m.plain = decode(bytes, charset);
    else if (kind == "text/html" && m.html.isEmpty())
        m.html = decode(bytes, charset);
}

Mail readMail(const QByteArray &message)
{
    Mail m;
    const Headers h = split(message, nullptr);
    m.from = headerText(h.value("from"));
    m.to = headerText(h.value("to"));
    m.cc = headerText(h.value("cc"));
    m.subject = headerText(h.value("subject"));
    m.sent = QDateTime::fromString(QString::fromLatin1(h.value("date")).simplified(), Qt::RFC2822Date);
    walkPart(message, 0, m);
    return m;
}

QString mailText(const Mail &m, int most)
{
    QStringList head;
    const auto line = [&head](const QString &label, const QString &value) {
        if (!value.isEmpty())
            head << label + QStringLiteral(": ") + value;
    };
    line(tr("From"), m.from);
    line(tr("To"), m.to);
    line(tr("Cc"), m.cc);
    line(tr("Sent"), when(m.sent));
    line(tr("Subject"), m.subject);
    line(tr("Attached"), m.attached.join(QStringLiteral(", ")));
    return plainLines(head.join(QLatin1Char('\n')) + QStringLiteral("\n\n") + (m.plain.isEmpty() ? fromHtml(m.html) : m.plain), most);
}

// The messages of a mailbox: each starts after a "From " line at the start of a line.
QVector<QByteArray> mailbox(const QByteArray &data, int most)
{
    QVector<QByteArray> out;
    qsizetype at = data.startsWith("From ") ? 0 : -1;
    while (at >= 0 && out.size() < most) {
        const qsizetype start = data.indexOf('\n', at);
        if (start < 0)
            break;
        const qsizetype next = data.indexOf("\nFrom ", start);
        out << data.mid(start + 1, next < 0 ? -1 : next - start);
        at = next < 0 ? -1 : next + 1;
    }
    return out;
}

// --- RTF: control words and groups; the text is what's left, \'hh bytes in its code page

struct RtfInfo {
    QString title, author, subject, lastAuthor, company;
    QDateTime created, changed;
};

QString rtf(const QByteArray &d, int most, RtfInfo *info)
{
    static const QSet<QByteArray> skipped = {"fonttbl", "colortbl", "stylesheet", "pict", "object", "header", "footer", "headerl", "headerr",
                                             "footerl", "footerr", "generator", "listtable", "listoverridetable", "rsidtbl", "xmlnstbl",
                                             "themedata", "colorschememapping", "latentstyles", "datastore", "fldinst", "info", "filetbl",
                                             "revtbl", "pgdsctbl", "mmathPr", "operator", "nonshppict", "blipuid", "xe", "tc"};
    static const QSet<QByteArray> infoText = {"title", "author", "subject", "operator", "company"};
    struct Group {
        bool skip = false;
        int ucSkip = 1;
        QByteArray infoField; // inside \info: which field's text this is
        QByteArray timeField; // \creatim, \revtim
    };
    QVector<Group> groups{Group{}};
    QString out;
    QByteArray pending; // \'hh bytes, put into characters together
    QStringDecoder codepage("windows-1252");
    QHash<QByteArray, QString> fields;
    QHash<QByteArray, QHash<QByteArray, int>> times;
    int fallback = 0; // characters to skip after \uN
    const auto flush = [&] {
        if (pending.isEmpty())
            return;
        const QString s = codepage.isValid() ? QString(codepage(pending)) : QString::fromLatin1(pending);
        pending.clear();
        Group &g = groups.last();
        if (!g.infoField.isEmpty())
            fields[g.infoField] += s;
        else if (!g.skip)
            out += s;
    };
    const auto put = [&](const QString &s) {
        flush();
        Group &g = groups.last();
        if (!g.infoField.isEmpty())
            fields[g.infoField] += s;
        else if (!g.skip)
            out += s;
    };
    bool inInfo = false;
    int infoDepth = 0;
    for (qsizetype i = 0; i < d.size() && out.size() < most * 2 && groups.size() < 1000;) {
        const char c = d[i];
        if (c == '{') {
            flush();
            groups.push_back(groups.last());
            groups.last().infoField.clear();
            groups.last().timeField.clear();
            ++i;
            continue;
        }
        if (c == '}') {
            flush();
            if (groups.size() > 1)
                groups.removeLast();
            if (inInfo && groups.size() < infoDepth)
                inInfo = false;
            ++i;
            continue;
        }
        if (c == '\r' || c == '\n') {
            ++i;
            continue;
        }
        if (c != '\\') {
            if (fallback > 0)
                --fallback;
            else
                put(QString(QLatin1Char(c)));
            ++i;
            continue;
        }
        // A control word or symbol.
        if (i + 1 >= d.size())
            break;
        const char n = d[i + 1];
        if (n == '\'' && i + 3 < d.size()) {
            const int hi = hexValue(d[i + 2]), lo = hexValue(d[i + 3]);
            i += 4;
            if (fallback > 0) {
                --fallback;
                continue;
            }
            if (hi >= 0 && lo >= 0)
                pending += char(hi << 4 | lo);
            continue;
        }
        if (!((n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z'))) {
            i += 2;
            if (n == '*') {
                groups.last().skip = true;
            } else if (n == '~') {
                put(QString(QChar(0xA0)));
            } else if (n == '_') {
                put(QStringLiteral("-"));
            } else if (n == '\\' || n == '{' || n == '}') {
                put(QString(QLatin1Char(n)));
            } else if (n == '\n' || n == '\r') {
                put(QStringLiteral("\n"));
            }
            continue;
        }
        qsizetype j = i + 1;
        while (j < d.size() && ((d[j] >= 'a' && d[j] <= 'z') || (d[j] >= 'A' && d[j] <= 'Z')) && j - i < 32)
            ++j;
        const QByteArray word = d.mid(i + 1, j - i - 1);
        bool hasNumber = false;
        int number = 0;
        if (j < d.size() && (d[j] == '-' || (d[j] >= '0' && d[j] <= '9'))) {
            const qsizetype s = j;
            ++j;
            while (j < d.size() && d[j] >= '0' && d[j] <= '9' && j - s < 10)
                ++j;
            number = d.mid(s, j - s).toInt();
            hasNumber = true;
        }
        if (j < d.size() && d[j] == ' ')
            ++j;
        i = j;
        Group &g = groups.last();
        if (word == "bin" && hasNumber) {
            i += std::max(0, number);
            continue;
        }
        if (word == "info") {
            inInfo = true;
            infoDepth = int(groups.size());
            g.skip = true;
            continue;
        }
        if (inInfo && infoText.contains(word)) {
            g.infoField = fields.contains(word) ? QByteArray("again") : word; // the first of each is kept
            continue;
        }
        if (inInfo && (word == "creatim" || word == "revtim")) {
            g.timeField = word;
            continue;
        }
        if (inInfo && !g.timeField.isEmpty() && hasNumber) {
            times[g.timeField][word] = number;
            continue;
        }
        if (skipped.contains(word)) {
            g.skip = true;
            continue;
        }
        if (word == "ansicpg" && hasNumber) {
            QStringDecoder named(QByteArray("windows-" + QByteArray::number(number)).constData());
            if (named.isValid())
                codepage = std::move(named);
            continue;
        }
        if (word == "uc" && hasNumber) {
            g.ucSkip = std::clamp(number, 0, 8);
            continue;
        }
        if (word == "u" && hasNumber) {
            put(QString(QChar(char16_t(number < 0 ? number + 65536 : number))));
            fallback = g.ucSkip;
            continue;
        }
        if (word == "par" || word == "line" || word == "sect" || word == "page")
            put(QStringLiteral("\n"));
        else if (word == "tab" || word == "cell")
            put(QStringLiteral("\t"));
        else if (word == "row")
            put(QStringLiteral("\n"));
        else if (word == "emdash")
            put(QString(QChar(0x2014)));
        else if (word == "endash")
            put(QString(QChar(0x2013)));
        else if (word == "bullet")
            put(QString(QChar(0x2022)));
        else if (word == "lquote")
            put(QString(QChar(0x2018)));
        else if (word == "rquote")
            put(QString(QChar(0x2019)));
        else if (word == "ldblquote")
            put(QString(QChar(0x201C)));
        else if (word == "rdblquote")
            put(QString(QChar(0x201D)));
    }
    flush();
    if (info) {
        info->title = fields.value("title").simplified();
        info->author = fields.value("author").simplified();
        info->subject = fields.value("subject").simplified();
        info->lastAuthor = fields.value("operator").simplified();
        info->company = fields.value("company").simplified();
        const auto time = [](const QHash<QByteArray, int> &t) {
            const QDate day(t.value("yr"), t.value("mo"), t.value("dy"));
            return day.isValid() && day.year() > 1980 ? QDateTime(day, QTime(t.value("hr"), t.value("min"))) : QDateTime();
        };
        info->created = time(times.value("creatim"));
        info->changed = time(times.value("revtim"));
    }
    return plainLines(out, most);
}

// --- Calendars and contacts (iCalendar, vCard): lines of NAME;PARAMS:value

QVector<QPair<QString, QString>> cardLines(const QByteArray &data, int most)
{
    QVector<QPair<QString, QString>> out;
    QString text = decode(data, "utf-8");
    text.replace(QLatin1String("\r\n "), QString()).replace(QLatin1String("\r\n\t"), QString()); // carried-on lines
    text.replace(QLatin1String("\n "), QString()).replace(QLatin1String("\n\t"), QString());
    for (const QString &rawLine : text.split(QLatin1Char('\n'))) {
        const QString line = rawLine.trimmed();
        const qsizetype colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        QString name = line.left(colon);
        QString value = line.mid(colon + 1);
        if (name.contains(QLatin1String("ENCODING=QUOTED-PRINTABLE"), Qt::CaseInsensitive)) { // vCard 2.1
            const QString charset = name.section(QLatin1String("CHARSET="), 1).section(QLatin1Char(';'), 0, 0);
            value = decode(quotedPrintable(value.toLatin1(), false), charset.isEmpty() ? QByteArray("utf-8") : charset.toLatin1());
        }
        value.replace(QLatin1String("\\n"), QLatin1String(" ")).replace(QLatin1String("\\,"), QLatin1String(",")).replace(QLatin1String("\\;"), QLatin1String(";"));
        out.push_back({name.section(QLatin1Char(';'), 0, 0).toUpper(), value.trimmed()});
        if (out.size() >= most)
            break;
    }
    return out;
}

QDateTime icsTime(const QString &v)
{
    if (v.size() >= 15) {
        QDateTime t = QDateTime::fromString(v.left(15), QStringLiteral("yyyyMMdd'T'HHmmss"));
        if (v.endsWith(QLatin1Char('Z')))
            t.setTimeZone(QTimeZone::UTC);
        return t;
    }
    return QDateTime(QDate::fromString(v.left(8), QStringLiteral("yyyyMMdd")), QTime(0, 0));
}

QString calendar(const QByteArray &data, int most, int *events)
{
    QStringList lines;
    QString summary, place;
    QDateTime start;
    bool inEvent = false;
    *events = 0;
    for (const auto &[name, value] : cardLines(data, 200000)) {
        if (name == QLatin1String("BEGIN") && value == QLatin1String("VEVENT")) {
            inEvent = true;
            summary.clear();
            place.clear();
            start = QDateTime();
        } else if (name == QLatin1String("END") && value == QLatin1String("VEVENT") && inEvent) {
            inEvent = false;
            ++*events;
            QString line = start.isValid() ? QLocale().toString(start.toLocalTime(), QLocale::ShortFormat) + QStringLiteral("  ") : QString();
            line += summary.isEmpty() ? tr("(no title)") : summary;
            if (!place.isEmpty())
                line += QStringLiteral(" (%1)").arg(place);
            lines << line;
        } else if (inEvent && name == QLatin1String("SUMMARY")) {
            summary = value;
        } else if (inEvent && name == QLatin1String("LOCATION")) {
            place = value;
        } else if (inEvent && name == QLatin1String("DTSTART")) {
            start = icsTime(value);
        }
    }
    return plainLines(lines.join(QLatin1Char('\n')), most);
}

QString contacts(const QByteArray &data, int most, int *cards)
{
    QStringList lines, now;
    QString name;
    *cards = 0;
    for (const auto &[key, value] : cardLines(data, 200000)) {
        if (key == QLatin1String("BEGIN")) {
            name.clear();
            now.clear();
        } else if (key == QLatin1String("END")) {
            ++*cards;
            lines << (name.isEmpty() ? tr("(no name)") : name) + (now.isEmpty() ? QString() : QStringLiteral(" · ") + now.join(QStringLiteral(" · ")));
        } else if (key == QLatin1String("FN")) {
            name = value;
        } else if (key == QLatin1String("N") && name.isEmpty()) {
            name = (value.section(QLatin1Char(';'), 1, 1) + QLatin1Char(' ') + value.section(QLatin1Char(';'), 0, 0)).trimmed();
        } else if ((key == QLatin1String("TEL") || key == QLatin1String("EMAIL") || key == QLatin1String("ORG")) && !value.isEmpty()) {
            now << value.section(QLatin1Char(';'), 0, 0);
        }
    }
    return plainLines(lines.join(QLatin1Char('\n')), most);
}

} // namespace

QString fromHtml(QString html)
{
    static const QRegularExpression hidden(QStringLiteral("<(script|style|head|title)[^>]*>.*?</\\1\\s*>"),
                                           QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression comments(QStringLiteral("<!--.*?-->"), QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression breaks(QStringLiteral("<(br|/p|/div|/tr|/li|/h\\d|/title)[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression cells(QStringLiteral("</t[dh]\\s*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
    static const QRegularExpression numbered(QStringLiteral("&#(x[0-9a-fA-F]{1,6}|[0-9]{1,7});"));
    static const QRegularExpression spaces(QStringLiteral("[ \t]+"));
    html.remove(comments).remove(hidden);
    html.replace(QLatin1Char('\n'), QLatin1Char(' ')).replace(QLatin1Char('\r'), QLatin1Char(' '));
    html.replace(breaks, QStringLiteral("\n")).replace(cells, QStringLiteral("\t")).remove(tags);
    QString out;
    qsizetype at = 0;
    for (auto it = numbered.globalMatch(html); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        out += html.mid(at, m.capturedStart() - at);
        const QString n = m.captured(1);
        const uint code = n.startsWith(QLatin1Char('x')) ? n.mid(1).toUInt(nullptr, 16) : n.toUInt();
        if (code > 0 && code < 0x110000)
            out += QString::fromUcs4(reinterpret_cast<const char32_t *>(&code), 1);
        at = m.capturedEnd();
    }
    out += html.mid(at);
    out.replace(QLatin1String("&nbsp;"), QLatin1String(" ")).replace(QLatin1String("&lt;"), QLatin1String("<")).replace(QLatin1String("&gt;"), QLatin1String(">"));
    out.replace(QLatin1String("&quot;"), QLatin1String("\"")).replace(QLatin1String("&apos;"), QLatin1String("'")).replace(QLatin1String("&amp;"), QLatin1String("&"));
    QStringList lines;
    for (const QString &line : out.split(QLatin1Char('\n')))
        lines << QString(line).replace(spaces, QStringLiteral(" ")).trimmed();
    return plainLines(lines.join(QLatin1Char('\n')), INT_MAX);
}

QString text(int type, const QByteArray &data, int most)
{
    const QString id = idOf(type);
    if (id == QLatin1String("eml"))
        return mailText(readMail(data), most);
    if (id == QLatin1String("mbox")) {
        const QVector<QByteArray> messages = mailbox(data, 100000);
        QStringList lines;
        for (const QByteArray &message : messages) {
            const qsizetype headerEnd = message.indexOf("\n\n");
            const Mail m = readMail(headerEnd < 0 ? message : message.left(headerEnd + 2)); // the headers are enough
            lines << QStringLiteral("%1 · %2 · %3").arg(m.sent.isValid() ? QLocale().toString(m.sent.toLocalTime(), QLocale::ShortFormat) : QStringLiteral("?"),
                                                        m.from.isEmpty() ? QStringLiteral("?") : m.from, m.subject.isEmpty() ? tr("(no subject)") : m.subject);
            if (lines.join(QLatin1Char('\n')).size() > most)
                break;
        }
        return plainLines(lines.join(QLatin1Char('\n')), most);
    }
    if (id == QLatin1String("rtf"))
        return rtf(data, most, nullptr);
    if (id == QLatin1String("html"))
        return fromHtml(decode(data, "utf-8")).left(most);
    if (id == QLatin1String("xml") || id == QLatin1String("svg"))
        return decode(data.left(qsizetype(most) * 4), "utf-8").left(most);
    if (id == QLatin1String("ics")) {
        int events = 0;
        return calendar(data, most, &events);
    }
    if (id == QLatin1String("vcf")) {
        int cards = 0;
        return contacts(data, most, &cards);
    }
    return {};
}

Heading heading(int type, const QByteArray &data)
{
    const QString id = idOf(type);
    Heading out;
    if (id == QLatin1String("eml")) {
        const Mail m = readMail(data);
        out.title = m.subject;
        out.when = m.sent;
    } else if (id == QLatin1String("rtf")) {
        RtfInfo info;
        rtf(data, 1, &info);
        out.title = info.title;
        out.when = info.created;
    } else if (id == QLatin1String("html")) {
        static const QRegularExpression title(QStringLiteral("<title[^>]*>(.*?)</title\\s*>"),
                                              QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
        out.title = fromHtml(title.match(decode(data.left(256 * 1024), "utf-8")).captured(1)).simplified();
    }
    return out;
}

QStringList properties(int type, const QByteArray &data)
{
    const QString id = idOf(type);
    QStringList out;
    const auto add = [&out](const QString &label, const QString &value) {
        if (!value.trimmed().isEmpty())
            out << label + QLatin1Char('\t') + value.trimmed().left(200);
    };
    if (id == QLatin1String("eml")) {
        const Mail m = readMail(data);
        add(tr("From"), m.from);
        add(tr("To"), m.to);
        add(tr("Cc"), m.cc);
        add(tr("Subject"), m.subject);
        add(tr("Sent"), when(m.sent));
        if (!m.attached.isEmpty())
            add(tr("Attached"), m.attached.join(QStringLiteral(", ")));
    } else if (id == QLatin1String("mbox")) {
        add(tr("Messages"), QString::number(mailbox(data, 10000000).size()));
    } else if (id == QLatin1String("rtf")) {
        RtfInfo info;
        rtf(data, 1, &info);
        add(tr("Title"), info.title);
        add(tr("Subject"), info.subject);
        add(tr("Author"), info.author);
        add(tr("Last changed by"), info.lastAuthor);
        add(tr("Company"), info.company);
        add(tr("Created"), when(info.created));
        add(tr("Changed"), when(info.changed));
    } else if (id == QLatin1String("html")) {
        static const QRegularExpression title(QStringLiteral("<title[^>]*>(.*?)</title\\s*>"),
                                              QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
        add(tr("Title"), fromHtml(title.match(decode(data.left(256 * 1024), "utf-8")).captured(1)).simplified());
    } else if (id == QLatin1String("ics")) {
        int count = 0;
        calendar(data, 1, &count);
        add(tr("Events"), QString::number(count));
    } else if (id == QLatin1String("vcf")) {
        int count = 0;
        contacts(data, 1, &count);
        add(tr("Contacts"), QString::number(count));
    }
    return out;
}

} // namespace texts
