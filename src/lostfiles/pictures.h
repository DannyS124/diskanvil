// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Pictures from a found file's bytes, the same way for the previews and the viewer. Most kinds
// go to Qt's own readers, each only to the one for what the file is. A camera's raw file shows
// the full-size JPEG the camera put inside it (every maker does), and a Photoshop file its merged
// picture, read by our own code below, so neither needs a raw or PSD plugin on the computer.

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>

namespace pictures {

// The format name Qt's reader knows the kind (a filetypes id) by; empty when it has none.
QByteArray readerFormat(const QString &id);
// Whether the kind is a camera's raw file.
bool isRaw(const QString &id);

// The picture, scaled down to fit `pixels` on its longest side (0: as big as it is). Null, with
// `why`, when it can't be shown. `type` is a filetypes number.
QImage decode(const QByteArray &data, int type, int pixels, QString *why = nullptr);
// How big the picture is, without decoding it; invalid when that can't be told.
QSize size(const QByteArray &data, int type);

// The biggest JPEG inside a file that decodes (a raw file's own preview); empty when none does.
QByteArray embeddedJpeg(const QByteArray &data);
// A Photoshop file's merged picture (or, failing that, the small one it keeps of itself).
QImage psd(const QByteArray &data, int pixels);

} // namespace pictures
