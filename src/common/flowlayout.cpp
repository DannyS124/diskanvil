// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "flowlayout.h"

#include <QStyle>
#include <QWidget>

FlowLayout::FlowLayout(QWidget *parent)
    : QLayout(parent)
{
}

FlowLayout::~FlowLayout()
{
    while (QLayoutItem *item = takeAt(0))
        delete item;
}

void FlowLayout::addItem(QLayoutItem *item)
{
    m_items << item;
}

QLayoutItem *FlowLayout::takeAt(int index)
{
    return index >= 0 && index < m_items.size() ? m_items.takeAt(index) : nullptr;
}

QSize FlowLayout::minimumSize() const
{
    QSize size;
    for (const QLayoutItem *item : m_items)
        size = size.expandedTo(item->minimumSize());
    const QMargins m = contentsMargins();
    return size + QSize(m.left() + m.right(), m.top() + m.bottom());
}

QSize FlowLayout::sizeHint() const
{
    // All on one line, when there's room for that.
    int width = 0, height = 0;
    for (const QLayoutItem *item : m_items) {
        if (item->isEmpty())
            continue;
        width += item->sizeHint().width() + (width > 0 ? gap(Qt::Horizontal) : 0);
        height = std::max(height, item->sizeHint().height());
    }
    const QMargins m = contentsMargins();
    return QSize(width + m.left() + m.right(), height + m.top() + m.bottom());
}

void FlowLayout::setGeometry(const QRect &rect)
{
    QLayout::setGeometry(rect);
    arrange(rect, true);
}

int FlowLayout::gap(Qt::Orientation orientation) const
{
    if (spacing() >= 0)
        return spacing();
    const QWidget *owner = parentWidget();
    if (!owner)
        return 6;
    return owner->style()->pixelMetric(orientation == Qt::Horizontal ? QStyle::PM_LayoutHorizontalSpacing : QStyle::PM_LayoutVerticalSpacing,
                                       nullptr, owner);
}

int FlowLayout::arrange(const QRect &rect, bool place) const
{
    const QMargins m = contentsMargins();
    const QRect area = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
    const int across = std::max(0, gap(Qt::Horizontal)), down = std::max(0, gap(Qt::Vertical));
    int x = area.x(), y = area.y(), lineHeight = 0;
    for (QLayoutItem *item : m_items) {
        if (item->isEmpty()) // hidden ones take no room
            continue;
        const QSize size = item->sizeHint();
        if (x > area.x() && x + size.width() > area.right() + 1) {
            x = area.x();
            y += lineHeight + down;
            lineHeight = 0;
        }
        if (place)
            item->setGeometry(QRect(QPoint(x, y), size));
        x += size.width() + across;
        lineHeight = std::max(lineHeight, size.height());
    }
    return y + lineHeight - rect.y() + m.bottom();
}
