// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Looking closer at a picture or a film's frame: our own image tools for the viewer. Lighter or
// darker, more contrast, the dark parts lifted, levels stretched; grain smoothed; sharpened; and
// blur taken back out (Richardson-Lucy deconvolution, for a picture out of focus or one that moved
// while it was taken). They bring out what's there. They can't add what the camera never caught,
// and they never make detail up (no "AI" enlarging, which invents it).

#include <QImage>
#include <QPolygonF>
#include <QStringList>
#include <QVector>

#include <functional>

namespace enhance {

struct Settings {
    int brightness = 0; // -100 to 100
    int contrast = 0;   // -100 to 100
    int shadows = 0;    // 0 to 100: the dark parts lifted
    bool autoLevels = false; // the darkest bit black and the lightest white
    int denoise = 0;    // 0 off, 1 a little, 2 more: grain smoothed (a median filter)
    int smooth = 0;     // 0 to 100: grain smoothed with the edges kept (a bilateral filter)
    int localContrast = 0; // 0 to 100: detail brought out in the dark and light parts each on its own (CLAHE)
    bool deinterlace = false; // an old camera's two half-pictures (the comb lines) made into one
    int deblock = 0;          // 0 to 100: the 8 by 8 squares heavy JPEG squeezing leaves, evened out
    bool steady = false;      // a film saved steadied (ffmpeg's deshake); it doesn't change a picture
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
// Bigger by a whole factor (2 to 8), smoothly (Lanczos): what's there, larger, nothing made up.
QImage enlarge(const QImage &in, int factor);
// To any size, the same way (for showing a small picture big without its squares).
QImage resize(const QImage &in, QSize to);
// Something seen at an angle (a number plate, a sign, a page) made square-on: the four corners
// picked on it, clockwise from its top left, become a rectangle as wide and tall as its sides.
QImage straighten(const QImage &in, const QPolygonF &corners);
// Frames of a film showing the same thing, lined up on the one at `reference` and put together:
// the camera's noise, different in each, evens out, and lined up to a fraction of a pixel they
// can show more than any one of them (shift and add). `scale` 2 makes it twice as big from what the
// frames hold; `middle` takes each point's middle value (a passer-by in a few frames is left out)
// rather than the average. `shifts` hears how far each frame was moved to line up.
QImage stack(const QVector<QImage> &frames, int reference, int scale, bool middle, QVector<QPointF> *shifts = nullptr,
             const std::function<bool(int percent)> &progress = {});

// A profile: the settings kept under a name, to use again. DiskAnvil comes with some for what
// usually goes wrong (too dark, out of focus, a faded print...); people save their own and pass
// them around as small files (*.anvil-enhance, JSON), which are checked when they come in: only
// the settings above, each kept to its range.
struct Profile {
    QString name;
    QString description;
    Settings settings;
    bool builtIn = false;
};
// The ones that come with DiskAnvil.
QVector<Profile> builtInProfiles();
// A profile as its file, and back. Anything else, or a file over 64 KB, is refused with why.
QByteArray profileToFile(const Profile &profile);
bool profileFromFile(const QByteArray &file, Profile *profile, QString *why);
constexpr const char *kProfileSuffix = "anvil-enhance";
// Where saved and imported ones are kept, one file each.
QString profilesFolder();
QVector<Profile> savedProfiles(const QString &folder = profilesFolder());
// Saves it (replacing one of the same name); its file's path, or empty with why.
QString saveProfile(const Profile &profile, QString *why, const QString &folder = profilesFolder());
bool removeProfile(const QString &name, const QString &folder = profilesFolder());

// ffmpeg's arguments for a film at another speed (0.1 to 2) with the light tools (lighter,
// contrast, dark parts, sharpen, black and white, inverted) the same as the viewer shows them,
// to an MP4. Progress goes to its output, as out_time_us= lines.
QStringList ffmpegArguments(const QString &in, const QString &out, double speed, const Settings &settings);

} // namespace enhance
