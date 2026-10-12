// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Looking closer at a picture or a film's frame: our own image tools for the viewer. Lighter or
// darker, more contrast, the dark parts lifted, levels stretched; grain smoothed; sharpened; and
// blur taken back out (Richardson-Lucy deconvolution, for a picture out of focus or one that moved
// while it was taken). They bring out what's there. They can't add what the camera never caught,
// and they never make detail up (no "AI" enlarging, which invents it).

#include <QImage>
#include <QStringList>

#include <functional>

namespace enhance {

struct Settings {
    int brightness = 0; // -100 to 100
    int contrast = 0;   // -100 to 100
    int shadows = 0;    // 0 to 100: the dark parts lifted
    bool autoLevels = false; // the darkest bit black and the lightest white
    int denoise = 0;    // 0 off, 1 a little, 2 more: grain smoothed (a median filter)
    int sharpen = 0;    // 0 to 100 (an unsharp mask)
    int focus = 0;      // 0 off, else how far out of focus it is, in pixels (1 to 15)
    int motion = 0;     // 0 off, else how far it moved while it was taken, in pixels (2 to 60)
    int motionAngle = 0; // which way it moved: degrees, 0 left to right, 90 up and down
    int rounds = 15;     // how hard the blur is taken out (Richardson-Lucy's iterations)
    bool grey = false;
    bool invert = false;
    bool isDefault() const;
    // What was done, in plain words, for the note saved with the picture.
    QStringList describe() const;
};

// The picture with the settings applied. `progress` hears how far it is (0 to 100) and stops it
// by returning false (then the result is empty).
QImage apply(const QImage &in, const Settings &settings, const std::function<bool(int percent)> &progress = {});
// Bigger by a whole factor (2, 3 or 4), smoothly (bicubic): what's there, larger, nothing made up.
QImage enlarge(const QImage &in, int factor);

} // namespace enhance
