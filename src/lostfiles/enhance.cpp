// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "enhance.h"

#include <QCoreApplication>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <thread>
#include <vector>

namespace enhance {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("enhance", text);
}

// One channel of the picture, 0 to 1.
struct Plane {
    int w = 0, h = 0;
    std::vector<float> v;
    float at(int x, int y) const { return v[size_t(std::clamp(y, 0, h - 1)) * size_t(w) + size_t(std::clamp(x, 0, w - 1))]; }
    float &operator()(int x, int y) { return v[size_t(y) * size_t(w) + size_t(x)]; }
};

// Rows shared out between the processor's cores.
template<typename F>
void rows(int h, F work)
{
    const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
    if (threads == 1 || h < 64) {
        work(0, h);
        return;
    }
    std::vector<std::thread> pool;
    const int step = (h + threads - 1) / threads;
    for (int from = 0; from < h; from += step)
        pool.emplace_back([&work, from, to = std::min(h, from + step)] { work(from, to); });
    for (std::thread &t : pool)
        t.join();
}

std::vector<float> gaussianKernel(double sigma)
{
    const int r = std::max(1, int(std::ceil(sigma * 3)));
    std::vector<float> k(size_t(2 * r + 1));
    double sum = 0;
    for (int i = -r; i <= r; ++i) {
        k[size_t(i + r)] = float(std::exp(-(i * i) / (2 * sigma * sigma)));
        sum += k[size_t(i + r)];
    }
    for (float &x : k)
        x = float(x / sum);
    return k;
}

// A Gaussian blur, across and then down (the edges carry on as they are).
Plane gaussian(const Plane &in, double sigma)
{
    const std::vector<float> k = gaussianKernel(sigma);
    const int r = int(k.size() / 2);
    Plane across{in.w, in.h, std::vector<float>(in.v.size())}, out{in.w, in.h, std::vector<float>(in.v.size())};
    rows(in.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < in.w; ++x) {
                float sum = 0;
                for (int i = -r; i <= r; ++i)
                    sum += k[size_t(i + r)] * in.at(x + i, y);
                across(x, y) = sum;
            }
        }
    });
    rows(in.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < in.w; ++x) {
                float sum = 0;
                for (int i = -r; i <= r; ++i)
                    sum += k[size_t(i + r)] * across.at(x, y + i);
                out(x, y) = sum;
            }
        }
    });
    return out;
}

// A movement blur: each point the average along a line `length` long, at `angle` degrees.
Plane motionBlur(const Plane &in, int length, int angle)
{
    const double a = angle * std::numbers::pi / 180;
    const double dx = std::cos(a), dy = -std::sin(a);
    std::vector<std::pair<float, float>> taps;
    for (int i = 0; i < length; ++i) {
        const double t = i - (length - 1) / 2.0;
        taps.push_back({float(t * dx), float(t * dy)});
    }
    Plane out{in.w, in.h, std::vector<float>(in.v.size())};
    rows(in.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < in.w; ++x) {
                float sum = 0;
                for (const auto &[ox, oy] : taps) {
                    // Between pixels: the four around it, by how near each is.
                    const float fx = float(x) + ox, fy = float(y) + oy;
                    const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
                    const float tx = fx - float(ix), ty = fy - float(iy);
                    sum += (in.at(ix, iy) * (1 - tx) + in.at(ix + 1, iy) * tx) * (1 - ty) + (in.at(ix, iy + 1) * (1 - tx) + in.at(ix + 1, iy + 1) * tx) * ty;
                }
                out(x, y) = sum / float(taps.size());
            }
        }
    });
    return out;
}

