// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Read-only look at Btrfs: which subvolumes are mounted where, and what snapper has
// snapshotted (through snapperd's D-Bus API, as the user; snapper's ALLOW_USERS decides).

#include <QDateTime>
#include <QMap>
#include <QString>
#include <QVector>

struct Subvolume {
    QString path;       // inside the file system, e.g. /@home
    QString mountPoint; // where it's mounted
    QString device;     // /dev/nvme0n1p2
    quint64 id = 0;
    QString options;    // mount options
};

struct SnapperConfig {
    QString name;
    QString subvolume; // the folder it snapshots, e.g. /
    QMap<QString, QString> attributes;
};

struct Snapshot {
    enum class Type { Single, Pre, Post };
    uint number = 0;
    Type type = Type::Single;
    uint preNumber = 0;
    QDateTime date;
    uint uid = 0;
    QString description;
    QString cleanup;
    QMap<QString, QString> userdata;
};

namespace snapper {

// Btrfs mounts, from /proc/self/mountinfo (or the given text, for tests).
QVector<Subvolume> mountedSubvolumes(const QByteArray &mountinfo = {});

bool available(); // snapperd answers on the system bus
QVector<SnapperConfig> configs(QString *error);
QVector<Snapshot> snapshots(const QString &config, QString *error);

} // namespace snapper
