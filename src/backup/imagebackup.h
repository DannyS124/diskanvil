// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Backs up a drive or partition to a compressed image (name.img.zst) with a small
// description next to it (name.img.zst.json), and restores one. The description holds
// checksums, so a restore can check the whole backup before writing a single byte.

#include "udisks.h"

#include <QDateTime>
#include <QJsonObject>
#include <QObject>

#include <atomic>

struct BackupInfo {
    QString kind;        // "disk" or "partition"
    QString device;      // as it was when backed up, e.g. /dev/sdb1
    QString model;
    QString serial;
    QString label;
    QString fsType;
    QString tableType;   // whole disks: "gpt", "dos" or empty
    quint64 size = 0;    // uncompressed bytes
    int sectorSize = 512;
    QString sha256;      // of the uncompressed data
    QString fileSha256;  // of the .zst file, for sha256sum
    QString app;
    QDateTime created;
    bool compressed = true;
    QString error;       // reading the description failed

    QJsonObject toJson() const;
    static BackupInfo fromJson(const QJsonObject &json);
};

namespace imagebackup {

QString infoPath(const QString &imagePath);
// Describes an image: from its .json if there is one, otherwise from the file itself
// (raw .img, or a .zst that records its size).
BackupInfo describe(const QString &imagePath);
BackupInfo infoFor(const Disk &disk, const Volume *volume);

} // namespace imagebackup

class BackupJob : public QObject
{
    Q_OBJECT
public:
    // Takes ownership of sourceFd. Writes imagePath and its .json; removes both if it fails.
    BackupJob(int sourceFd, const QString &imagePath, const BackupInfo &info);
    ~BackupJob() override;
    void cancel() { m_cancel = true; }

public slots:
    void run();

signals:
    void progress(const QString &phase, quint64 done, quint64 total);
    void finished(bool ok, const QString &message);

private:
    int m_source;
    QString m_path;
    BackupInfo m_info;
    std::atomic<bool> m_cancel{false};
};

class RestoreJob : public QObject
{
    Q_OBJECT
public:
    // Takes ownership of targetFd. checkFirst: read the whole backup and compare its
    // checksum before writing. wholeDisk: fix up the GPT backup if the target is bigger.
    RestoreJob(const QString &imagePath, const BackupInfo &info, int targetFd, bool checkFirst, bool verify);
    ~RestoreJob() override;
    void cancel() { m_cancel = true; }

public slots:
    void run();

signals:
    void progress(const QString &phase, quint64 done, quint64 total);
    // wroteAnything: false when it stopped before touching the target
    void finished(bool ok, const QString &message, bool wroteAnything);

private:
    QString m_path;
    BackupInfo m_info;
    int m_target;
    bool m_checkFirst;
    bool m_verify;
    std::atomic<bool> m_cancel{false};
};