// Richardson-Lucy: the sharp picture that, blurred the way this one was, gives it. Each round
// blurs the guess, compares it with the picture, and corrects the guess by how far off it was.
// The blur is the same both ways round (a Gaussian, or a line through the middle), so the
// correction blurs the same way.
Plane deblur(const Plane &observed, const std::function<Plane(const Plane &)> &blur, int rounds, const std::function<bool()> &stop)
{
    constexpr float tiny = 1e-4f;
    Plane guess = observed;
    for (float &v : guess.v)
        v = std::clamp(v, tiny, 1.0f);
    for (int round = 0; round < rounds; ++round) {
        if (stop && stop())
            return {};
        const Plane blurred = blur(guess);
        Plane ratio{observed.w, observed.h, std::vector<float>(observed.v.size())};
        for (size_t i = 0; i < ratio.v.size(); ++i)
            ratio.v[i] = observed.v[i] / std::max(blurred.v[i], tiny);
        const Plane correction = blur(ratio);
        for (size_t i = 0; i < guess.v.size(); ++i)
            guess.v[i] = std::clamp(guess.v[i] * correction.v[i], 0.0f, 1.5f);
    }
    return guess;
}

// Grain smoothed: each point the middle value of those around it.
Plane median(const Plane &in, int radius)
{
    Plane out{in.w, in.h, std::vector<float>(in.v.size())};
    rows(in.h, [&](int y0, int y1) {
        std::vector<float> around;
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < in.w; ++x) {
                around.clear();
                for (int j = -radius; j <= radius; ++j) {
                    for (int i = -radius; i <= radius; ++i)
                        around.push_back(in.at(x + i, y + j));
                }
                std::nth_element(around.begin(), around.begin() + long(around.size() / 2), around.end());
                out(x, y) = around[around.size() / 2];
            }
        }
    });
    return out;
}

} // namespace

bool Settings::isDefault() const
{
    return brightness == 0 && contrast == 0 && shadows == 0 && !autoLevels && denoise == 0 && sharpen == 0 && focus == 0 && motion == 0 && !grey
        && !invert;
}

QStringList Settings::describe() const
{
    QStringList out;
    if (denoise > 0)
        out << tr("grain smoothed (%1)").arg(denoise);
    if (focus > 0)
        out << tr("out-of-focus blur of %1 px taken out (%2 rounds)").arg(focus).arg(rounds);
    if (motion > 0)
        out << tr("movement blur of %1 px at %2° taken out (%3 rounds)").arg(motion).arg(motionAngle).arg(rounds);
    if (sharpen > 0)
        out << tr("sharpened %1%").arg(sharpen);
    if (autoLevels)
        out << tr("levels stretched");
    if (brightness != 0)
        out << tr("brightness %1%2").arg(brightness > 0 ? QStringLiteral("+") : QString()).arg(brightness);
    if (contrast != 0)
        out << tr("contrast %1%2").arg(contrast > 0 ? QStringLiteral("+") : QString()).arg(contrast);
    if (shadows > 0)
        out << tr("dark parts lifted %1%").arg(shadows);
    if (grey)
        out << tr("black and white");
    if (invert)
        out << tr("inverted");
    return out;
}

