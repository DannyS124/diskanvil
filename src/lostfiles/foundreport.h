// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What Find Lost Files found, as a report to keep or hand on: each file with what's known of it
// (where it was, its times, whether it looks whole, where a photo or film was taken) and, when
// asked for, its fingerprints (MD5, SHA-1 and SHA-256: the same file always gives the same ones,
// so they show later that a copy is unchanged). A web page anyone can open, or CSV for a
// spreadsheet.

#include "lostscan.h"
#include "naming.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>

namespace foundreport {

struct Fingerprints {
    QByteArray md5, sha1, sha256; // in hex
    bool done = false;
};
// The file's three fingerprints, read whole (false `done` when it was stopped).
Fingerprints fingerprints(const std::shared_ptr<lost::Source> &source, const lost::Found &file, const std::function<bool()> &stop = {});

struct Row {
    int index = -1; // in the files
    Fingerprints prints;
    naming::Spot spot;
    QString place; // the spot in words
};

// Reads what's needed for `indexes` of `files`: where pictures and films were taken, and when
// `prints`, every file's fingerprints. Run it on a worker thread; `progress` is told how far it
// is, and returning false from it stops it (what's done so far is returned).
using Progress = std::function<bool(int done, int total)>;
QVector<Row> collect(const std::shared_ptr<lost::Source> &source, const QVector<lost::Found> &files, const QVector<int> &indexes, bool prints,
                     const Progress &progress = {});

struct About {
    QString title;  // what was looked through: the drive, partition or image
    QString source; // its device or file
    QDateTime made;
    QString version; // DiskAnvil's
};
QString html(const About &about, const QVector<lost::Found> &files, const QVector<Row> &rows);
QByteArray csv(const QVector<lost::Found> &files, const QVector<Row> &rows);

} // namespace foundreport
