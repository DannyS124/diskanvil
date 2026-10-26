// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lostsource.h"
#include "unpack.h"
#include "vshadow.h"

#include <QObject>
#include <QSet>
#include <QVector>

#include <atomic>
#include <memory>

// Find Lost Files: what it found, how it looks for it, and how it saves it.
namespace lost {

enum class Condition : quint8 {
    Good,         // all there, as far as can be told
    MaybeDamaged, // its structure breaks off or doesn't add up: it may open with parts missing
    Overwritten,  // its space has been used for something else since
    Unreadable,   // part of it is on spots the drive couldn't read; those parts are blank
};

enum class Origin : quint8 {
    FileSystem, // the file system still lists it: name and folder are known
    Contents,   // found by what's inside it: the name is made up
};

struct Found {
    QString name;
    QString folder; // where it was, when the file system says
    int type = -1;  // filetypes number, -1 when it's not one of those
    quint64 size = 0;
    QVector<Extent> extents; // where its bytes are, in order
    Condition condition = Condition::Good;
    Origin origin = Origin::Contents;
    bool deleted = true;
    qint64 modified = 0; // ms since 1970, 0 when not known
    qint64 created = 0;  // the same way, when the file system keeps it
    qint64 accessed = 0; // last opened (FAT keeps only the day: accessedDayOnly)
    qint64 changed = 0;  // NTFS: when its record last changed (renamed, moved, deleted...)
    bool accessedDayOnly = false;
    qint64 deletedAt = 0; // when it was deleted, the same way (NTFS's change journal, the Recycle Bin)
    qint64 inside = 0;    // the date it carries itself (a photo's, an email's...), for one found by its contents
    bool nameOnly = false; // only its name and folder are left: nothing to save
    bool nameGuessed = false;   // FAT lost the first letter, and nothing said what it was
    bool piecesGuessed = false; // where its bytes are is worked out, not known
    bool checked = false;       // its contents were checked, start to end
    QByteArray data;            // a small file kept in the file system's own records (NTFS): its bytes
    QString note;               // anything else worth saying about it, in plain words
    std::shared_ptr<Source> from; // where its bytes are, when not on what's looked through: an earlier copy of it
    // Windows keeps it compressed: `extents` (or `data`, for a WOF stream small enough to be in its
    // record) are its bytes as they're kept, and this says how to unpack them. FileReader does.
    unpack::Packing packing;
};

// A found file's bytes, read from where they are on the source.
class FileReader
{
public:
    FileReader(std::shared_ptr<Source> source, const Found &file);
    quint64 size() const { return m_size; }
    qint64 read(quint64 at, char *buf, qint64 len);
    QByteArray head(qint64 len); // the first len bytes (fewer for a smaller file)

private:
    std::shared_ptr<Source> m_source;
    QVector<Extent> m_extents;
    QByteArray m_data;
    quint64 m_size;
};

// A file system Find Lost Files read itself.
// What an NTFS volume's MFT records hold now, by record number: enough to tell whether a file
// in one of its earlier copies is the same as today's.
struct RecordNow {
    qint64 written = 0; // last written, and last changed in any way (NTFS's own times)
    qint64 changed = 0;
    quint64 size = 0;
    quint16 sequence = 0;
    bool inUse = false;
    bool known = false;
};
using RecordIndex = QVector<RecordNow>;

struct Volume {
    QString type;  // "vfat", "exfat" or "ntfs"
    QString label; // or its type and place, when it has none
    Extent where;
    int files = 0;
    int deleted = 0;
    // FAT and exFAT: how big a cluster is and where cluster 2 is on the source (to read the
    // folders a quick format left behind).
    quint64 clusterSize = 0;
    quint64 heap = 0;
    // NTFS: Windows' earlier copies of it (shadow copies), and what its records hold now.
    QVector<vss::Copy> copies;
    QString copiesProblem;
    std::shared_ptr<const RecordIndex> records;
};

// Lists what the file systems on the source know about, deleted files and folders included,
// with their names: FAT and exFAT so far (lostfs.h). Quick, since it only reads their tables
// and folders. Then it checks what's in each deleted file, so it can say which still look whole.
class QuickScan : public QObject
{
    Q_OBJECT
public:
    explicit QuickScan(std::shared_ptr<Source> source);
    void cancel() { m_cancel = true; }

    // After it's finished: the file systems it read, and where the files it listed are, so
    // the deep scan doesn't list them a second time.
    QVector<Volume> volumes() const { return m_volumes; }
    QVector<Extent> knownRanges() const { return m_ranges; }
    QVector<quint64> knownStarts() const { return m_starts; }

    // Deleted files up to this many bytes in all get their whole structure checked; past
    // that only their start is (it reads every byte of them).
    static constexpr quint64 kFullCheckBudget = 2ull * 1024 * 1024 * 1024;

public Q_SLOTS:
    void run();

Q_SIGNALS:
    void found(const QVector<lost::Found> &files);
    void progress(quint64 done, quint64 total);
    void finished(bool stopped);

private:
    void addRanges(const Found &file);

