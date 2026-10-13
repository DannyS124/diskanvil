// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jpegexact.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QImageReader>
#include <QTransform>

#ifdef DISKANVIL_JPEG
#include <csetjmp>
#include <cstdio>
#include <jpeglib.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace jpegexact {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("jpegexact", text);
}

} // namespace

#ifdef DISKANVIL_JPEG

namespace {

struct Error {
    jpeg_error_mgr base;
    std::jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
};

void onError(j_common_ptr info)
{
    auto *e = reinterpret_cast<Error *>(info->err);
    (*info->err->format_message)(info, e->message);
    std::longjmp(e->jump, 1);
}

// The 8 by 8 DCT and its inverse, as JPEG defines them (orthonormal), from a table of cosines.
struct Dct {
    float c[8][8];
    Dct()
    {
        for (int u = 0; u < 8; ++u)
            for (int x = 0; x < 8; ++x)
                c[u][x] = float((u == 0 ? std::sqrt(1.0 / 8) : std::sqrt(2.0 / 8)) * std::cos((2 * x + 1) * u * std::numbers::pi / 16));
    }
    void forward(const float in[64], float out[64]) const
    {
        float t[64];
        for (int y = 0; y < 8; ++y)
            for (int u = 0; u < 8; ++u) {
                float s = 0;
                for (int x = 0; x < 8; ++x)
                    s += c[u][x] * in[y * 8 + x];
                t[y * 8 + u] = s;
            }
        for (int v = 0; v < 8; ++v)
            for (int u = 0; u < 8; ++u) {
                float s = 0;
                for (int y = 0; y < 8; ++y)
                    s += c[v][y] * t[y * 8 + u];
                out[v * 8 + u] = s;
            }
    }
    void inverse(const float in[64], float out[64]) const
    {
        float t[64];
        for (int v = 0; v < 8; ++v)
            for (int x = 0; x < 8; ++x) {
                float s = 0;
                for (int u = 0; u < 8; ++u)
                    s += c[u][x] * in[v * 8 + u];
                t[v * 8 + x] = s;
            }
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                float s = 0;
                for (int v = 0; v < 8; ++v)
                    s += c[v][y] * t[v * 8 + x];
                out[y * 8 + x] = s;
            }
    }
};

// One component: its blocks' stored numbers and tables, and the picture being worked on, at the
// component's own size (a whole number of blocks).
struct Component {
    int blocksWide = 0, blocksHigh = 0, width = 0, height = 0;
    int hSample = 1, vSample = 1;
    std::vector<std::array<short, 64>> stored; // the file's numbers, zigzag undone (natural order)
    std::array<unsigned short, 64> quant{};
    std::vector<float> pixels;                 // blocksWide*8 by blocksHigh*8, centred on 0 (-128..127)
};

// What each block's stored numbers mean: k times the table's step, with everything from
// (k - 1/2) to (k + 1/2) steps rounding to it.
void project(Component &c, const Dct &dct)
{
    const int stride = c.blocksWide * 8;
    for (int by = 0; by < c.blocksHigh; ++by)
        for (int bx = 0; bx < c.blocksWide; ++bx) {
            float block[64], coef[64];
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x)
                    block[y * 8 + x] = c.pixels[size_t((by * 8 + y) * stride + bx * 8 + x)];
            dct.forward(block, coef);
            const auto &k = c.stored[size_t(by * c.blocksWide + bx)];
            for (int i = 0; i < 64; ++i) {
                const float q = c.quant[size_t(i)];
                coef[i] = std::clamp(coef[i], (k[size_t(i)] - 0.5f) * q, (k[size_t(i)] + 0.5f) * q);
            }
            dct.inverse(coef, block);
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x)
                    c.pixels[size_t((by * 8 + y) * stride + bx * 8 + x)] = block[y * 8 + x];
        }
}

