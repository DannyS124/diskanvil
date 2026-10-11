// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "checksums.h"

#include <QObject>

#include <atomic>

// Writes a disk image to a device and reads it back to verify: an ISO or an .img as it is, or
// a compressed one (unpacked on the way, see ImageSource). With a checksum, the file is checked
// against it first, so a broken download never gets written. Runs in a worker thread; the fd
// comes from UDisks::openDevice and is closed when done.
class ImageWriter : public QObject
{
    Q_OBJECT
public:
    ImageWriter(const QString &imagePath, int fd, bool verify, const checksums::Expected &expected = {});
    ~ImageWriter() override;

    void cancel() { m_cancel = true; }

public slots:
    void run();

signals:
    void progress(const QString &phase, quint64 done, quint64 total);
    void finished(bool ok, const QString &message);

private:
    bool fail(const QString &message);

    QString m_imagePath;
    int m_fd;
    bool m_verify;
    checksums::Expected m_expected;
    std::atomic<bool> m_cancel{false};
};
