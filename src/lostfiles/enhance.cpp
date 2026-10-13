// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "enhance.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTransform>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
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
    return fisheye == 0 && nlm == 0 && !fullRange && field == 0 && homomorphic == 0 && retinex == 0 && channel == AllColour && deblock == 0 && brightness == 0 && contrast == 0 && shadows == 0 && !autoLevels && denoise == 0 && smooth == 0 && localContrast == 0 && !deinterlace
        && sharpen == 0 && focus == 0 && motion == 0 && !grey && !invert;
}

QStringList Settings::describe() const
{
    QStringList out;
    if (fisheye > 0)
        out << tr("a wide lens's bend taken out (%1%, division model)").arg(fisheye);
    if (fullRange)
        out << tr("video levels 16-235 stretched to the full range");
    if (field == 1 || field == 2)
        out << tr("only the %1 half-picture (field), its lines doubled").arg(field == 1 ? tr("first") : tr("second"));
    if (deinterlace)
        out << tr("deinterlaced");
    if (deblock > 0)
        out << tr("JPEG's squares evened out (%1%)").arg(deblock);
    if (smooth > 0)
        out << tr("grain smoothed with the edges kept (%1%)").arg(smooth);
    if (denoise > 0)
        out << tr("grain smoothed (%1)").arg(denoise);
    if (nlm > 0)
        out << tr("grain taken out by non-local means (%1%)").arg(nlm);
    const QString how = wiener ? tr("Wiener, grain allowance %1").arg(noise) : tr("Richardson-Lucy, %1 rounds").arg(rounds);
    if (focus > 0)
        out << tr("out-of-focus blur of %1 px (%2) taken out (%3)").arg(focus).arg(focusDisc ? tr("a lens's disc") : tr("Gaussian")).arg(how);
    if (motion > 0)
        out << tr("movement blur of %1 px at %2° taken out (%3)").arg(motion).arg(motionAngle).arg(how);
    if (sharpen > 0)
        out << tr("sharpened %1%").arg(sharpen);
    if (homomorphic > 0)
        out << tr("uneven light evened out %1% (homomorphic filter)").arg(homomorphic);
    if (retinex > 0)
        out << tr("multi-scale Retinex %1%").arg(retinex);
    if (localContrast > 0)
        out << tr("local contrast %1% (CLAHE)").arg(localContrast);
    if (autoLevels)
        out << tr("levels stretched");
    if (brightness != 0)
        out << tr("brightness %1%2").arg(brightness > 0 ? QStringLiteral("+") : QString()).arg(brightness);
    if (contrast != 0)
        out << tr("contrast %1%2").arg(contrast > 0 ? QStringLiteral("+") : QString()).arg(contrast);
    if (shadows > 0)
        out << tr("dark parts lifted %1%").arg(shadows);
    if (channel != AllColour)
        out << tr("only its %1, as grey").arg(channelName(channel));
    if (grey)
        out << tr("black and white");
    if (invert)
        out << tr("inverted");
    if (steady)
        out << tr("steadied");
    return out;
}

