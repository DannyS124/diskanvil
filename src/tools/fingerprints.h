// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Are My Files Still OK?: a fingerprint (SHA-256) of every file in a folder, kept, and checked
// again later. Photos and videos kept for years on a drive can go bad without anyone touching
// them (a failing drive, "bit rot", a bad copy): the file is still there, its date is the same,
// but what's in it changed. Checked against the fingerprints, each file is the same, damaged
// (changed though its date didn't: nobody saved it), edited (changed, with a newer date),
// missing, unreadable, or new since.
//
// The fingerprints are kept in the folder itself (so they go where the folder goes), as
// "DiskAnvil fingerprints.sha256": the format sha256sum reads (sha256sum -c checks it too), with
// when they were made in a comment at the top. When the folder can't be written to, they're
// kept in DiskAnvil's own folder instead.

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>

namespace fingerprints {

inline constexpr char kFileName[] = "DiskAnvil fingerprints.sha256";

struct Entry {
    QString path;    // relative to the folder, with "/"
    QByteArray sha;  // lower-case hex
};

struct Book {
    QDateTime made;
    QVector<Entry> files;
};

// Where a folder's fingerprints are: in it, or in DiskAnvil's own folder. When there are none
// yet, where they'd go (`exists` false).
QString bookFor(const QString &folder, bool *exists);
// The one in DiskAnvil's own folder (for one that can't be written to).
QString ownCopyFor(const QString &folder);

bool read(const QString &file, Book *book, QString *error);
QByteArray toText(const Book &book);
// From the text (the tests; read() uses it).
Book fromText(const QByteArray &text);

enum class State { Same, Damaged, Edited, Missing, Unreadable, New };
QString stateName(State state);

struct Result {
    QString path;
    State state = State::Same;
    QDateTime modified; // the file's own date now
    QString detail;     // why it couldn't be read
};

// Compares `now` (the folder as it is) with `then`.
struct Seen {
    QString path;
    QByteArray sha;      // empty when it couldn't be read
    QDateTime modified;
    QString error;
};
QVector<Result> compare(const Book &then, const QVector<Seen> &now);

// Records or checks a folder, on a worker thread.
class Job : public QObject
{
    Q_OBJECT
public:
    enum class Mode { Record, Check };
    Job(QString folder, Mode mode);
    void cancel() { m_cancel = true; }

public slots:
    void run();

signals:
    void progress(quint64 done, quint64 total, const QString &file);
    // Check: what's changed (Same ones included). Record: empty. `book`: where they're kept.
    void finished(bool ok, const QVector<fingerprints::Result> &results, const QString &book, const QString &error);

private:
    QString m_folder;
    Mode m_mode;
    std::atomic<bool> m_cancel{false};
};

} // namespace fingerprints

Q_DECLARE_METATYPE(fingerprints::Result)
