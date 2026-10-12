// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Searching a failing drive without wearing it out more: everything Find Lost Files reads from
// it is written into a Rescue Copy image as it goes, and read from there after that, so each
// part of the drive is read once. The image's map (ddrescue's format, next to it) says what's in
// it already and where the drive wouldn't read; Rescue Copy can finish the image later from there.

#include "lostsource.h"
#include "rescuecopy.h"

#include <QMutex>
#include <QString>

#include <memory>

namespace lost {

class ReadThrough : public Layer
{
public:
    // `drive`: the failing drive. `image`: the Rescue Copy image file (made, or carried on with
    // when it's there with its map). Null, with why, when it can't be made.
    static std::unique_ptr<ReadThrough> open(std::shared_ptr<Source> drive, const QString &image, QString *why);
    ~ReadThrough() override;

    quint64 size() const override { return m_size; }
    bool read(quint64 offset, char *buf, quint64 len) override;

    QString mapPath() const { return m_mapPath; }
    // Writes the map now (it's written every few seconds as it goes too, and when it's done with).
    bool saveMap(QString *why = nullptr);
    // How much is in the image now.
    quint64 copied() const;

private:
    ReadThrough() = default;

    std::shared_ptr<Source> m_drive;
    int m_image = -1;
    quint64 m_size = 0;
    QString m_mapPath;
    RescueMap m_map;
    mutable QMutex m_lock;
    qint64 m_savedAt = 0;
};

} // namespace lost