namespace {

// An old camera's picture is two half-pictures, the odd lines a moment after the even ones: with
// movement they comb. The odd lines are made again from the even ones around them.
void deinterlace(Plane &p)
{
    for (int y = 1; y < p.h; y += 2) {
        for (int x = 0; x < p.w; ++x)
            p(x, y) = (p.at(x, y - 1) + p.at(x, y + 1)) / 2;
    }
}

// A Gaussian blur wide enough to take in the light of a whole area: worked out on a smaller copy
// (a wide blur loses nothing that way) and made big again.
Plane wideBlur(const Plane &in, double sigma)
{
    const int f = std::max(1, int(sigma / 4));
    if (f == 1)
        return gaussian(in, sigma);
    Plane small{(in.w + f - 1) / f, (in.h + f - 1) / f, {}};
    small.v.assign(size_t(small.w) * size_t(small.h), 0);
    for (int y = 0; y < small.h; ++y)
        for (int x = 0; x < small.w; ++x) {
            float sum = 0;
            int n = 0;
            for (int j = 0; j < f && y * f + j < in.h; ++j)
                for (int i = 0; i < f && x * f + i < in.w; ++i, ++n)
                    sum += in.at(x * f + i, y * f + j);
            small(x, y) = sum / float(std::max(1, n));
        }
    const Plane blurred = gaussian(small, sigma / f);
    Plane out{in.w, in.h, std::vector<float>(in.v.size())};
    for (int y = 0; y < in.h; ++y)
        for (int x = 0; x < in.w; ++x) {
            const float sx = (x + 0.5f) / f - 0.5f, sy = (y + 0.5f) / f - 0.5f;
            const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
            const float tx = sx - ix, ty = sy - iy;
            out(x, y) = (blurred.at(ix, iy) * (1 - tx) + blurred.at(ix + 1, iy) * tx) * (1 - ty) + (blurred.at(ix, iy + 1) * (1 - tx) + blurred.at(ix + 1, iy + 1) * tx) * ty;
        }
    return out;
}

// The brightness as light falling on things times what they reflect: the light (the wide blur, in
// logs) turned down, what's lit turned up. A backlit doorway's face comes out of the dark.
Plane homomorphicFilter(const Plane &light, int strength)
{
    const float amount = strength / 100.0f;
    Plane logs{light.w, light.h, std::vector<float>(light.v.size())};
    for (size_t i = 0; i < logs.v.size(); ++i)
        logs.v[i] = std::log(std::max(light.v[i], 1e-3f));
    const Plane lighting = wideBlur(logs, std::max(light.w, light.h) / 30.0);
    const float low = 1 - 0.6f * amount, high = 1 + 0.8f * amount;
    Plane out{light.w, light.h, std::vector<float>(light.v.size())};
    for (size_t i = 0; i < out.v.size(); ++i)
        out.v[i] = std::exp(low * lighting.v[i] + high * (logs.v[i] - lighting.v[i]));
    return out;
}

// Multi-scale Retinex (Jobson, Rahman and Woodell): each point against the light around it at three
// distances, averaged, then spread over black to white (the darkest and lightest 1% left out).
Plane retinexFilter(const Plane &light)
{
    const double size = std::max(light.w, light.h);
    Plane logs{light.w, light.h, std::vector<float>(light.v.size())};
    for (size_t i = 0; i < logs.v.size(); ++i)
        logs.v[i] = std::log(std::max(light.v[i], 1e-3f));
    Plane sum{light.w, light.h, std::vector<float>(light.v.size(), 0)};
    for (const double scale : {size / 60, size / 12, size / 4}) {
        const Plane around = wideBlur(light, std::max(1.5, scale));
        for (size_t i = 0; i < sum.v.size(); ++i)
            sum.v[i] += (logs.v[i] - std::log(std::max(around.v[i], 1e-3f))) / 3;
    }
    std::vector<float> sorted = sum.v;
    const size_t lo = sorted.size() / 100, hi = sorted.size() - 1 - sorted.size() / 100;
    std::nth_element(sorted.begin(), sorted.begin() + std::ptrdiff_t(lo), sorted.end());
    const float bottom = sorted[lo];
    std::nth_element(sorted.begin(), sorted.begin() + std::ptrdiff_t(hi), sorted.end());
    const float top = sorted[hi];
    for (float &v : sum.v)
        v = std::clamp((v - bottom) / std::max(top - bottom, 1e-4f), 0.0f, 1.0f);
    return sum;
}

// A Fourier transform of our own (radix 2), in place, for convolving with any blur and
// taking it back out (Wiener).
using Complex = std::complex<float>;
void fft(std::vector<Complex> &a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double angle = 2 * std::numbers::pi / double(len) * (inverse ? 1 : -1);
        const Complex step(float(std::cos(angle)), float(std::sin(angle)));
        for (size_t i = 0; i < n; i += len) {
            Complex w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                const Complex u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
    if (inverse)
        for (Complex &x : a)
            x /= float(n);
}

// A grid of complex numbers, W by H (powers of two), turned in both directions.
struct Spectrum {
    int w = 0, h = 0;
    std::vector<Complex> v;
    void transform(bool inverse)
    {
        rows(h, [&](int y0, int y1) {
            std::vector<Complex> line(static_cast<size_t>(w));
            for (int y = y0; y < y1; ++y) {
                std::copy_n(v.begin() + std::ptrdiff_t(size_t(y) * size_t(w)), w, line.begin());
                fft(line, inverse);
                std::copy_n(line.begin(), w, v.begin() + std::ptrdiff_t(size_t(y) * size_t(w)));
            }
        });
        rows(w, [&](int x0, int x1) {
            std::vector<Complex> column(static_cast<size_t>(h));
            for (int x = x0; x < x1; ++x) {
                for (int y = 0; y < h; ++y)
                    column[size_t(y)] = v[size_t(y) * size_t(w) + size_t(x)];
                fft(column, inverse);
                for (int y = 0; y < h; ++y)
                    v[size_t(y) * size_t(w) + size_t(x)] = column[size_t(y)];
            }
        });
    }
};

int powerOfTwo(int n)
{
    int p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

// The picture on a grid big enough for its blur, its edges mirrored outwards (so the transform,
// which wraps round, doesn't see a hard edge and ring), and the blur's own spectrum, its middle at 0,0.
Spectrum padded(const Plane &p, int margin, int W, int H)
{
    Spectrum s{W, H, std::vector<Complex>(size_t(W) * size_t(H))};
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int sx = x - margin, sy = y - margin;
            // Mirrored past each edge, and mirrored back where the grid wraps round.
            if (sx >= p.w + (W - p.w - margin) / 2) sx -= W;
            if (sy >= p.h + (H - p.h - margin) / 2) sy -= H;
            sx = sx < 0 ? -sx - 1 : sx >= p.w ? 2 * p.w - sx - 1 : sx;
            sy = sy < 0 ? -sy - 1 : sy >= p.h ? 2 * p.h - sy - 1 : sy;
            s.v[size_t(y) * size_t(W) + size_t(x)] = p.at(sx, sy);
        }
    return s;
}

Spectrum kernelSpectrum(const std::vector<float> &kernel, int size, int W, int H)
{
    Spectrum k{W, H, std::vector<Complex>(size_t(W) * size_t(H))};
    const int r = size / 2;
    for (int j = 0; j < size; ++j)
        for (int i = 0; i < size; ++i) {
            const float v = kernel[size_t(j * size + i)];
            if (v != 0)
                k.v[size_t((j - r + H) % H) * size_t(W) + size_t((i - r + W) % W)] = v;
        }
    k.transform(false);
    return k;
}

// The blur as a picture of what one point becomes: a disc (a lens out of focus), a Gaussian, or
// a line (it moved), summing to 1. `size` is its width and height.
std::vector<float> discKernel(double radius, int *size)
{
    const int r = int(std::ceil(radius)) + 1;
    *size = 2 * r + 1;
    std::vector<float> k(size_t(*size) * size_t(*size));
    double sum = 0;
    for (int y = -r; y <= r; ++y)
        for (int x = -r; x <= r; ++x) {
            // Its edge anti-aliased: how much of each pixel the disc covers, roughly.
            const double d = std::hypot(x, y);
            const double v = std::clamp(radius + 0.5 - d, 0.0, 1.0);
            k[size_t((y + r) * *size + x + r)] = float(v);
            sum += v;
        }
    for (float &v : k)
        v = float(v / sum);
    return k;
}

std::vector<float> gaussianKernel2D(double sigma, int *size)
{
    const int r = std::max(1, int(std::ceil(sigma * 3)));
    *size = 2 * r + 1;
    std::vector<float> k(size_t(*size) * size_t(*size));
    double sum = 0;
    for (int y = -r; y <= r; ++y)
        for (int x = -r; x <= r; ++x) {
            const double v = std::exp(-(x * x + y * y) / (2 * sigma * sigma));
            k[size_t((y + r) * *size + x + r)] = float(v);
            sum += v;
        }
    for (float &v : k)
        v = float(v / sum);
    return k;
}

std::vector<float> lineKernel(int length, int angle, int *size)
{
    const int r = length / 2 + 1;
    *size = 2 * r + 1;
    std::vector<float> k(size_t(*size) * size_t(*size));
    const double a = angle * std::numbers::pi / 180, dx = std::cos(a), dy = -std::sin(a);
    double sum = 0;
    for (int i = 0; i < length * 4; ++i) {
        const double t = (i / 4.0) - (length - 1) / 2.0;
        const double fx = t * dx + r, fy = t * dy + r;
        const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
        const double tx = fx - ix, ty = fy - iy;
        const auto add = [&](int x, int y, double v) {
            if (x >= 0 && y >= 0 && x < *size && y < *size) {
                k[size_t(y * *size + x)] += float(v);
                sum += v;
            }
        };
        add(ix, iy, (1 - tx) * (1 - ty)); add(ix + 1, iy, tx * (1 - ty)); add(ix, iy + 1, (1 - tx) * ty); add(ix + 1, iy + 1, tx * ty);
    }
    for (float &v : k)
        v = float(v / sum);
    return k;
}

// Wiener deconvolution: the picture that, blurred by the kernel, gives this one, with `noise`
// (the noise-to-signal ratio) keeping the grain from being blown up where the blur took
// everything away.
Plane wienerDeblur(const Plane &in, const std::vector<float> &kernel, int size, float noise)
{
    const int margin = size + 8;
    const int W = powerOfTwo(in.w + 2 * margin), H = powerOfTwo(in.h + 2 * margin);
    Spectrum pic = padded(in, margin, W, H);
    pic.transform(false);
    const Spectrum k = kernelSpectrum(kernel, size, W, H);
    for (size_t i = 0; i < pic.v.size(); ++i) {
        const Complex h = k.v[i];
        pic.v[i] = pic.v[i] * std::conj(h) / (std::norm(h) + noise);
    }
    pic.transform(true);
    Plane out{in.w, in.h, std::vector<float>(in.v.size())};
    for (int y = 0; y < in.h; ++y)
        for (int x = 0; x < in.w; ++x)
            out(x, y) = pic.v[size_t(y + margin) * size_t(W) + size_t(x + margin)].real();
    return out;
}

// Blurring by any kernel (for Richardson-Lucy with a disc), through the transform.
Plane convolve(const Plane &in, const std::vector<float> &kernel, int size)
{
    const int margin = size + 8;
    const int W = powerOfTwo(in.w + 2 * margin), H = powerOfTwo(in.h + 2 * margin);
    Spectrum pic = padded(in, margin, W, H);
    pic.transform(false);
    const Spectrum k = kernelSpectrum(kernel, size, W, H);
    for (size_t i = 0; i < pic.v.size(); ++i)
        pic.v[i] *= k.v[i];
    pic.transform(true);
    Plane out{in.w, in.h, std::vector<float>(in.v.size())};
    for (int y = 0; y < in.h; ++y)
        for (int x = 0; x < in.w; ++x)
            out(x, y) = pic.v[size_t(y + margin) * size_t(W) + size_t(x + margin)].real();
    return out;
}

// Non-local means (Buades, Coll and Morel): each point the average of points around it whose
// little patches look like its own, weighted by how alike; grain, different in each, averages
// out, while edges and texture, alike in their patches, stay. Patches are compared on the
// brightness, and the same weights used for each colour.
void nonLocalMeans(std::array<Plane, 3> &c, const Plane &light, int strength)
{
    constexpr int reach = 6, patch = 2;
    // How grainy it is (the spread of what's left after a little blur), so the strength means
    // the same on every picture.
    const Plane soft = gaussian(light, 1.0);
    std::vector<float> rest(light.v.size());
    for (size_t i = 0; i < rest.size(); ++i)
        rest[i] = std::abs(light.v[i] - soft.v[i]);
    std::nth_element(rest.begin(), rest.begin() + std::ptrdiff_t(rest.size() / 2), rest.end());
    const float sigma = std::max(0.004f, rest[rest.size() / 2] * 1.4826f * 1.6f);
    const float h = sigma * (0.4f + strength / 100.0f * 1.4f);
    const float h2 = h * h * float((2 * patch + 1) * (2 * patch + 1));
    const int w = light.w, H = light.h;
    std::array<Plane, 3> sum = {Plane{w, H, std::vector<float>(light.v.size(), 0)}, Plane{w, H, std::vector<float>(light.v.size(), 0)},
                                Plane{w, H, std::vector<float>(light.v.size(), 0)}};
    std::vector<float> total(light.v.size(), 0);
    // One offset at a time: its patch distances for every point from a running box sum.
    std::vector<float> diff(light.v.size()), box(light.v.size());
    for (int dy = -reach; dy <= reach; ++dy) {
        for (int dx = -reach; dx <= reach; ++dx) {
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < w; ++x) {
                    const float d = light.at(x, y) - light.at(x + dx, y + dy);
                    diff[size_t(y) * size_t(w) + size_t(x)] = d * d;
                }
            // Box sum across, then down.
            rows(H, [&](int y0, int y1) {
                for (int y = y0; y < y1; ++y) {
                    float run = 0;
                    for (int x = -patch; x <= patch; ++x)
                        run += diff[size_t(y) * size_t(w) + size_t(std::clamp(x, 0, w - 1))];
                    for (int x = 0; x < w; ++x) {
                        box[size_t(y) * size_t(w) + size_t(x)] = run;
                        run += diff[size_t(y) * size_t(w) + size_t(std::clamp(x + patch + 1, 0, w - 1))]
                             - diff[size_t(y) * size_t(w) + size_t(std::clamp(x - patch, 0, w - 1))];
                    }
                }
            });
            rows(w, [&](int x0, int x1) {
                for (int x = x0; x < x1; ++x) {
                    float run = 0;
                    for (int y = -patch; y <= patch; ++y)
                        run += box[size_t(std::clamp(y, 0, H - 1)) * size_t(w) + size_t(x)];
                    for (int y = 0; y < H; ++y) {
                        const float wgt = std::exp(-std::max(0.0f, run - 2 * sigma * sigma * float((2 * patch + 1) * (2 * patch + 1))) / h2);
                        const size_t i = size_t(y) * size_t(w) + size_t(x);
                        for (int k = 0; k < 3; ++k)
                            sum[size_t(k)].v[i] += wgt * c[size_t(k)].at(x + dx, y + dy);
                        total[i] += wgt;
                        run += box[size_t(std::clamp(y + patch + 1, 0, H - 1)) * size_t(w) + size_t(x)]
                             - box[size_t(std::clamp(y - patch, 0, H - 1)) * size_t(w) + size_t(x)];
                    }
                }
            });
        }
    }
    for (size_t i = 0; i < total.size(); ++i)
        for (int k = 0; k < 3; ++k)
            c[size_t(k)].v[i] = sum[size_t(k)].v[i] / std::max(total[i], 1e-6f);
}

// JPEG squeezes a picture in 8 by 8 squares; squeezed hard, their edges show. Where the step
// across a square's edge is small (the squares, not a real edge in the picture), it's spread over
// the pixels either side so the squares melt into each other. A big step is a real edge: kept.
void deblock(Plane &p, int strength)
{
    const float limit = 0.03f + strength / 100.0f * 0.12f, amount = 0.4f + strength / 100.0f * 0.6f;
    const auto across = [&](float &a2, float &a1, float &b1, float &b2) {
        const float d = b1 - a1;
        if (std::abs(d) >= limit)
            return;
        const float c = d * amount;
        a1 += c * 0.375f; b1 -= c * 0.375f;
        a2 += c * 0.125f; b2 -= c * 0.125f;
    };
    for (int y = 0; y < p.h; ++y)
        for (int x = 8; x + 1 < p.w; x += 8)
            across(p(x - 2, y), p(x - 1, y), p(x, y), p(x + 1, y));
    for (int y = 8; y + 1 < p.h; y += 8)
        for (int x = 0; x < p.w; ++x)
            across(p(x, y - 2), p(x, y - 1), p(x, y), p(x, y + 1));
}

// Each point the average of those around it, weighted by how near they are and by how alike in
// brightness: grain is evened out, while an edge (a big step in brightness) is left sharp.
void bilateral(std::array<Plane, 3> &c, const Plane &light, int strength)
{
    const int r = 1 + strength / 25;                 // 1 to 5 pixels around
    const float space = 2 * (0.5f + r / 2.0f) * (0.5f + r / 2.0f);
    const float range = 2 * std::pow(0.02f + strength / 100.0f * 0.13f, 2.0f);
    std::vector<float> near(size_t((2 * r + 1) * (2 * r + 1)));
    for (int j = -r; j <= r; ++j)
        for (int i = -r; i <= r; ++i)
            near[size_t((j + r) * (2 * r + 1) + i + r)] = std::exp(-(i * i + j * j) / space);
    const std::array<Plane, 3> in = c;
    rows(light.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < light.w; ++x) {
                const float here = light.at(x, y);
                float sum[3] = {0, 0, 0}, total = 0;
                for (int j = -r; j <= r; ++j) {
                    for (int i = -r; i <= r; ++i) {
                        const float d = light.at(x + i, y + j) - here;
                        const float wt = near[size_t((j + r) * (2 * r + 1) + i + r)] * std::exp(-d * d / range);
                        for (int k = 0; k < 3; ++k)
                            sum[k] += wt * in[size_t(k)].at(x + i, y + j);
                        total += wt;
                    }
                }
                for (int k = 0; k < 3; ++k)
                    c[size_t(k)](x, y) = sum[k] / total;
            }
        }
    });
}