// A small step towards less total variation (smoother, edges kept): each point moved by the
// divergence of its normalised gradient.
void smooth(Component &c, float step)
{
    const int w = c.blocksWide * 8, h = c.blocksHigh * 8;
    const auto at = [&](int x, int y) { return c.pixels[size_t(std::clamp(y, 0, h - 1) * w + std::clamp(x, 0, w - 1))]; };
    std::vector<float> gx(c.pixels.size()), gy(c.pixels.size());
    constexpr float eps = 1.0f;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float dx = at(x + 1, y) - at(x, y), dy = at(x, y + 1) - at(x, y);
            const float n = std::sqrt(dx * dx + dy * dy + eps);
            gx[size_t(y * w + x)] = dx / n;
            gy[size_t(y * w + x)] = dy / n;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float div = gx[size_t(y * w + x)] - (x > 0 ? gx[size_t(y * w + x - 1)] : 0) + gy[size_t(y * w + x)] - (y > 0 ? gy[size_t((y - 1) * w + x)] : 0);
            c.pixels[size_t(y * w + x)] += step * div;
        }
}

} // namespace

bool available()
{
    return true;
}

QImage decode(const QByteArray &file, int rounds, QString *why, const std::function<bool(int percent)> &progress)
{
    jpeg_decompress_struct info;
    Error error;
    info.err = jpeg_std_error(&error.base);
    error.base.error_exit = onError;
    error.message[0] = 0;
    std::vector<Component> comps;
    int width = 0, height = 0, maxH = 1, maxV = 1;
    J_COLOR_SPACE space = JCS_UNKNOWN;
    if (setjmp(error.jump)) {
        jpeg_destroy_decompress(&info);
        *why = tr("It isn't a JPEG this can decode again: %1").arg(QString::fromLatin1(error.message));
        return {};
    }
    jpeg_create_decompress(&info);
    jpeg_mem_src(&info, reinterpret_cast<const unsigned char *>(file.constData()), static_cast<unsigned long>(file.size()));
    jpeg_read_header(&info, TRUE);
    space = info.jpeg_color_space;
    if (!(space == JCS_YCbCr && info.num_components == 3) && !(space == JCS_GRAYSCALE && info.num_components == 1)) {
        jpeg_destroy_decompress(&info);
        *why = tr("Only colour (YCbCr) and grey JPEGs are decoded again; this one is another kind (CMYK, or unusual).");
        return {};
    }
    if (qint64(info.image_width) * info.image_height > 60000000LL) {
        jpeg_destroy_decompress(&info);
        *why = tr("It's too big to decode again here (over 60 million pixels).");
        return {};
    }
    jvirt_barray_ptr *arrays = jpeg_read_coefficients(&info);
    width = int(info.image_width);
    height = int(info.image_height);
    for (int i = 0; i < info.num_components; ++i) {
        maxH = std::max(maxH, info.comp_info[i].h_samp_factor);
        maxV = std::max(maxV, info.comp_info[i].v_samp_factor);
    }
    for (int i = 0; i < info.num_components; ++i) {
        jpeg_component_info *ci = &info.comp_info[i];
        Component c;
        c.blocksWide = int(ci->width_in_blocks);
        c.blocksHigh = int(ci->height_in_blocks);
        c.hSample = ci->h_samp_factor;
        c.vSample = ci->v_samp_factor;
        c.width = int(std::ceil(double(width) * ci->h_samp_factor / maxH));
        c.height = int(std::ceil(double(height) * ci->v_samp_factor / maxV));
        if (!ci->quant_table) {
            jpeg_destroy_decompress(&info);
            *why = tr("Its tables are missing.");
            return {};
        }
        for (int k = 0; k < 64; ++k)
            c.quant[size_t(k)] = ci->quant_table->quantval[k];
        c.stored.resize(size_t(c.blocksWide) * size_t(c.blocksHigh));
        for (int by = 0; by < c.blocksHigh; ++by) {
            JBLOCKARRAY rowsOf = (*info.mem->access_virt_barray)(reinterpret_cast<j_common_ptr>(&info), arrays[i], JDIMENSION(by), 1, FALSE);
            for (int bx = 0; bx < c.blocksWide; ++bx)
                for (int k = 0; k < 64; ++k)
                    c.stored[size_t(by * c.blocksWide + bx)][size_t(k)] = rowsOf[0][bx][k];
        }
        comps.push_back(std::move(c));
    }
    jpeg_finish_decompress(&info);
    jpeg_destroy_decompress(&info);

    // First what an ordinary decoder shows (the middle of each rounding), then smoothed and put
    // back within the file's numbers, again and again.
    const Dct dct;
    const int passes = std::clamp(rounds, 1, 100);
    for (Component &c : comps) {
        const int stride = c.blocksWide * 8;
        c.pixels.assign(size_t(stride) * size_t(c.blocksHigh * 8), 0);
        for (int by = 0; by < c.blocksHigh; ++by)
            for (int bx = 0; bx < c.blocksWide; ++bx) {
                float coef[64], block[64];
                for (int k = 0; k < 64; ++k)
                    coef[k] = float(c.stored[size_t(by * c.blocksWide + bx)][size_t(k)]) * c.quant[size_t(k)];
                dct.inverse(coef, block);
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x)
                        c.pixels[size_t((by * 8 + y) * stride + bx * 8 + x)] = block[y * 8 + x];
            }
    }
    for (int r = 0; r < passes; ++r) {
        for (Component &c : comps) {
            for (int s = 0; s < 1; ++s)
                smooth(c, 1.0f);
            project(c, dct);
        }
        if (progress && !progress(r * 100 / passes))
            return {};
    }

    // Each component at the picture's size (smoothly, where colour was kept at half size), then
    // to RGB as JFIF has it.
    const auto value = [&](const Component &c, int x, int y) {
        // The sample at the centre of the full-size pixel x, y.
        const float fx = (x + 0.5f) * c.hSample / maxH - 0.5f, fy = (y + 0.5f) * c.vSample / maxV - 0.5f;
        const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
        const float tx = fx - ix, ty = fy - iy;
        const int stride = c.blocksWide * 8, w = c.width, h = c.height;
        const auto at = [&](int px, int py) { return c.pixels[size_t(std::clamp(py, 0, h - 1) * stride + std::clamp(px, 0, w - 1))]; };
        return (at(ix, iy) * (1 - tx) + at(ix + 1, iy) * tx) * (1 - ty) + (at(ix, iy + 1) * (1 - tx) + at(ix + 1, iy + 1) * tx) * ty + 128.0f;
    };
    QImage out(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const float Y = value(comps[0], x, y);
            if (comps.size() == 1) {
                const int g = std::clamp(int(std::lround(Y)), 0, 255);
                line[x] = qRgb(g, g, g);
                continue;
            }
            const float cb = value(comps[1], x, y) - 128, cr = value(comps[2], x, y) - 128;
            line[x] = qRgb(std::clamp(int(std::lround(Y + 1.402f * cr)), 0, 255), std::clamp(int(std::lround(Y - 0.344136f * cb - 0.714136f * cr)), 0, 255),
                           std::clamp(int(std::lround(Y + 1.772f * cb)), 0, 255));
        }
    }
    // Turned the way the camera was held, as the viewer shows it.
    QByteArray copy = file;
    QBuffer buffer(&copy);
    QImageReader reader(&buffer, "jpeg");
    const QImageIOHandler::Transformations t = reader.transformation();
    QTransform turn;
    if (t & QImageIOHandler::TransformationRotate90)
        turn.rotate(90);
    QImage turned = out;
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    if (t & QImageIOHandler::TransformationMirror)
        turned = turned.flipped(Qt::Horizontal);
    if (t & QImageIOHandler::TransformationFlip)
        turned = turned.flipped(Qt::Vertical);
#else
    turned = turned.mirrored(t & QImageIOHandler::TransformationMirror, t & QImageIOHandler::TransformationFlip);
#endif
    if (t & QImageIOHandler::TransformationRotate90)
        turned = turned.transformed(turn);
    return turned;
}

#else

bool available()
{
    return false;
}

QImage decode(const QByteArray &, int, QString *why, const std::function<bool(int)> &)
{
    *why = tr("This copy of DiskAnvil was built without libjpeg, so a JPEG can't be decoded again.");
    return {};
}

#endif

} // namespace jpegexact
