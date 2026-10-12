// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What was erased, and how: a record of each Wipe Disk and Secure Erase that finished, kept so a
// certificate of erasure can be made for it (for a customer, or the next owner).

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace erasure {

struct Record {
    QString id;        // stays the same for this erasure
    QString model, serial, device, bus;
    quint64 size = 0;
    QString method;    // in plain words: "Every sector written with zeros (Wipe Disk)"
    bool zeros = false; // a wipe with zeros: its result can be checked by reading it back
    QDateTime started, finished;
    QString checked;   // what reading it back found, when it was checked
    QString app;       // DiskAnvil and its version

    QJsonObject toJson() const;
    static Record fromJson(const QJsonObject &json);
};

// Where they're kept: a file of DiskAnvil's own (the tests put it elsewhere).
extern QString storePath;
void remember(const Record &record);
QVector<Record> all(); // newest first
void update(const Record &record); // the same id, changed (checked)

// The certificate, as a page of HTML (made into a PDF): the drive, the method, the times, and who
// did it for whom.
QString certificateHtml(const Record &record, const QString &erasedBy, const QString &forWhom, const QString &notes);

} // namespace erasure