// Contrast-limited adaptive histogram equalisation, on the brightness: the picture in 8 by 8
// tiles, each one's levels spread over black to white (limited, so flat parts' grain isn't blown
// up), and each point blended from the four tiles nearest it so there are no seams.
Plane clahe(const Plane &light, int strength)
{
    constexpr int tiles = 8, bins = 256;
    const int tw = std::max(1, (light.w + tiles - 1) / tiles), th = std::max(1, (light.h + tiles - 1) / tiles);
    const float limit = 2.0f + strength / 100.0f * 8.0f; // times an even share
    std::vector<std::array<float, bins>> map(size_t(tiles * tiles));
    for (int ty = 0; ty < tiles; ++ty) {
        for (int tx = 0; tx < tiles; ++tx) {
            std::array<float, bins> count{};
            int n = 0;
            for (int y = ty * th; y < std::min(light.h, (ty + 1) * th); ++y)
                for (int x = tx * tw; x < std::min(light.w, (tx + 1) * tw); ++x, ++n)
                    count[size_t(std::clamp(int(light.at(x, y) * (bins - 1) + 0.5f), 0, bins - 1))] += 1;
            std::array<float, bins> &m = map[size_t(ty * tiles + tx)];
            if (n == 0) {
                for (int b = 0; b < bins; ++b)
                    m[size_t(b)] = b / float(bins - 1);
                continue;
            }
            // What's over the limit is shared out evenly.
            const float most = limit * n / bins;
            float over = 0;
            for (float &v : count) {
                if (v > most) {
                    over += v - most;
                    v = most;
                }
            }
            float sum = 0;
            for (int b = 0; b < bins; ++b) {
                sum += count[size_t(b)] + over / bins;
                m[size_t(b)] = sum / n;
            }
        }
    }
    Plane out{light.w, light.h, std::vector<float>(light.v.size())};
    rows(light.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float fy = std::clamp((y + 0.5f) / th - 0.5f, 0.0f, tiles - 1.0f);
            const int ty = std::min(int(fy), tiles - 2 < 0 ? 0 : tiles - 2);
            const float wy = fy - ty;
            for (int x = 0; x < light.w; ++x) {
                const float fx = std::clamp((x + 0.5f) / tw - 0.5f, 0.0f, tiles - 1.0f);
                const int tx = std::min(int(fx), tiles - 2);
                const float wx = fx - tx;
                const size_t b = size_t(std::clamp(int(light.at(x, y) * (bins - 1) + 0.5f), 0, bins - 1));
                const float top = map[size_t(ty * tiles + tx)][b] * (1 - wx) + map[size_t(ty * tiles + tx + 1)][b] * wx;
                const float bottom = map[size_t((ty + 1) * tiles + tx)][b] * (1 - wx) + map[size_t((ty + 1) * tiles + tx + 1)][b] * wx;
                out(x, y) = top * (1 - wy) + bottom * wy;
            }
        }
    });
    return out;
}

