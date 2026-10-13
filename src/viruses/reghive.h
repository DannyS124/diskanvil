// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Windows' registry files ("hives": SYSTEM, SOFTWARE, a user's NTUSER.DAT), read with our own
// code, for a Windows that isn't running: the same on DiskAnvil Live and in the Windows version.
//
// Windows 8.1 and later write changes to a hive's logs (.LOG1 and .LOG2) first, and into the
// hive itself much later. A hive taken from a PC that was shut down with Fast Startup, or
// crashed, or is still running, can be far behind: a Run key malware added an hour ago may be
// only in the log. So the newest log's changes are put back in memory when the hive is read
// (never written to the drive).
//
// Changes (switching a startup entry off) are written in place, and only to a hive that's up
// to date on its own: one Windows closed properly.

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace reg {

enum Type : quint32 {
    None = 0,
    Sz = 1,
    ExpandSz = 2,
    Binary = 3,
    Dword = 4,
    DwordBigEndian = 5,
    Link = 6,
    MultiSz = 7,
    Qword = 11,
};

struct Value {
    QString name; // empty: the key's default value
    quint32 type = None;
    QByteArray data;
    quint32 cell = 0; // its record in the hive

    // Strings as they are, numbers in decimal, anything else in hex.
    QString text() const;
    QStringList strings() const; // a REG_MULTI_SZ's strings; one for the other kinds
    quint64 number(bool *ok = nullptr) const;
};

// A key: where its record is in the hive. 0 is no key.
using Key = quint32;

class Hive
{
public:
    // Reads the hive file and the logs next to it (<name>.LOG1 and .LOG2, any case).
    bool open(const QString &path, QString *error);
    // The same from memory (the tests): the hive's bytes and its logs'.
    bool load(QByteArray hive, const QVector<QByteArray> &logs, QString *error);

    // Whether changes from the logs were put back, and how many.
    int recoveredFrom() const { return m_recovered; }
    // The hive wasn't closed properly and its logs couldn't bring it up to date (they're
    // missing or damaged): it's read as it is, and may be out of date.
    bool maybeBehind() const { return m_behind; }
    // Changes can be written: Windows closed this hive properly and nothing came from the logs.
    bool canWrite() const { return (m_clean && m_recovered == 0) || m_whole; }
    // A hive the logs brought up to date can still be changed, when it's then written out whole
    // with saveWhole() (its old logs set aside: they'd be put back onto it). False when it's behind.
    bool changeWhole();

    Key root() const { return m_root; }
    // A key under `from` (the root if 0), by its path: names with "\" between them, any case.
    Key key(const QString &path, Key from = 0) const;
    QVector<Key> subkeys(Key key) const;
    QString name(Key key) const;
    QDateTime written(Key key) const;
    QVector<Value> values(Key key) const;
    std::optional<Value> value(Key key, const QString &name) const;

    // Changes in memory, written with save(). Only on a hive that canWrite().
    bool setDword(Key key, const QString &name, quint32 number, QString *error);
    // A string, when the new one fits where the old one was (switching an entry back to
    // Windows' own, which is shorter).
    bool setString(Key key, const QString &name, const QString &text, QString *error);
    // The same for a list of strings (REG_MULTI_SZ): one taken out always fits.
    bool setStrings(Key key, const QString &name, const QStringList &strings, QString *error);
    bool removeValue(Key key, const QString &name, QString *error);
    // Writes the changed parts back to `path`, the file it was opened from.
    bool save(const QString &path, QString *error);
    // The whole hive as it is now, up to date and closed properly, to a new file at `path`.
    bool saveWhole(const QString &path, QString *error) const;
    const QByteArray &bytes() const { return m_data; } // the whole hive as it is now (the tests)

    // Damaged structures met while reading (a broken list, a record out of bounds...).
    int problems() const { return m_problems; }

private:
    struct Cell {
        const char *data = nullptr;
        quint32 size = 0;
    };
    Cell cell(quint32 offset) const;
    bool isKey(quint32 offset) const;
    void collectSubkeys(quint32 list, QVector<Key> &out, int depth) const;
    std::optional<Value> readValue(quint32 offset) const;
    QByteArray valueData(quint32 size, quint32 offset) const;
    bool applyLogs(const QVector<QByteArray> &logs);
    bool valueRecord(Key key, const QString &name, quint32 *listCell, int *index, quint32 *record) const;
    bool setText(Key key, const QString &name, const QString &text, quint32 wantedType, QString *error); // 0: REG_SZ or EXPAND_SZ
    void markChanged(quint32 offset, quint32 size);

    QByteArray m_data; // the base block, then the hive bins
    quint32 m_binsSize = 0;
    quint32 m_minor = 0;
    Key m_root = 0;
    bool m_clean = false;
    bool m_behind = false;
    bool m_whole = false;
    int m_recovered = 0;
    mutable int m_problems = 0;
    QVector<QPair<quint32, quint32>> m_changed; // file offset, size
};

// Marvin32, the hash Windows checks log entries with (published with .NET's source).
quint64 marvin32(const char *data, qsizetype size, quint64 seed = 0x82EF4D887A4E55C5ull);

} // namespace reg