    std::shared_ptr<Source> m_source;
    std::atomic<bool> m_cancel{false};
    QVector<Volume> m_volumes;
    QVector<Extent> m_ranges;
    QVector<quint64> m_starts;
};

// The files in Windows' earlier copies of the volumes a quick scan found that aren't the same on
// the drive now: changed since, or gone. The newest copy first; each file is read from its copy.
class CopyScan : public QObject
{
    Q_OBJECT
public:
    CopyScan(std::shared_ptr<Source> source, QVector<Volume> volumes);
    void cancel() { m_cancel = true; }

public Q_SLOTS:
    void run();

Q_SIGNALS:
    void found(const QVector<lost::Found> &files);
    void progress(int done, int total, qint64 copyTaken);
    void finished(bool stopped, int listed, const QString &problem);

private:
    std::shared_ptr<Source> m_source;
    QVector<Volume> m_volumes;
    std::atomic<bool> m_cancel{false};
};

// Looks through the source for files by their contents (carving): every sector is checked for
// the start of a file it knows, and each start is followed to that file's end. Reads the
// source once, start to end; slow on a big drive, but it finds files no file system remembers.
class DeepScan : public QObject
{
    Q_OBJECT
public:
    explicit DeepScan(std::shared_ptr<Source> source, quint64 start = 0, quint64 end = 0);
    void cancel() { m_cancel = true; }
    // What the quick scan listed already: nothing starting at one of `starts` is listed again,
    // and nothing inside `ranges` is looked at (files that are there for sure).
    void skip(QVector<Extent> ranges, QVector<quint64> starts);
    // The file systems the quick scan read: their cluster layout, for the folders a quick format
    // left behind (they're listed at the end, with their files' names).
    void setVolumes(QVector<Volume> volumes) { m_volumes = std::move(volumes); }

    static constexpr int kPerTypeLimit = 250000; // past that, more of one type isn't listed
    static constexpr int kTotalLimit = 1000000;  // and past this, nothing more at all
    static constexpr int kLeftoverFolderLimit = 200000;

public Q_SLOTS:
    void run();

Q_SIGNALS:
    void found(const QVector<lost::Found> &files); // a batch every quarter second or so
    void progress(quint64 done, quint64 total);
    void finished(bool stopped);

private:
    std::shared_ptr<Source> m_source;
    quint64 m_start, m_end;
    std::atomic<bool> m_cancel{false};
    QVector<Extent> m_skipRanges; // sorted, not overlapping
    QSet<quint64> m_skipStarts;
    QVector<Volume> m_volumes;
};

// Finds the files that are the same as another found one, byte for byte (the same photo found
// in a folder and again by its contents, or kept twice): files of one size are compared by the
// SHA-256 of their start, then of all of them. Each copy is reported with the one it's a copy
// of, the first of them in the list.
class CopyFinder : public QObject
{
    Q_OBJECT
public:
    // `indexes`: each file's place in the list, reported back.
    CopyFinder(std::shared_ptr<Source> source, QVector<Found> files, QVector<int> indexes);
    void cancel() { m_cancel = true; }

public Q_SLOTS:
    void run();

Q_SIGNALS:
    void progress(quint64 done, quint64 total); // bytes read
    void finished(const QHash<int, int> &copyOf, quint64 copyBytes, bool stopped);

private:
    std::shared_ptr<Source> m_source;
    QVector<Found> m_files;
    QVector<int> m_indexes;
    std::atomic<bool> m_cancel{false};
};

// Looks through the source for words someone remembers from a lost document or email, kept as
// text (UTF-8, UTF-16 as Windows and Word keep it, or Windows' western code page; capitals don't
// matter): where all of them are close together, the text around them is found, as text to look
// at and save. Packed files (Word 2007 and later, zipped) don't keep their words as text, so
// they're found by their kind instead; Word's older files, its AutoRecover files, emails and the
// page file do.
class WordSearch : public QObject
{
    Q_OBJECT
public:
    // `known`: the files found already, so text inside one of them says so.
    WordSearch(std::shared_ptr<Source> source, QStringList words, QVector<Found> known);
    void cancel() { m_cancel = true; }

public Q_SLOTS:
    void run();

Q_SIGNALS:
    void found(const QVector<lost::Found> &files);
    void progress(quint64 done, quint64 total);
    void finished(bool stopped, int places);

private:
    static constexpr quint64 kChunk = 16 * 1024 * 1024;
    std::shared_ptr<Source> m_source;
    QStringList m_words;
    QVector<Found> m_known;
    std::atomic<bool> m_cancel{false};
};

// Saves found files into `folder`/Recovered <date and time>/, never over anything that's there,
// keeping going past files that fail. Writes "What was saved.txt" next to them.
class Saver : public QObject
{
    Q_OBJECT
public:
    Saver(std::shared_ptr<Source> source, QVector<Found> files, QString folder);
    void cancel() { m_cancel = true; }

public Q_SLOTS:
    void run();

Q_SIGNALS:
    void progress(quint64 done, quint64 total);
    // position: where the file was in the list it was given.
    void fileSaved(int position, const QString &path);
    void finished(int saved, int failed, const QString &folder, const QString &message);

private:
    std::shared_ptr<Source> m_source;
    QVector<Found> m_files;
    QString m_folder;
    std::atomic<bool> m_cancel{false};
};

// A file name any file system takes (FAT and NTFS included), from whatever a drive said.
QString cleanName(const QString &name);
// The part of a file's extents from byte `from` of it, `length` long.
QVector<Extent> slice(const QVector<Extent> &extents, quint64 from, quint64 length);
// The small pictures in one of Windows' thumbnail caches (thumbcache.h) found on the source, as
// files of their own (pointing into the cache's bytes where they're in one piece there).
QVector<Found> thumbnailsIn(const std::shared_ptr<Source> &source, const Found &cache);

// The made-up name for a file found by its contents: "Picture 000042.jpg".
QString carvedName(int type, int number);
// A better one, from what's inside it (naming.h), and the date it carries; the made-up one stays
// when nothing inside says. Reads a little of the file.
void nameFromContents(Found &file, Source &source);

} // namespace lost