Plane lightOf(const std::array<Plane, 3> &c)
{
    Plane y{c[0].w, c[0].h, std::vector<float>(c[0].v.size())};
    for (size_t i = 0; i < y.v.size(); ++i)
        y.v[i] = 0.299f * c[0].v[i] + 0.587f * c[1].v[i] + 0.114f * c[2].v[i];
    return y;
}

// Lanczos with three lobes: the sharpest of the usual ways to go between pixels.
float lanczos(float t)
{
    t = std::abs(t);
    if (t < 1e-5f)
        return 1;
    if (t >= 3)
        return 0;
    const float pt = float(std::numbers::pi) * t;
    return 3 * std::sin(pt) * std::sin(pt / 3) / (pt * pt);
}

// A point between pixels of `src`, from the 6 by 6 around it (Lanczos), clamped to what's there.
QRgb sample(const QImage &src, float sx, float sy)
{
    const int w = src.width(), h = src.height();
    const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
    float r = 0, g = 0, b = 0, a = 0, total = 0;
    float lo[4] = {255, 255, 255, 255}, hi[4] = {0, 0, 0, 0};
    for (int j = -2; j <= 3; ++j) {
        const float wy = lanczos(sy - float(iy + j));
        if (wy == 0)
            continue;
        const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(std::clamp(iy + j, 0, h - 1)));
        for (int i = -2; i <= 3; ++i) {
            const float wt = wy * lanczos(sx - float(ix + i));
            if (wt == 0)
                continue;
            const QRgb p = line[std::clamp(ix + i, 0, w - 1)];
            r += wt * qRed(p);
            g += wt * qGreen(p);
            b += wt * qBlue(p);
            a += wt * qAlpha(p);
            total += wt;
            if (std::abs(i - (sx - ix)) < 1.5f && std::abs(j - (sy - iy)) < 1.5f) {
                const float v[4] = {float(qRed(p)), float(qGreen(p)), float(qBlue(p)), float(qAlpha(p))};
                for (int k = 0; k < 4; ++k) {
                    lo[k] = std::min(lo[k], v[k]);
                    hi[k] = std::max(hi[k], v[k]);
                }
            }
        }
    }
    if (total == 0)
        return 0;
    // Kept between the pixels nearest it: no bright or dark halo beside an edge (Lanczos rings).
    const auto keep = [&](float v, int k) { return std::clamp(qRound(std::clamp(v / total, lo[k], hi[k])), 0, 255); };
    return qRgba(keep(r, 0), keep(g, 1), keep(b, 2), keep(a, 3));
}

} // namespace

QVector<Profile> builtInProfiles()
{
    const auto make = [](const QString &name, const QString &description, const std::function<void(Settings &)> &set) {
        Profile p;
        p.name = name;
        p.description = description;
        set(p.settings);
        p.builtIn = true;
        return p;
    };
    return {
        make(tr("Too dark"), tr("A photo taken with too little light: lighter, the dark parts lifted, the levels stretched, a little grain smoothed."),
             [](Settings &s) { s.brightness = 20; s.shadows = 50; s.autoLevels = true; s.denoise = 1; }),
        make(tr("Out of focus"), tr("Soft all over: a blur of 3 pixels taken back out, then sharpened a little. Raise Out of focus if it's softer."),
             [](Settings &s) { s.focus = 3; s.sharpen = 20; }),
        make(tr("Moved while it was taken"), tr("Streaked by a shaky hand or something moving: then use Draw Along a Streak to set how far and which way."),
             [](Settings &s) { s.motion = 10; s.sharpen = 10; }),
        make(tr("Faded old photo"), tr("A print gone pale: the levels stretched, more contrast, sharpened a little."),
             [](Settings &s) { s.autoLevels = true; s.contrast = 25; s.sharpen = 15; }),
        make(tr("Night camera"), tr("A security or dashboard camera at night: lighter, the dark parts lifted, the detail in them brought out, "
                                    "grain smoothed with the edges kept, sharpened."),
             [](Settings &s) { s.brightness = 15; s.shadows = 50; s.localContrast = 50; s.smooth = 50; s.sharpen = 20; }),
        make(tr("Old security camera"), tr("Grainy footage from an older camera, with comb lines when things move: deinterlaced, grain smoothed "
                                           "with the edges kept, local contrast, sharpened. Pick a spot and stack frames for more."),
             [](Settings &s) { s.deinterlace = true; s.smooth = 40; s.localContrast = 40; s.sharpen = 25; }),
        make(tr("Number plate or sign"), tr("Letters to read: detail brought out where it's dark or bright, sharpened hard, black and white. Pick "
                                            "a spot around it; Straighten if it's at an angle."),
             [](Settings &s) { s.localContrast = 70; s.sharpen = 45; s.grey = true; s.autoLevels = true; }),
        make(tr("Small or blocky picture"), tr("A thumbnail, or a photo squeezed hard: JPEG's squares evened out, grain smoothed with the edges kept, "
                                               "sharpened a little. Save it bigger below; shown bigger it's smoothed too (Pixels shows the squares)."),
             [](Settings &s) { s.deblock = 70; s.smooth = 25; s.sharpen = 15; }),
        make(tr("Grainy"), tr("Speckled from a high ISO or a cheap camera: grain smoothed, then sharpened a little."),
             [](Settings &s) { s.denoise = 2; s.sharpen = 10; }),
        make(tr("Scanned page"), tr("A document or receipt: black and white, the levels stretched, strong contrast, sharpened, so the writing stands out."),
             [](Settings &s) { s.grey = true; s.autoLevels = true; s.contrast = 40; s.sharpen = 30; }),
        make(tr("Film negative"), tr("A photo of a negative: turned back into a positive, the levels stretched."),
             [](Settings &s) { s.invert = true; s.autoLevels = true; }),
    };
}

QByteArray profileToFile(const Profile &p)
{
    const Settings &s = p.settings;
    QJsonObject settings{{QStringLiteral("brightness"), s.brightness}, {QStringLiteral("contrast"), s.contrast},
                         {QStringLiteral("shadows"), s.shadows},       {QStringLiteral("autoLevels"), s.autoLevels},
                         {QStringLiteral("denoise"), s.denoise},       {QStringLiteral("sharpen"), s.sharpen},
                         {QStringLiteral("focus"), s.focus},           {QStringLiteral("motion"), s.motion},
                         {QStringLiteral("motionAngle"), s.motionAngle}, {QStringLiteral("grey"), s.grey},
                         {QStringLiteral("invert"), s.invert},         {QStringLiteral("smooth"), s.smooth},
                         {QStringLiteral("localContrast"), s.localContrast}, {QStringLiteral("deinterlace"), s.deinterlace},
                         {QStringLiteral("steady"), s.steady},         {QStringLiteral("deblock"), s.deblock},
                         {QStringLiteral("homomorphic"), s.homomorphic}, {QStringLiteral("retinex"), s.retinex},
                         {QStringLiteral("channel"), s.channel},       {QStringLiteral("field"), s.field},
                         {QStringLiteral("fullRange"), s.fullRange},   {QStringLiteral("nlm"), s.nlm},
                         {QStringLiteral("noise"), s.noise},           {QStringLiteral("focusDisc"), s.focusDisc},
                         {QStringLiteral("wiener"), s.wiener},         {QStringLiteral("fisheye"), s.fisheye}};
    QJsonObject o{{QStringLiteral("diskanvil-enhance-profile"), 1}, {QStringLiteral("name"), p.name}, {QStringLiteral("settings"), settings}};
    if (!p.description.isEmpty())
        o.insert(QStringLiteral("description"), p.description);
    return QJsonDocument(o).toJson();
}

