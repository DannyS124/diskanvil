// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QObject>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

// Copies a tree of files onto a mounted USB stick and checks every one of them: each file is
// hashed while it's copied, then flushed, dropped from the page cache and read back from the
// stick. Make a DiskForge Live USB, the file copy mode of Write Image to USB and Make a Windows USB all
// go through this.
namespace filecopy {

struct Entry {
    QString path;        // "live/vmlinuz": no leading slash, '/' between folders
    bool isDir = false;
    quint64 size = 0;
};

// Where the files come from.
class Source
{
public:
    virtual ~Source() = default;
    // Empty when it opened fine.
    virtual QString error() const = 0;
    // Folders come before what's in them.
    virtual const QVector<Entry> &entries() const = 0;
    // The volume name (an ISO's volume ID), if there is one.
    virtual QString label() const { return {}; }
    // Reads exactly `length` bytes of `entry` from `offset`.
    virtual bool read(const Entry &entry, quint64 offset, char *buffer, qsizetype length) = 0;
};

// An ISO file, read with isofs (Joliet names), without mounting it.
std::unique_ptr<Source> openIso(const QString &isoPath);
// A folder: a mounted ISO (UDF, or Rock Ridge without Joliet), or a stick. Symbolic links are
// left out (FAT32 can't hold them, and some ISOs have one pointing at their own root).
std::unique_ptr<Source> openFolder(const QString &root);

struct Options {
    // SHA-256 (hex) for each path, from a sha256sum.txt. Checked while copying.
    QHash<QString, QByteArray> expected;
    // true: leave this file out.
    std::function<bool(const QString &path)> skip;
    // Changes a small file on the way (boot menus, see isomode). Returns the new contents, or a
    // null QByteArray to copy it as it is. Only asked for files under 1 MiB.
    std::function<QByteArray(const QString &path, const QByteArray &data)> transform;
    // Room to leave free on the stick, on top of the files.
    quint64 reserve = 16 * 1024 * 1024;
};

// Runs in a worker thread, or straight from another worker's run().
class Copier : public QObject
{
    Q_OBJECT
public:
    Copier(Source *source, const QString &target, Options options = {});
    void cancel() { m_cancel = true; }
    bool cancelled() const { return m_cancel; }
    int filesCopied() const { return m_files; }
    quint64 bytesCopied() const { return m_bytes; }

public slots:
    void run();

signals:
    void progress(const QString &phase, quint64 done, quint64 total);
    void finished(bool ok, const QString &message);

private:
    bool copy(const Entry &entry, quint64 &done, quint64 total);
    bool check(const QString &path, quint64 &done, quint64 total);
    void finish(bool ok, const QString &message);

    Source *m_source;
    QString m_root;
    Options m_options;
    QHash<QString, QByteArray> m_written; // SHA-256 of what went onto the stick
    QString m_error;
    int m_files = 0;
    quint64 m_bytes = 0;
    std::atomic<bool> m_cancel{false};
};

// "Stopped. The stick is only half done; make it again before using it."
QString stoppedMessage();

} // namespace filecopy
