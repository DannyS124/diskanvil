// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Windows Won't Start: two of the usual reasons, fixed from outside it (DiskAnvil Live, or the
// Windows version on another Windows' drive).
//
// A stuck update: "Undoing changes", "Preparing Windows" or "Working on updates" for ever.
// Windows lists what it has still to do at the next start in WinSxS\pending.xml, and points
// at that list from its COMPONENTS registry file. Microsoft's own way out is
// DISM /RevertPendingActions; without DISM it's the same by hand: the list set aside (kept,
// renamed) and the pointers to it taken out of COMPONENTS, the file backed up first.
//
// A damaged registry file ("the registry file is missing or contains errors", 0xc0000098,
// 0xc000014c, a blue screen saying REGISTRY_ERROR): Windows' registry files (SYSTEM, SOFTWARE,
// SAM, SECURITY, DEFAULT) each checked, and one put back from an earlier day: from RegBack,
// where Windows kept copies until 2018 (and still does when it's told to), or from a shadow
// copy (System Restore). The damaged one is kept, renamed, never deleted.

#include <QByteArray>
#include <QHash>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

namespace lost {
class Source;
}

namespace winstart {

// A file under a Windows drive mounted at `root`, any case: "Windows/WinSxS/pending.xml".
// Empty when it isn't there.
QString find(const QString &root, const QString &path);

struct StuckUpdate {
    bool stuck = false;        // there's a list of things to do at the next start
    QString pendingFile;       // the list (WinSxS\pending.xml), where it is here
    QDateTime pendingWritten;  // when it was made
    qint64 pendingSize = 0;
    int actions = 0;           // how many things it lists
    QStringList packages;      // updates waiting (from SOFTWARE's Component Based Servicing)
    QStringList pointers;      // what COMPONENTS has pointing at the list
    QString componentsFile;
    QString problem;           // why it can't be undone here (the drive's read-only...)
};
StuckUpdate findStuckUpdate(const QString &root);

// Undoes it our way: pending.xml renamed to pending.xml.before-diskanvil-<when>, COMPONENTS
// copied to COMPONENTS.before-diskanvil-<when> and the pointers taken out. `done` says each step.
bool undoStuckUpdate(const QString &root, QStringList *done, QString *error);

// What the Windows version runs for it: Microsoft's own DISM, on that Windows ("D:\").
QStringList dismArguments(const QString &windowsDrive);

// Windows' registry files, checked.
enum class HiveState { Fine, UpToDateFromLogs, Behind, Damaged, Missing };
struct HiveCheck {
    QString name;      // "SYSTEM"
    QString file;      // where it is here; empty when missing
    HiveState state = HiveState::Missing;
    QString problem;   // in plain words, when it isn't fine
    int keys = 0;
};
inline const QStringList kHives = {QStringLiteral("SYSTEM"), QStringLiteral("SOFTWARE"), QStringLiteral("SAM"),
                                   QStringLiteral("SECURITY"), QStringLiteral("DEFAULT")};
HiveCheck checkHive(const QString &file, const QString &name, const std::atomic<bool> *cancel = nullptr);
// The same for bytes (an earlier copy).
HiveCheck checkHiveBytes(const QByteArray &bytes, const QString &name);
QVector<HiveCheck> checkHives(const QString &root, const std::atomic<bool> *cancel = nullptr);

// An earlier copy of a registry file.
struct EarlierCopy {
    QString hive;          // "SYSTEM"
    QString from;          // "RegBack", or the shadow copy's date in words
    QDateTime when;        // when it was made, as near as is known
    QString file;          // RegBack: where it is; empty for one read from a shadow copy
    std::function<QByteArray(QString *why)> read; // its bytes
};
// RegBack's copies (empty ones, the way Windows leaves them now, aren't copies).
QVector<EarlierCopy> regBackCopies(const QString &root);

// A file's record in NTFS's table (its inode number on Linux, its file index on Windows): how
// it's found again in a shadow copy. 0 when it can't be had.
quint64 ntfsRecord(const QString &file);
// The registry files' copies in Windows' shadow copies of its drive, newest first. `volume`: the
// Windows partition, read-only; `records`: each hive's record ("SYSTEM" → ntfsRecord()).
QVector<EarlierCopy> shadowCopies(const std::shared_ptr<lost::Source> &volume, const QHash<QString, quint64> &records, QString *problem);

// Puts `bytes` back as that hive: the file there now (and its .LOG1/.LOG2, which belong to it)
// renamed to <name>.before-diskanvil-<when>. Refused when the bytes aren't a whole registry file.
bool putBack(const QString &root, const QString &hive, const QByteArray &bytes, QStringList *done, QString *error);

// The stamp put on what's set aside: "2026-10-13-0904".
QString stamp();

} // namespace winstart