bool profileFromFile(const QByteArray &file, Profile *p, QString *why)
{
    if (file.size() > 64 * 1024) {
        *why = tr("It's too big to be an enhance profile.");
        return false;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file, &error);
    const QJsonObject o = document.object();
    if (error.error != QJsonParseError::NoError || !o.contains(QStringLiteral("diskanvil-enhance-profile"))) {
        *why = tr("It isn't a DiskAnvil enhance profile.");
        return false;
    }
    if (o.value(QStringLiteral("diskanvil-enhance-profile")).toInt() != 1) {
        *why = tr("It was made by a newer DiskAnvil: update this one to use it.");
        return false;
    }
    // Names and notes shown on the screen: one line, no control characters, not too long.
    const auto clean = [](QString text, int most) {
        text.replace(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]+")), QStringLiteral(" "));
        return text.simplified().left(most);
    };
    Profile out;
    out.name = clean(o.value(QStringLiteral("name")).toString(), 60);
    out.description = clean(o.value(QStringLiteral("description")).toString(), 300);
    if (out.name.isEmpty()) {
        *why = tr("The profile has no name.");
        return false;
    }
    const QJsonObject j = o.value(QStringLiteral("settings")).toObject();
    const auto number = [&j](const char *key, int low, int high) { return std::clamp(j.value(QLatin1String(key)).toInt(0), low, high); };
    Settings &s = out.settings;
    s.brightness = number("brightness", -100, 100);
    s.contrast = number("contrast", -100, 100);
    s.shadows = number("shadows", 0, 100);
    s.denoise = number("denoise", 0, 2);
    s.sharpen = number("sharpen", 0, 100);
    s.focus = number("focus", 0, 15);
    s.motion = number("motion", 0, 60);
    if (s.motion == 1)
        s.motion = 2;
    s.motionAngle = number("motionAngle", 0, 179);
    s.autoLevels = j.value(QStringLiteral("autoLevels")).toBool();
    s.grey = j.value(QStringLiteral("grey")).toBool();
    s.invert = j.value(QStringLiteral("invert")).toBool();
    s.smooth = number("smooth", 0, 100);
    s.deblock = number("deblock", 0, 100);
    s.homomorphic = number("homomorphic", 0, 100);
    s.retinex = number("retinex", 0, 100);
    s.channel = number("channel", 0, Settings::MostContrast);
    s.field = number("field", 0, 2);
    s.fullRange = j.value(QStringLiteral("fullRange")).toBool();
    s.nlm = number("nlm", 0, 100);
    s.fisheye = number("fisheye", 0, 100);
    s.noise = std::max(1, number("noise", 0, 100));
    s.focusDisc = j.value(QStringLiteral("focusDisc")).toBool();
    s.wiener = j.value(QStringLiteral("wiener")).toBool();
    s.localContrast = number("localContrast", 0, 100);
    s.deinterlace = j.value(QStringLiteral("deinterlace")).toBool();
    s.steady = j.value(QStringLiteral("steady")).toBool();
    *p = out;
    return true;
}

QString profilesFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/enhance-profiles");
}

namespace {
// Its file's name: the profile's name made safe for any file system.
QString profileFile(const QString &folder, const QString &name)
{
    QString base = name;
    base.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]+")), QStringLiteral("_"));
    base = base.trimmed();
    while (base.endsWith(QLatin1Char('.')))
        base.chop(1);
    if (base.isEmpty())
        base = QStringLiteral("profile");
    return folder + QLatin1Char('/') + base + QLatin1Char('.') + QLatin1String(kProfileSuffix);
}
} // namespace

QVector<Profile> savedProfiles(const QString &folder)
{
    QVector<Profile> out;
    const QFileInfoList files = QDir(folder).entryInfoList({QStringLiteral("*.") + QLatin1String(kProfileSuffix)}, QDir::Files, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &info : files) {
        QFile f(info.filePath());
        Profile p;
        QString why;
        if (info.size() <= 64 * 1024 && f.open(QIODevice::ReadOnly) && profileFromFile(f.readAll(), &p, &why))
            out << p;
    }
    return out;
}

QString saveProfile(const Profile &p, QString *why, const QString &folder)
{
    if (!QDir().mkpath(folder)) {
        *why = tr("The folder for profiles couldn't be made: %1").arg(folder);
        return {};
    }
    removeProfile(p.name, folder); // one of the same name, whatever its file is called
    const QString path = profileFile(folder, p.name);
    QSaveFile out(path);
    const QByteArray bytes = profileToFile(p);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) {
        *why = out.errorString();
        return {};
    }
    return path;
}