QImage apply(const QImage &in, const Settings &s, const std::function<bool(int percent)> &progress)
{
    if (in.isNull() || s.isDefault())
        return in;
    const QImage src = in.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width(), h = src.height();
    std::array<Plane, 3> c;
    for (Plane &p : c)
        p = {w, h, std::vector<float>(size_t(w) * size_t(h))};
    for (int y = 0; y < h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            c[0](x, y) = qRed(line[x]) / 255.0f;
            c[1](x, y) = qGreen(line[x]) / 255.0f;
            c[2](x, y) = qBlue(line[x]) / 255.0f;
        }
    }
    bool stopped = false;
    const auto step = [&](int percent) {
        if (progress && !progress(percent))
            stopped = true;
        return stopped;
    };

    if (s.denoise > 0) {
        for (Plane &p : c)
            p = median(p, s.denoise);
        if (step(15))
            return {};
    }
    // Blur taken out of the brightness only (where the detail is), then given back to each colour.
    if (s.focus > 0 || s.motion > 0) {
        Plane y{w, h, std::vector<float>(size_t(w) * size_t(h))};
        for (size_t i = 0; i < y.v.size(); ++i)
            y.v[i] = 0.299f * c[0].v[i] + 0.587f * c[1].v[i] + 0.114f * c[2].v[i];
        Plane sharp = y;
        const int rounds = std::clamp(s.rounds, 1, 60);
        if (s.focus > 0) {
            const double sigma = std::clamp(s.focus, 1, 15) / 2.0;
            sharp = deblur(sharp, [sigma](const Plane &p) { return gaussian(p, sigma); }, rounds, [&] { return step(20); });
            if (stopped)
                return {};
        }
        if (s.motion > 0) {
            const int length = std::clamp(s.motion, 2, 60), angle = s.motionAngle;
            sharp = deblur(sharp, [length, angle](const Plane &p) { return motionBlur(p, length, angle); }, rounds, [&] { return step(40); });
            if (stopped)
                return {};
        }
        for (size_t i = 0; i < y.v.size(); ++i) {
            const float delta = sharp.v[i] - y.v[i];
            for (Plane &p : c)
                p.v[i] += delta;
        }
        if (step(60))
            return {};
    }
    if (s.sharpen > 0) {
        const float amount = s.sharpen / 50.0f;
        for (Plane &p : c) {
            const Plane soft = gaussian(p, 1.2);
            for (size_t i = 0; i < p.v.size(); ++i)
                p.v[i] += amount * (p.v[i] - soft.v[i]);
        }
        if (step(80))
            return {};
    }
    if (s.autoLevels) {
        // The darkest and lightest half percent left out, the rest stretched to fill black to white.
        std::vector<int> counts(1024);
        for (const Plane &p : c) {
            for (float v : p.v)
                ++counts[size_t(std::clamp(int(v * 1023), 0, 1023))];
        }
        const qint64 total = qint64(w) * h * 3, cut = total / 200;
        int lo = 0, hi = 1023;
        for (qint64 seen = 0; lo < 1023 && (seen += counts[size_t(lo)]) <= cut;)
            ++lo;
        for (qint64 seen = 0; hi > 0 && (seen += counts[size_t(hi)]) <= cut;)
            --hi;
        if (hi > lo) {
            const float low = lo / 1023.0f, span = (hi - lo) / 1023.0f;
            for (Plane &p : c) {
                for (float &v : p.v)
                    v = (v - low) / span;
            }
        }
    }
    // Brightness, contrast and the dark parts, the same for each colour.
    const float bright = s.brightness / 250.0f;
    const float contrast = s.contrast >= 0 ? 1 + s.contrast / 50.0f : 1 + s.contrast / 100.0f;
    const float gamma = 1.0f / (1 + s.shadows / 66.0f);
    rows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                for (Plane &p : c) {
                    float v = std::clamp(p(x, y), 0.0f, 1.0f);
                    if (s.shadows > 0)
                        v = std::pow(v, gamma);
                    v = (v - 0.5f) * contrast + 0.5f + bright;
                    p(x, y) = std::clamp(v, 0.0f, 1.0f);
                }
                if (s.grey) {
                    const float g = 0.299f * c[0](x, y) + 0.587f * c[1](x, y) + 0.114f * c[2](x, y);
                    c[0](x, y) = c[1](x, y) = c[2](x, y) = g;
                }
                if (s.invert) {
                    for (Plane &p : c)
                        p(x, y) = 1 - p(x, y);
                }
            }
        }
    });
    QImage out(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y) {
        const QRgb *from = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < w; ++x)
            line[x] = qRgba(qRound(c[0](x, y) * 255), qRound(c[1](x, y) * 255), qRound(c[2](x, y) * 255), qAlpha(from[x]));
    }
    step(100);
    return out;
}

