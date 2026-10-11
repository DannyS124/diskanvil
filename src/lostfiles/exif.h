// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What a photo says about itself (EXIF): when it was taken, with what camera and settings, and
// where, when the camera wrote that down. Read straight from the photo's bytes: a JPEG's APP1
// segment, a TIFF-based file (TIFF itself and most cameras' raw files), or the EXIF block a
// HEIC, PNG or WebP carries. Every offset is checked, so a damaged or made-up file can't make it
// read past its end, and loops between folders of tags can't go round for ever.

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>

namespace exif {

struct Info {
    QString make, model, lens, software, artist;
    QDateTime taken;  // when the photo was taken, by the camera's clock
    bool takenHasZone = false; // the camera said its time zone too
    double exposure = 0;  // seconds
    double aperture = 0;  // the f-number
    int iso = 0;
    double focalLength = 0; // mm
    int width = 0, height = 0;
    int orientation = 0; // 1 to 8, 0 when not said
    bool hasLocation = false;
    double latitude = 0, longitude = 0; // degrees, north and east positive
    bool hasAltitude = false;
    double altitude = 0; // metres above the sea
    bool empty() const;
};

// From a whole file, or its start.
Info read(const QByteArray &file);
// From the TIFF structure EXIF is made of, starting at "II*\0" or "MM\0*".
Info fromTiff(QByteArrayView tiff);

// In plain words, a "label\tvalue" per line: Taken, Camera, Lens, Settings, Location, Edited
// with, By. Only what's known.
QStringList describe(const Info &info);
// The label describe() gives a picture's size ("Size"), for others to add one the same way.
QString sizeLabel();

} // namespace exif