bool removeProfile(const QString &name, const QString &folder)
{
    bool removed = false;
    const QFileInfoList files = QDir(folder).entryInfoList({QStringLiteral("*.") + QLatin1String(kProfileSuffix)}, QDir::Files);
    for (const QFileInfo &info : files) {
        QFile f(info.filePath());
        Profile p;
        QString why;
        if (f.open(QIODevice::ReadOnly) && profileFromFile(f.read(64 * 1024 + 1), &p, &why) && p.name.compare(name, Qt::CaseInsensitive) == 0) {
            f.close();
            removed = QFile::remove(info.filePath()) || removed;
        }
    }
    return removed;
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

    if (s.fisheye > 0) {
        // Each point of the result from where the bent picture has it (the inverse of unbend,
        // found by a few steps of a fixed-point solve), smoothly between pixels.
        const QSize size(w, h);
        const QPointF centre(w / 2.0, h / 2.0);
        const double half = std::hypot(w, h) / 2, k = -std::clamp(s.fisheye, 0, 100) / 100.0 * 0.45;
        std::array<Plane, 3> bent = c;
        rows(h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < w; ++x) {
                    // Undistorted u = d / (1 + k |d|^2); solve for d given u.
                    const QPointF u = (QPointF(x + 0.5, y + 0.5) - centre) / half;
                    QPointF d = u;
                    for (int it = 0; it < 8; ++it)
                        d = u * (1 + k * (d.x() * d.x() + d.y() * d.y()));
                    const QPointF from = centre + d * half - QPointF(0.5, 0.5);
                    const int ix = int(std::floor(from.x())), iy = int(std::floor(from.y()));
                    const float tx = float(from.x() - ix), ty = float(from.y() - iy);
                    for (int ch = 0; ch < 3; ++ch) {
                        const Plane &p = bent[size_t(ch)];
                        c[size_t(ch)](x, y) = (p.at(ix, iy) * (1 - tx) + p.at(ix + 1, iy) * tx) * (1 - ty) + (p.at(ix, iy + 1) * (1 - tx) + p.at(ix + 1, iy + 1) * tx) * ty;
                    }
                }
        });
        Q_UNUSED(size)
    }
    if (s.fullRange) {
        // 16 is black and 235 white in a video's limited range: stretched to the full range.
        for (Plane &p : c)
            for (float &v : p.v)
                v = (v - 16 / 255.0f) / (219 / 255.0f);
    }
    if (s.field == 1 || s.field == 2) {
        // One half-picture, every other line, each line shown twice.
        const int keep = s.field - 1;
        for (Plane &p : c)
            for (int y = 0; y < p.h; ++y) {
                const int from = std::clamp(y - ((y - keep) % 2 + 2) % 2, 0, p.h - 1);
                if (from != y)
                    for (int x = 0; x < p.w; ++x)
                        p(x, y) = p(x, from);
            }
    }
    if (s.deinterlace) {
        for (Plane &p : c)
            deinterlace(p);
    }
    if (s.deblock > 0) {
        for (Plane &p : c)
            deblock(p, std::clamp(s.deblock, 0, 100));
        if (step(5))
            return {};
    }
    if (s.denoise > 0) {
        for (Plane &p : c)
            p = median(p, s.denoise);
        if (step(10))
            return {};
    }
    if (s.nlm > 0) {
        nonLocalMeans(c, lightOf(c), std::clamp(s.nlm, 0, 100));
        if (step(14))
            return {};
    }
    if (s.smooth > 0) {
        bilateral(c, lightOf(c), std::clamp(s.smooth, 0, 100));
        if (step(15))
            return {};
    }
    // Blur taken out of the brightness only (where the detail is), then given back to each colour.
    if (s.focus > 0 || s.motion > 0) {
        const Plane y = lightOf(c);
        Plane sharp = y;
        const int rounds = std::clamp(s.rounds, 1, 60);
        // Wiener's allowance for grain: 1 is almost none (sharpest, grainiest), 100 a lot.
        const float noise = float(std::pow(10.0, -4.0 + std::clamp(s.noise, 1, 100) / 100.0 * 3.0));
        if (s.focus > 0) {
            int size = 0;
            const std::vector<float> kernel = s.focusDisc ? discKernel(std::clamp(s.focus, 1, 15), &size) : gaussianKernel2D(std::clamp(s.focus, 1, 15) / 2.0, &size);
            if (s.wiener)
                sharp = wienerDeblur(sharp, kernel, size, noise);
            else if (s.focusDisc)
                sharp = deblur(sharp, [&kernel, size](const Plane &p) { return convolve(p, kernel, size); }, rounds, [&] { return step(20); });
            else {
                const double sigma = std::clamp(s.focus, 1, 15) / 2.0;
                sharp = deblur(sharp, [sigma](const Plane &p) { return gaussian(p, sigma); }, rounds, [&] { return step(20); });
            }
            if (stopped)
                return {};
        }
        if (s.motion > 0) {
            const int length = std::clamp(s.motion, 2, 60), angle = s.motionAngle;
            if (s.wiener) {
                int size = 0;
                sharp = wienerDeblur(sharp, lineKernel(length, angle, &size), size, noise);
            } else {
                sharp = deblur(sharp, [length, angle](const Plane &p) { return motionBlur(p, length, angle); }, rounds, [&] { return step(40); });
            }
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
    if (s.homomorphic > 0 || s.retinex > 0) {
        // On the brightness; each colour keeps its share of it.
        const Plane light = lightOf(c);
        Plane shaped = light;
        if (s.homomorphic > 0)
            shaped = homomorphicFilter(shaped, std::clamp(s.homomorphic, 0, 100));
        if (s.retinex > 0) {
            const Plane r = retinexFilter(shaped);
            const float amount = std::clamp(s.retinex, 0, 100) / 100.0f;
            for (size_t i = 0; i < shaped.v.size(); ++i)
                shaped.v[i] = shaped.v[i] * (1 - amount) + r.v[i] * amount;
        }
        for (size_t i = 0; i < light.v.size(); ++i) {
            const float ratio = std::min(shaped.v[i] / std::max(light.v[i], 1e-3f), 20.0f);
            for (Plane &p : c)
                p.v[i] = std::clamp(p.v[i] * ratio, 0.0f, 1.0f);
        }
        if (step(83))
            return {};
    }
    if (s.localContrast > 0) {
        // On the brightness, given back to each colour as it changed.
        const Plane light = lightOf(c);
        const Plane spread = clahe(light, std::clamp(s.localContrast, 0, 100));
        const float amount = 0.4f + s.localContrast / 100.0f * 0.6f;
        for (size_t i = 0; i < light.v.size(); ++i) {
            const float delta = (spread.v[i] - light.v[i]) * amount;
            for (Plane &p : c)
                p.v[i] += delta;
        }
        if (step(85))
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
    if (s.channel != Settings::AllColour) {
        // One part of the colour, as grey, spread over black to white.
        Plane one{w, h, std::vector<float>(size_t(w) * size_t(h))};
        float axis[3] = {0.299f, 0.587f, 0.114f};
        if (s.channel == Settings::MostContrast) {
            // The direction in colour the picture varies most along (its first principal component).
            double mean[3] = {0, 0, 0}, cov[3][3] = {};
            const size_t n = one.v.size();
            for (size_t i = 0; i < n; ++i)
                for (int k = 0; k < 3; ++k)
                    mean[k] += c[size_t(k)].v[i] / double(n);
            for (size_t i = 0; i < n; ++i)
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 3; ++b)
                        cov[a][b] += (c[size_t(a)].v[i] - mean[a]) * (c[size_t(b)].v[i] - mean[b]);
            double vec[3] = {1, 1, 1};
            for (int round = 0; round < 50; ++round) {
                double next[3] = {0, 0, 0}, len = 0;
                for (int a = 0; a < 3; ++a) {
                    for (int b = 0; b < 3; ++b)
                        next[a] += cov[a][b] * vec[b];
                    len += next[a] * next[a];
                }
                len = std::sqrt(len);
                if (len < 1e-12)
                    break;
                for (int a = 0; a < 3; ++a)
                    vec[a] = next[a] / len;
            }
            for (int k = 0; k < 3; ++k)
                axis[k] = float(vec[k]);
        }
        for (size_t i = 0; i < one.v.size(); ++i) {
            const float r = c[0].v[i], g = c[1].v[i], b = c[2].v[i];
            const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
            float v = 0;
            switch (s.channel) {
            case Settings::Red: v = r; break;
            case Settings::Green: v = g; break;
            case Settings::Blue: v = b; break;
            case Settings::Brightness: v = 0.299f * r + 0.587f * g + 0.114f * b; break;
            case Settings::BlueDifference: v = 0.5f - 0.168736f * r - 0.331264f * g + 0.5f * b; break;
            case Settings::RedDifference: v = 0.5f + 0.5f * r - 0.418688f * g - 0.081312f * b; break;
            case Settings::Hue: {
                float hue = 0;
                if (mx > mn) {
                    if (mx == r) hue = std::fmod((g - b) / (mx - mn), 6.0f);
                    else if (mx == g) hue = (b - r) / (mx - mn) + 2;
                    else hue = (r - g) / (mx - mn) + 4;
                }
                v = (hue < 0 ? hue + 6 : hue) / 6;
                break;
            }
            case Settings::Saturation: v = mx > 0 ? (mx - mn) / mx : 0; break;
            case Settings::Value: v = mx; break;
            default: v = axis[0] * r + axis[1] * g + axis[2] * b; break;
            }
            one.v[i] = v;
        }
        const auto [lo, hi] = std::minmax_element(one.v.begin(), one.v.end());
        const float low = *lo, span = std::max(*hi - *lo, 1e-4f);
        for (size_t i = 0; i < one.v.size(); ++i) {
            const float v = (one.v[i] - low) / span;
            c[0].v[i] = c[1].v[i] = c[2].v[i] = v;
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
    QStringList video;
    if (s.fullRange)
        video << QStringLiteral("scale=in_range=limited:out_range=full");
    if (s.field == 1 || s.field == 2)
        video << QStringLiteral("field=type=%1,scale=iw:ih*2:flags=neighbor").arg(s.field == 1 ? QStringLiteral("top") : QStringLiteral("bottom"));
    if (s.deinterlace)
        video << QStringLiteral("yadif");
    if (s.steady)
        video << QStringLiteral("deshake");
    if (s.smooth > 0)
        video << QStringLiteral("hqdn3d=%1").arg(1 + s.smooth / 100.0 * 7, 0, 'f', 1);
    video << QStringLiteral("setpts=PTS/%1").arg(speed, 0, 'f', 3);
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
    factor = std::clamp(factor, 1, 8);
    if (in.isNull() || factor == 1)
        return in;
    const QImage src = in.convertToFormat(QImage::Format_ARGB32);
    const int W = src.width() * factor, H = src.height() * factor;
    QImage out(W, H, QImage::Format_ARGB32);
    if (out.isNull())
        return in;
    rows(H, [&](int y0, int y1) {
        for (int Y = y0; Y < y1; ++Y) {
            const float sy = (Y + 0.5f) / factor - 0.5f;
            QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(Y));
            for (int X = 0; X < W; ++X)
                line[X] = sample(src, (X + 0.5f) / factor - 0.5f, sy);
        }
    });
    return out;
}

QImage resize(const QImage &in, QSize to)
{
    if (in.isNull() || to.isEmpty() || qint64(to.width()) * to.height() > 100000000LL)
        return {};
    const QImage src = in.convertToFormat(QImage::Format_ARGB32);
    QImage out(to, QImage::Format_ARGB32);
    const float fx = float(src.width()) / to.width(), fy = float(src.height()) / to.height();
    rows(to.height(), [&](int y0, int y1) {
        for (int Y = y0; Y < y1; ++Y) {
            QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(Y));
            const float sy = (Y + 0.5f) * fy - 0.5f;
            for (int X = 0; X < to.width(); ++X)
                line[X] = sample(src, (X + 0.5f) * fx - 0.5f, sy);
        }
    });
    return out;
}

QImage straighten(const QImage &in, const QPolygonF &corners)
{
    if (corners.size() != 4)
        return {};
    // As wide as its longer top or bottom, as tall as its longer side.
    const auto length = [](QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); };
    const int W = int(std::lround(std::max(length(corners[0], corners[1]), length(corners[3], corners[2]))));
    const int H = int(std::lround(std::max(length(corners[0], corners[3]), length(corners[1], corners[2]))));
    return straighten(in, corners, QSize(W, H));
}

