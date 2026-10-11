// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>

#include <atomic>

struct BenchmarkResult {
    double readMBps = -1;
    double minReadMBps = -1;
    double maxReadMBps = -1;
    double accessMs = -1;
    double writeMBps = -1; // -1 if no write test
};

// Read speed and access time straight from the device (fd from UDisks::openDevice with
// forBenchmark, opened O_DIRECT), plus an optional write test through a temporary file
// on a mounted volume, so nothing on the disk is overwritten.
class Benchmark : public QObject
{
    Q_OBJECT
public:
    Benchmark(int fd, quint64 deviceSize, const QString &writeDir = {});
    ~Benchmark() override;

    void cancel() { m_cancel = true; }

public slots:
    void run();

signals:
    void progress(const QString &phase, int percent);
    void finished(bool ok, const BenchmarkResult &result, const QString &error);

private:
    int m_fd;
    quint64 m_size;
    QString m_writeDir;
    std::atomic<bool> m_cancel{false};
};

Q_DECLARE_METATYPE(BenchmarkResult)
