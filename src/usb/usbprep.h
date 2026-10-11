// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "udiskssteps.h"

#include <QStringList>
#include <QVector>

// Sets a USB stick up through UDisks, one step at a time: a new partition table, its
// partitions (each with its file system and label), their flags, then mounts the ones asked
// for. Every step waits until UDisks shows the result before the next one starts. Make a
// DiskForge Live USB, the file copy mode of Write Image to USB and Make a Windows USB use it.
//
// It erases the stick: the dialogs ask first.
class UsbPrep : public UDisksSteps
{
    Q_OBJECT
public:
    struct Partition {
        QString fsType;     // "vfat", "ext4"...
        QString label;
        quint64 size = 0;   // 0: the rest of the stick
        quint64 flags = 0;  // MBR: 0x80 marks it bootable
        bool mount = false; // mounted once everything is made
    };

    UsbPrep(UDisks *udisks, const QString &diskPath, const QString &tableType, const QVector<Partition> &partitions,
            QObject *parent = nullptr);

    void start();
    // Unmounts what start() mounted, then emits done().
    void finish();

    // Once ready: the partitions' UDisks objects and mount points, in the order asked for.
    QStringList volumePaths() const { return m_volumes; }
    QStringList mountPoints() const { return m_mounts; }

signals:
    void ready(const QStringList &mountPoints);
    void done(bool ok, const QString &message);

private:
    void makeTable();
    void makePartition(int index);
    void setFlags(int index);
    void mountNext(int index);
    void unmountNext(int index);
    QString mountPointOf(const QString &volumePath) const;

    QString m_disk;
    QString m_table;
    QVector<Partition> m_partitions;
    QStringList m_volumes;
    QStringList m_mounts;
};