QImage straighten(const QImage &in, const QPolygonF &corners, QSize size)
{
    const int W = size.width(), H = size.height();
    if (in.isNull() || corners.size() != 4 || W < 2 || H < 2 || qint64(W) * H > 100000000LL)
        return {};
    QTransform back;
    const QPolygonF square{QPointF(0, 0), QPointF(W, 0), QPointF(W, H), QPointF(0, H)};
    if (!QTransform::quadToQuad(square, corners, back))
        return {};
    const QImage src = in.convertToFormat(QImage::Format_ARGB32);
    QImage out(W, H, QImage::Format_ARGB32);
    rows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
            for (int x = 0; x < W; ++x) {
                const QPointF at = back.map(QPointF(x + 0.5, y + 0.5));
                line[x] = sample(src, float(at.x() - 0.5), float(at.y() - 0.5));
            }
        }
    });
    return out;
}

namespace {

// How far `moving` is from `fixed` (both brightness, the same size): the shift, to a tenth of a
// pixel, that makes them most alike in the middle of the picture. A search over whole pixels on
// a quarter-size copy, then whole pixels near it, then a parabola through the best and its
// neighbours for the fraction.
QPointF lineUp(const Plane &fixed, const Plane &moving)
{
    const int w = fixed.w, h = fixed.h;
    const int margin = std::max(2, std::min(w, h) / 6);
    const auto differs = [&](double dx, double dy, int step) {
        double sum = 0;
        int n = 0;
        const int ix = int(std::floor(dx)), iy = int(std::floor(dy));
        const float tx = float(dx - ix), ty = float(dy - iy);
        for (int y = margin; y < h - margin; y += step) {
            for (int x = margin; x < w - margin; x += step) {
                const float m = (moving.at(x + ix, y + iy) * (1 - tx) + moving.at(x + ix + 1, y + iy) * tx) * (1 - ty)
                              + (moving.at(x + ix, y + iy + 1) * (1 - tx) + moving.at(x + ix + 1, y + iy + 1) * tx) * ty;
                const float d = m - fixed.at(x, y);
                sum += d * d;
                ++n;
            }
        }
        return n ? sum / n : 1e9;
    };
    const int reach = std::max(2, margin - 1);
    const int coarse = std::max(1, std::min(w, h) / 120);
    int bx = 0, by = 0;
    double best = 1e18;
    for (int dy = -reach; dy <= reach; dy += coarse) {
        for (int dx = -reach; dx <= reach; dx += coarse) {
            const double d = differs(dx, dy, std::max(1, coarse * 2));
            if (d < best) {
                best = d;
                bx = dx;
                by = dy;
            }
        }
    }
    for (int round = 0; round < 2; ++round) {
        int nx = bx, ny = by;
        best = differs(bx, by, 1);
        for (int dy = -coarse; dy <= coarse; ++dy) {
            for (int dx = -coarse; dx <= coarse; ++dx) {
                const double d = differs(bx + dx, by + dy, 1);
                if (d < best) {
                    best = d;
                    nx = bx + dx;
                    ny = by + dy;
                }
            }
        }
        bx = nx;
        by = ny;
    }
    // The fraction: a finer and finer search around the best whole pixel.
    const int every = std::max(1, std::min(w, h) / 300);
    double fx = bx, fy = by;
    for (const double stride : {0.25, 0.05}) {
        double nx = fx, ny = fy;
        best = differs(fx, fy, every);
        const int steps = stride > 0.1 ? 2 : 4;
        for (int j = -steps; j <= steps; ++j) {
            for (int i = -steps; i <= steps; ++i) {
                const double d = differs(fx + i * stride, fy + j * stride, every);
                if (d < best) {
                    best = d;
                    nx = fx + i * stride;
                    ny = fy + j * stride;
                }
            }
        }
        fx = nx;
        fy = ny;
    }
    return {fx, fy};
}

Plane lightOf(const QImage &image)
{
    const QImage src = image.convertToFormat(QImage::Format_ARGB32);
    Plane p{src.width(), src.height(), std::vector<float>(size_t(src.width()) * size_t(src.height()))};
    for (int y = 0; y < src.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        for (int x = 0; x < src.width(); ++x)
            p(x, y) = qGray(line[x]) / 255.0f;
    }
    return p;
}

} // namespace

QImage stack(const QVector<QImage> &frames, int reference, int scale, Combine how, QVector<QPointF> *shifts, const std::function<bool(int percent)> &progress)
{
    if (frames.isEmpty() || reference < 0 || reference >= frames.size())
        return {};
    const QSize size = frames[reference].size();
    QVector<QImage> same;
    for (const QImage &f : frames) {
        if (f.size() == size)
            same << f.convertToFormat(QImage::Format_ARGB32);
    }
    if (same.isEmpty())
        return {};
    reference = std::min(reference, int(same.size()) - 1);
    scale = std::clamp(scale, 1, 2);
    // How far each one is from the reference.
    const Plane fixed = lightOf(same[reference]);
    QVector<QPointF> moved(same.size());
    for (int i = 0; i < same.size(); ++i) {
        if (i != reference)
            moved[i] = lineUp(fixed, lightOf(same[i]));
        if (progress && !progress(i * 50 / int(same.size())))
            return {};
    }
    if (shifts)
        *shifts = moved;
    // Each point of the result, from every frame at the same place in what was filmed.
    const int W = size.width() * scale, H = size.height() * scale;
    QImage out(W, H, QImage::Format_ARGB32);
    if (out.isNull())
        return {};
    std::atomic<bool> stopped = false;
    rows(H, [&](int y0, int y1) {
        std::vector<int> r(size_t(same.size())), g(r), b(r);
        for (int Y = y0; Y < y1 && !stopped; ++Y) {
            QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(Y));
            const float sy = (Y + 0.5f) / scale - 0.5f;
            for (int X = 0; X < W; ++X) {
                const float sx = (X + 0.5f) / scale - 0.5f;
                for (int i = 0; i < same.size(); ++i) {
                    const QRgb p = sample(same[i], sx + float(moved[i].x()), sy + float(moved[i].y()));
                    r[size_t(i)] = qRed(p);
                    g[size_t(i)] = qGreen(p);
                    b[size_t(i)] = qBlue(p);
                }
                const auto combine = [how](std::vector<int> &v) {
                    if (how == Combine::Middle) {
                        std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(v.size() / 2), v.end());
                        return v[v.size() / 2];
                    }
                    double sum = 0, sq = 0;
                    for (int x : v) {
                        sum += x;
                        sq += double(x) * x;
                    }
                    const double mean = sum / double(v.size());
                    if (how == Combine::AverageWithoutOutliers && v.size() > 3) {
                        // The ones more than twice the spread from the average left out.
                        const double spread = std::sqrt(std::max(0.0, sq / double(v.size()) - mean * mean));
                        double kept = 0;
                        int n = 0;
                        for (int x : v) {
                            if (std::abs(x - mean) <= 2 * spread + 1) {
                                kept += x;
                                ++n;
                            }
                        }
                        if (n > 0)
                            return int(std::lround(kept / n));
                    }
                    return int(std::lround(mean));
                };
                line[X] = qRgb(combine(r), combine(g), combine(b));
            }
            if (progress && y0 == 0 && !progress(50 + Y * 50 / std::max(1, y1)))
                stopped = true;
        }
    });
    return stopped ? QImage() : out;
}

