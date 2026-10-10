// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Scan for Viruses, its first layer: files, checked against an antivirus's list of known
// malware. ClamAV everywhere it's installed, run as its own program (clamscan; libclamav is
// GPL-2.0-only and can't be linked into GPL-3 DiskForge). On Windows without ClamAV, Microsoft
// Defender's own command-line scanner, which every Windows has and keeps up to date itself.
//
// ClamAV's list of signatures lives where DiskForge keeps it: on DiskForge Live's stick, in
// ~/.local/share/diskforge/clamav on a Linux PC (or the system's own, when that's up to date),
// and in ClamAV's own folder on Windows. The first download is about 250 MB, after that only
// the day's changes.

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>

namespace virus {

struct Engine {
    enum class Kind { None, ClamAV, Defender };
    Kind kind = Kind::None;
    QString scanner;     // clamscan, or MpCmdRun.exe
    QString updater;     // freshclam (ClamAV only)
    QString database;    // the folder with ClamAV's list; empty when there's none yet
    bool databaseWritable = false;
    QDateTime listDate;  // when the list was made
    qint64 signatures = 0;
    QString name() const;
};

// What this system has to scan with, and its list's age. On DiskForge Live (`live`) the list is
// used from memory, kLiveList, where the dialog has it brought from the stick first.
Engine findEngine(bool live = false);
inline const char *kLiveList = "/run/diskforge-live/clamav";

// The date and signature count in the header of a ClamAV list (daily.cvd or .cld), or an
// invalid date.
QDateTime listDate(const QByteArray &header, qint64 *signatures = nullptr);

struct Detection {
    QString path;
    QString name; // what the antivirus calls it: Win.Trojan.Agent-123456
};

// Which files to look at: everything, or only the kinds malware comes as (programs, libraries,
// scripts, documents with macros, archives, shortcuts).
enum class Depth { Quick, Everything };
bool quickScanWants(const QString &fileName);

// Scans folders: where a drive is mounted (a Btrfs drive can be in several places), or any
// folder. Lists the files first to know how many there are, then hands them to the scanner in
// one go and follows its output.
class Scan : public QObject
{
    Q_OBJECT
public:
    Scan(Engine engine, QStringList folders, Depth depth);
    void cancel() { m_cancel = true; }
    // Files scanned whatever the depth: the programs that start with Windows.
    void alsoScan(const QStringList &files) { m_also = files; }

    // What's left out: the paging, swap and hibernation files (gigabytes of memory contents),
    // and folders of copies of what's scanned anyway (Windows' shadow copies, Btrfs snapshots).
    static bool skipped(const QString &path);

public Q_SLOTS:
    void run();

Q_SIGNALS:
    void phase(const QString &text);
    void progress(quint64 done, quint64 total);
    void found(const virus::Detection &detection);
    // unreadable: files the scanner couldn't open (another user's, the system's, in use)
    void finished(bool completed, int scanned, int unreadable, const QString &problem);

private:
    qint64 listFiles(QIODevice &list);
    void runClamAv(const QString &list, qint64 total);
    void runDefender();

    Engine m_engine;
    QStringList m_folders;
    QStringList m_also;
    Depth m_depth;
    std::atomic<bool> m_cancel{false};
};

// Gets the newest list with freshclam: everything the first time, the changes after that.
class Update : public QObject
{
    Q_OBJECT
public:
    Update(Engine engine, QString into);
    void cancel() { m_cancel = true; }

public Q_SLOTS:
    void run();

Q_SIGNALS:
    void progress(const QString &line);
    void finished(bool ok, const QString &message);

private:
    Engine m_engine;
    QString m_into;
    std::atomic<bool> m_cancel{false};
};

// What an antivirus's name for a find means, in plain words ("a Trojan: a harmful program
// that pretends to be something else"), and how much it matters: 0 a test file or an unwanted
// program, 1 a guess from how a file is built, 2 malware.
QString describe(const QString &name);
int severity(const QString &name);
// The name as people see it: without ClamAV's ".UNOFFICIAL" on the end.
QString shownName(const QString &name);

// clamscan's lines about a file: "<path>: OK", "<path>: <name> FOUND", or why it couldn't be
// read ("<path>: Access denied"). A path can have ": " in it, so it's whatever comes before the
// known ending. Sets `detection` for a file (its name only for a hit).
enum class ClamLine { Other, Clean, Found, Unreadable };
ClamLine parseClamLine(const QString &line, Detection &detection);
// Microsoft Defender's report, after a scan.
QVector<Detection> parseDefender(const QString &output);

} // namespace virus

Q_DECLARE_METATYPE(virus::Detection)
