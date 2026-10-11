// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Adds up how much space each folder takes on one file system, like `du -x`: it stays
// on that file system, counts hard-linked files once and measures space actually used
// on disk. Read-only, runs as the user.

#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <memory>

struct UsageNode {
    QString name;
    quint64 size = 0;    // bytes used on disk, including everything inside
    quint64 files = 0;   // files inside, counted recursively
    bool isDir = false;
    bool skipped = false; // another file system or a folder we couldn't open
    // Biggest first. Small files are folded into one "n smaller files" entry.
    QVector<UsageNode> children;
};

class UsageScan : public QObject
{
    Q_OBJECT
public:
    explicit UsageScan(const QString &root);
    void cancel() { m_cancel = true; }
    // Files listed one by one in each folder; the rest are folded together.
    static constexpr int kFilesPerFolder = 48;

public slots:
    void run();

signals:
    void progress(quint64 bytes, quint64 files);
    // unreadable: folders that couldn't be opened (other users' or the system's)
    void finished(bool completed, std::shared_ptr<UsageNode> root, int unreadable);

private:
    QString m_root;
    std::atomic<bool> m_cancel{false};
};

Q_DECLARE_METATYPE(std::shared_ptr<UsageNode>)
