// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The words in found files that are text underneath: emails (.eml, MIME, RFC 5322) and
// mailboxes (mbox), RTF documents, web pages, calendars (.ics) and contacts (.vcf). For Find
// Lost Files' previews and viewer: an email with its headers decoded and its readable part, an
// RTF's words without its markup, a calendar's events, the contacts' names, numbers and
// addresses. Found files can hold anything, so every walk has a limit.

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>

namespace texts {

// The words of a web page without its markup (scripts, styles and its head left out).
QString fromHtml(QString html);
// The words of an email, mailbox, RTF document, web page, calendar or contacts file (`type` is a
// filetypes number), up to `most` characters. Empty for other kinds.
QString text(int type, const QByteArray &data, int most = 4000);
// What it says about itself, a "label\tvalue" per line: an email's sender, recipients, subject,
// date and attachments; how many messages, events or contacts; a document's title and author.
QStringList properties(int type, const QByteArray &data);

// An email's subject and when it was sent; a web page's or RTF document's title and date.
struct Heading {
    QString title;
    QDateTime when;
};
Heading heading(int type, const QByteArray &data);

} // namespace texts
