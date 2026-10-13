// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

#include <functional>

// A JPEG decoded again from what it really stores. A JPEG keeps each 8 by 8 square as rounded
// numbers (its DCT coefficients); a normal decoder shows the middle of each rounding, and hard
// squeezing shows as squares. This one looks for the smoothest picture whose numbers round to
// exactly what the file has (projection onto the quantisation constraint, as in the POCS
// deblocking literature and Google's Knusperli): the squares go, and nothing the file doesn't
// allow is made up: saved as a JPEG with the same tables, it gives the same numbers back.
namespace jpegexact {

// Whether this program can (it needs libjpeg).
bool available();
// The picture, turned the way its EXIF says (as viewers show it), or null with why (not a JPEG,
// a kind it doesn't take: progressive and baseline YCbCr or grey are taken; CMYK isn't).
// `rounds`: how many times it's smoothed and put back within the file's numbers (10 to 60).
QImage decode(const QByteArray &file, int rounds, QString *why, const std::function<bool(int percent)> &progress = {});

} // namespace jpegexact