QStringList ffmpegArguments(const QString &in, const QString &out, double speed, const Settings &s)
{
    speed = std::clamp(speed, 0.1, 2.0);
    QStringList video{QStringLiteral("setpts=PTS/%1").arg(speed, 0, 'f', 3)};
    if (s.brightness != 0 || s.contrast != 0 || s.shadows != 0) {
        const double contrast = s.contrast >= 0 ? 1 + s.contrast / 50.0 : 1 + s.contrast / 100.0;
        video << QStringLiteral("eq=brightness=%1:contrast=%2:gamma=%3").arg(s.brightness / 250.0, 0, 'f', 3).arg(contrast, 0, 'f', 3).arg(1 + s.shadows / 66.0, 0, 'f', 3);
    }
    if (s.sharpen > 0)
        video << QStringLiteral("unsharp=5:5:%1").arg(s.sharpen / 50.0, 0, 'f', 2);
    if (s.grey)
        video << QStringLiteral("hue=s=0");
    if (s.invert)
        video << QStringLiteral("negate");
    QStringList args{QStringLiteral("-hide_banner"), QStringLiteral("-nostats"), QStringLiteral("-progress"), QStringLiteral("pipe:1"), QStringLiteral("-y"),
                     QStringLiteral("-i"), in, QStringLiteral("-vf"), video.join(QLatin1Char(','))};
    // The sound slowed with it, down to half speed; slower than that it isn't worth hearing.
    if (speed >= 0.5)
        args << QStringLiteral("-af") << QStringLiteral("atempo=%1").arg(speed, 0, 'f', 3) << QStringLiteral("-c:a") << QStringLiteral("aac");
    else
        args << QStringLiteral("-an");
    args << QStringLiteral("-c:v") << QStringLiteral("libx264") << QStringLiteral("-crf") << QStringLiteral("18") << QStringLiteral("-pix_fmt")
         << QStringLiteral("yuv420p") << out;
    return args;
}

QImage enlarge(const QImage &in, int factor)
{
    factor = std::clamp(factor, 1, 4);
    if (in.isNull() || factor == 1)
        return in;
    const QImage src = in.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width(), h = src.height(), W = w * factor, H = h * factor;
    // Catmull-Rom: through the pixels that are there, smoothly between them.
    const auto weight = [](float t) {
        t = std::abs(t);
        if (t < 1)
            return 1.5f * t * t * t - 2.5f * t * t + 1;
        if (t < 2)
            return -0.5f * t * t * t + 2.5f * t * t - 4 * t + 2;
        return 0.0f;
    };
    const auto pixel = [&src, w, h](int x, int y) {
        return reinterpret_cast<const QRgb *>(src.constScanLine(std::clamp(y, 0, h - 1)))[std::clamp(x, 0, w - 1)];
    };
    QImage out(W, H, QImage::Format_ARGB32);
    if (out.isNull())
        return in;
    rows(H, [&](int y0, int y1) {
        for (int Y = y0; Y < y1; ++Y) {
            const float sy = (Y + 0.5f) / factor - 0.5f;
            const int iy = int(std::floor(sy));
            QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(Y));
            for (int X = 0; X < W; ++X) {
                const float sx = (X + 0.5f) / factor - 0.5f;
                const int ix = int(std::floor(sx));
                float r = 0, g = 0, b = 0, a = 0;
                for (int j = -1; j <= 2; ++j) {
                    const float wy = weight(sy - float(iy + j));
                    for (int i = -1; i <= 2; ++i) {
                        const float wt = wy * weight(sx - float(ix + i));
                        const QRgb p = pixel(ix + i, iy + j);
                        r += wt * qRed(p);
                        g += wt * qGreen(p);
                        b += wt * qBlue(p);
                        a += wt * qAlpha(p);
                    }
                }
                line[X] = qRgba(std::clamp(qRound(r), 0, 255), std::clamp(qRound(g), 0, 255), std::clamp(qRound(b), 0, 255), std::clamp(qRound(a), 0, 255));
            }
        }
    });
    return out;
}

} // namespace enhance
