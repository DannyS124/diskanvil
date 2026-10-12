// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Evidence images in the Expert Witness format (.E01, EnCase 6's layout): a drive's every sector,
// in 32 KiB chunks packed with zlib, with the case's details, and MD5 and SHA-1 of the whole, as
// forensic tools (EnCase, FTK, Autopsy, libewf) expect. Our own writer and reader, from the libyal
// project's description of the format, checked against libewf's own tools.

#include "lostsource.h"

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <memory>

namespace ewf {

struct Case {
    QString caseNumber, evidenceNumber, description, examiner, notes;
    QString model, serial; // the drive's
};

struct Result {
    bool ok = false;
    QString error;
    quint64 bytes = 0;
    QByteArray md5, sha1, sha256; // of the drive's data
    QVector<QPair<quint64, quint64>> unreadable; // (sector, count): what couldn't be read, kept as zeros
    QDateTime started, finished;
    quint64 fileSize = 0;
};

// Reads `size` bytes from `fd` into the evidence file `path`.
class Acquire : public QObject
{
    Q_OBJECT
public:
    Acquire(int fd, quint64 size, int sectorSize, const QString &path, const Case &details);
    ~Acquire() override;
    void cancel() { m_cancel = true; }

public slots:
    void run();

signals:
    void progress(quint64 done, quint64 total);
    void finished(const ewf::Result &result);

private:
    int m_fd;
    quint64 m_size;
    int m_sectorSize;
    QString m_path;
    Case m_case;
    std::atomic<bool> m_cancel{false};
};

struct Info {
    QString problem;
    quint64 size = 0;
    int sectorSize = 512;
    QByteArray md5, sha1; // as kept in the file
    QString caseNumber, evidenceNumber, description, examiner, notes, model, serial;
};

// An evidence file (and its .E02, .E03... next to it) read as the drive it holds.
std::unique_ptr<lost::Layer> open(const QString &path, Info *info);
// Reads it all back and checks it against the MD5 it keeps: empty when it matches.
QString verify(const QString &path, const std::atomic<bool> *cancel = nullptr);

} // namespace ewf

Q_DECLARE_METATYPE(ewf::Result)
