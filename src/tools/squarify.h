// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Squarified treemap layout (Bruls, Huizing and van Wijk, 2000): boxes with areas in
// proportion to `sizes` (biggest first), kept as close to square as it can, filling
// `rect`. Zero sizes get an empty box.

#include <QRectF>
#include <QVector>

#include <algorithm>
#include <limits>

inline QVector<QRectF> squarify(const QVector<double> &sizes, const QRectF &rect)
{
    QVector<QRectF> out(sizes.size());
    int n = 0;
    double total = 0;
    while (n < sizes.size() && sizes[n] > 0)
        total += sizes[n++];
    if (total <= 0 || rect.isEmpty())
        return out;
    const double scale = rect.width() * rect.height() / total;

    // The worst aspect ratio in a row of boxes laid along a side of length `side`.
    auto worst = [](double largest, double smallest, double sum, double side) {
        const double s2 = sum * sum, w2 = side * side;
        return std::max(w2 * largest / s2, s2 / (w2 * smallest));
    };

    QRectF free = rect;
    int i = 0;
    while (i < n) {
        const double side = std::min(free.width(), free.height());
        int end = i;
        double rowSum = 0, best = std::numeric_limits<double>::infinity();
        while (end < n) {
            const double sum = rowSum + sizes[end] * scale;
            const double w = worst(sizes[i] * scale, sizes[end] * scale, sum, side);
            if (w > best)
                break;
            best = w;
            rowSum = sum;
            ++end;
        }
        const double thickness = rowSum / side;
        double along = 0;
        for (int k = i; k < end; ++k) {
            const double length = sizes[k] * scale / thickness;
            if (free.width() >= free.height())
                out[k] = QRectF(free.left(), free.top() + along, thickness, length);
            else
                out[k] = QRectF(free.left() + along, free.top(), length, thickness);
            along += length;
        }
        if (free.width() >= free.height())
            free.setLeft(free.left() + thickness);
        else
            free.setTop(free.top() + thickness);
        i = end;
    }
    return out;
}