double sharpness(const QImage &image)
{
    const QImage grey = image.convertToFormat(QImage::Format_Grayscale8);
    double sum = 0, sq = 0;
    long n = 0;
    for (int y = 1; y + 1 < grey.height(); ++y) {
        const uchar *above = grey.constScanLine(y - 1), *line = grey.constScanLine(y), *below = grey.constScanLine(y + 1);
        for (int x = 1; x + 1 < grey.width(); ++x, ++n) {
            const double l = above[x] + below[x] + line[x - 1] + line[x + 1] - 4.0 * line[x];
            sum += l;
            sq += l * l;
        }
    }
    return n ? sq / double(n) - (sum / double(n)) * (sum / double(n)) : 0;
}

QVector<QPolygonF> followCorners(const QVector<QImage> &frames, int reference, const QPolygonF &corners)
{
    QVector<QPolygonF> out(frames.size());
    if (frames.isEmpty() || reference < 0 || reference >= frames.size() || corners.size() != 4)
        return out;
    QVector<QImage> grey;
    for (const QImage &f : frames)
        grey << f.convertToFormat(QImage::Format_Grayscale8);
    constexpr int patch = 12, reach = 14;
    const auto value = [](const QImage &g, double x, double y) {
        const int ix = int(std::floor(x)), iy = int(std::floor(y));
        const double tx = x - ix, ty = y - iy;
        const auto at = [&g](int px, int py) { return double(g.constScanLine(std::clamp(py, 0, g.height() - 1))[std::clamp(px, 0, g.width() - 1)]); };
        return (at(ix, iy) * (1 - tx) + at(ix + 1, iy) * tx) * (1 - ty) + (at(ix, iy + 1) * (1 - tx) + at(ix + 1, iy + 1) * tx) * ty;
    };
    // One corner from one frame to the next: the shift whose patch matches best (mean taken off
    // each, so a change of light doesn't matter), then a fraction of a pixel either way.
    const auto track = [&](const QImage &from, QPointF at, const QImage &to, QPointF guess) {
        std::vector<double> ref;
        double refMean = 0;
        for (int j = -patch; j <= patch; ++j)
            for (int i = -patch; i <= patch; ++i) {
                ref.push_back(value(from, at.x() + i, at.y() + j));
                refMean += ref.back();
            }
        refMean /= double(ref.size());
        const auto cost = [&](double dx, double dy) {
            double s = 0, mean = 0;
            std::vector<double> here;
            here.reserve(ref.size());
            for (int j = -patch; j <= patch; ++j)
                for (int i = -patch; i <= patch; ++i) {
                    here.push_back(value(to, guess.x() + dx + i, guess.y() + dy + j));
                    mean += here.back();
                }
            mean /= double(here.size());
            for (size_t k = 0; k < here.size(); ++k) {
                const double d = (here[k] - mean) - (ref[k] - refMean);
                s += d * d;
            }
            return s;
        };
        double best = 1e300, bx = 0, by = 0;
        for (int dy = -reach; dy <= reach; ++dy)
            for (int dx = -reach; dx <= reach; ++dx) {
                const double c = cost(dx, dy);
                if (c < best) {
                    best = c;
                    bx = dx;
                    by = dy;
                }
            }
        for (const double stride : {0.25, 0.05}) {
            double nx = bx, ny = by;
            for (int j = -2; j <= 2; ++j)
                for (int i = -2; i <= 2; ++i) {
                    const double c = cost(bx + i * stride, by + j * stride);
                    if (c < best) {
                        best = c;
                        nx = bx + i * stride;
                        ny = by + j * stride;
                    }
                }
            bx = nx;
            by = ny;
        }
        return guess + QPointF(bx, by);
    };
    out[reference] = corners;
    // Outwards from the reference, each frame matched against the reference itself (so small
    // errors don't add up frame by frame), starting from where the frame next to it had them.
    for (const int dir : {1, -1}) {
        for (int i = reference + dir; i >= 0 && i < frames.size(); i += dir) {
            QPolygonF here;
            for (int k = 0; k < 4; ++k)
                here << track(grey[reference], corners[k], grey[i], out[i - dir][k]);
            out[i] = here;
        }
    }
    return out;
}

QPointF unbend(QPointF point, QSize picture, int fisheye)
{
    // Division model: undistorted = centre + (p - centre) / (1 + k r^2), r measured against half
    // the picture's diagonal so the strength means the same at any size.
    const QPointF centre(picture.width() / 2.0, picture.height() / 2.0);
    const double half = std::hypot(picture.width(), picture.height()) / 2;
    const double k = -std::clamp(fisheye, 0, 100) / 100.0 * 0.45;
    const QPointF d = (point - centre) / half;
    const double r2 = d.x() * d.x() + d.y() * d.y();
    return centre + d * half / (1 + k * r2);
}

int fisheyeFromLine(const QVector<QPointF> &points, QSize picture)
{
    if (points.size() < 3)
        return -1;
    // The strength that leaves the points nearest a straight line (the line through the first and last).
    int best = 0;
    double bestOff = 1e300;
    for (int f = 0; f <= 100; ++f) {
        QVector<QPointF> p;
        for (const QPointF &q : points)
            p << unbend(q, picture, f);
        const QPointF a = p.first(), b = p.last();
        const double len = std::hypot(b.x() - a.x(), b.y() - a.y());
        if (len < 1)
            return -1;
        double off = 0;
        for (const QPointF &q : p)
            off += std::abs((b.x() - a.x()) * (a.y() - q.y()) - (a.x() - q.x()) * (b.y() - a.y())) / len;
        if (off < bestOff) {
            bestOff = off;
            best = f;
        }
    }
    return best;
}

QImage clippedOverlay(const QImage &picture)
{
    const QImage src = picture.convertToFormat(QImage::Format_ARGB32);
    QImage out(src.size(), QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    for (int y = 0; y < src.height(); ++y) {
        const QRgb *from = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        QRgb *to = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            const int r = qRed(from[x]), g = qGreen(from[x]), b = qBlue(from[x]);
            if (r >= 254 || g >= 254 || b >= 254)
                to[x] = qRgba(255, 30, 60, 200);
            else if (r <= 1 && g <= 1 && b <= 1)
                to[x] = qRgba(40, 110, 255, 200);
        }
    }
    return out;
}

QString channelName(int channel)
{
    switch (channel) {
    case Settings::Red: return tr("red");
    case Settings::Green: return tr("green");
    case Settings::Blue: return tr("blue");
    case Settings::Brightness: return tr("brightness (Y)");
    case Settings::BlueDifference: return tr("blue difference (Cb)");
    case Settings::RedDifference: return tr("red difference (Cr)");
    case Settings::Hue: return tr("hue");
    case Settings::Saturation: return tr("saturation");
    case Settings::Value: return tr("value (V)");
    case Settings::MostContrast: return tr("most contrast (principal component)");
    default: return {};
    }
}

} // namespace enhance
