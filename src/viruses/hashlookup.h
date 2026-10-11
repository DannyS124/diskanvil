// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Scan for Viruses' online check: whether CIRCL's hashlookup service (Luxembourg's computer
// emergency response team) knows a file, by its SHA-256 alone. It knows tens of millions of
// files from software makers and Linux distributions, so a program it knows is very likely
// what it says it is. Free, no account; only ever asked after the person says yes.

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>

namespace hashlookup {

struct Answer {
    QString sha256;   // lower-case hex
    bool known = false;
    bool failed = false;
    QString what;     // for a known file: "notepad.exe, part of Windows 11 (NSRL)"
};

// Where the service is; the tests use one of their own.
extern QString server;

// The words for a known file, from the service's answer.
QString describe(const QByteArray &json);

class Lookup : public QObject
{
    Q_OBJECT
public:
    explicit Lookup(QObject *parent = nullptr);
    void ask(const QStringList &sha256);

Q_SIGNALS:
    void answered(const hashlookup::Answer &answer);
    void finished();

private:
    QNetworkAccessManager m_network;
    int m_waiting = 0;
};

} // namespace hashlookup

Q_DECLARE_METATYPE(hashlookup::Answer)
