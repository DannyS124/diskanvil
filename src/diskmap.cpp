// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "diskmap.h"

#include "format.h"
#include "theme.h"

#include <QContextMenuEvent>
#include <QHelpEvent>
#include <QIcon>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QTextDocument>

#include <algorithm>

namespace {

constexpr int kMargin = 8;
constexpr int kGap = 10;
constexpr int kHeaderWidth = 180;
constexpr int kStrip = 8;
constexpr int kMinSegment = 96;

QString diskIconName(const Disk &d)
{
    if (d.isLoop)
        return QStringLiteral("media-optical");
    if (d.removable || d.bus == QLatin1String("usb"))
        return QStringLiteral("drive-removable-media-usb");
    return QStringLiteral("drive-harddisk");
}

} // namespace

DiskMap::DiskMap(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true); // the hand over a lock
    connect(&Theme::instance(), &Theme::changed, this, qOverload<>(&QWidget::update));
    setMinimumWidth(kHeaderWidth + 3 * kMinSegment);
    setFocusPolicy(Qt::ClickFocus);
}

QSize DiskMap::sizeHint() const
{
    return QSize(900, minimumHeight());
}

void DiskMap::setDisks(const QVector<Disk> &disks)
{
    QString diskPath, volumePath;
    const Selection old = m_sel;
    if (old.kind != Selection::Kind::None) {
        diskPath = m_disks[old.disk].blockPath;
        if (old.kind == Selection::Kind::Volume)
            volumePath = m_disks[old.disk].volumes[old.volume].objectPath;
    }

    m_disks = disks;
    m_sel = {};
    for (int i = 0; i < m_disks.size(); ++i) {
        if (m_disks[i].blockPath != diskPath)
            continue;
        if (old.kind == Selection::Kind::Disk) {
            m_sel.kind = Selection::Kind::Disk;
            m_sel.disk = i;
        } else if (old.kind == Selection::Kind::Volume) {
            selectVolume(volumePath);
        } else if (old.kind == Selection::Kind::Free) {
            for (const Span &s : diskSpans(m_disks[i])) {
                if (s.isFree() && s.offset == old.offset) {
                    m_sel = old;
                    m_sel.disk = i;
                    m_sel.size = s.size;
                }
            }
        }
    }
    relayout();
    update();
}

void DiskMap::selectVolume(const QString &objectPath)
{
    m_sel = {};
    for (int d = 0; d < m_disks.size(); ++d) {
        for (int v = 0; v < m_disks[d].volumes.size(); ++v) {
            if (m_disks[d].volumes[v].objectPath == objectPath) {
                m_sel.kind = Selection::Kind::Volume;
                m_sel.disk = d;
                m_sel.volume = v;
            }
        }
    }
    update();
}

void DiskMap::selectDisk(int index)
{
    m_sel = {};
    if (index >= 0 && index < m_disks.size()) {
        m_sel.kind = Selection::Kind::Disk;
        m_sel.disk = index;
    }
    update();
}

void DiskMap::relayout()
{
    m_rows.clear();
    m_rowHeight = std::max(80, 6 + 24 + 3 * fontMetrics().height() + 8);

    const int barLeft = kMargin + kHeaderWidth + 4;
    const int barWidth = std::max(width() - barLeft - kMargin, 3 * kMinSegment);
    int y = kMargin;
    for (const Disk &disk : std::as_const(m_disks)) {
        Row row;
        row.header = QRect(kMargin, y, kHeaderWidth, m_rowHeight);

        // min width per segment, the rest split by size
        const QVector<Span> spans = diskSpans(disk);
        const int n = spans.size();
        quint64 total = 0;
        for (const Span &s : spans)
            total += s.size;
        const int minWidth = n ? std::min(kMinSegment, barWidth / n) : 0;
        const int spare = barWidth - minWidth * n;
        quint64 before = 0;
        int x = barLeft;
        for (int i = 0; i < n; ++i) {
            before += spans[i].size;
            const int right = barLeft + minWidth * (i + 1) + (total ? int(double(spare) * double(before) / double(total)) : 0);
            row.segments.push_back({spans[i], QRect(x, y, right - x, m_rowHeight)});
            x = right;
        }
        m_rows.push_back(row);
        y += m_rowHeight + kGap;
    }
    setMinimumHeight(m_rows.isEmpty() ? 100 : y - kGap + kMargin);
}

void DiskMap::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    relayout();
}

bool DiskMap::hitTest(const QPoint &pos, int *row, int *segment) const
{
    for (int r = 0; r < m_rows.size(); ++r) {
        if (m_rows[r].header.contains(pos)) {
            *row = r;
            *segment = -1;
            return true;
        }
        for (int s = 0; s < m_rows[r].segments.size(); ++s) {
            if (m_rows[r].segments[s].rect.contains(pos)) {
                *row = r;
                *segment = s;
                return true;
            }
        }
    }
    return false;
}

DiskMap::Selection DiskMap::selectionAt(int row, int segment) const
{
    Selection sel;
    sel.disk = row;
    if (segment < 0) {
        sel.kind = Selection::Kind::Disk;
        return sel;
    }
    const Span &span = m_rows[row].segments[segment].span;
    if (span.isFree()) {
        sel.kind = Selection::Kind::Free;
        sel.offset = span.offset;
        sel.size = span.size;
    } else {
        sel.kind = Selection::Kind::Volume;
        sel.volume = span.volume;
    }
    return sel;
}

bool DiskMap::isSelected(int row, int segment) const
{
    if (m_sel.disk != row)
        return false;
    if (segment < 0)
        return m_sel.kind == Selection::Kind::Disk;
    const Span &span = m_rows[row].segments[segment].span;
    if (span.isFree())
        return m_sel.kind == Selection::Kind::Free && m_sel.offset == span.offset;
    return m_sel.kind == Selection::Kind::Volume && m_sel.volume == span.volume;
}

void DiskMap::mousePressEvent(QMouseEvent *event)
{
    int row, segment;
    m_sel = hitTest(event->position().toPoint(), &row, &segment) ? selectionAt(row, segment) : Selection{};
    update();
    emit selectionChanged();
    // The lock: selected first, so Unlock and Lock work on that partition.
    if (event->button() == Qt::LeftButton && lockAt(event->position().toPoint(), &row, &segment))
        emit lockClicked(m_disks[row].volumes[m_rows[row].segments[segment].span.volume].objectPath);
}

void DiskMap::mouseMoveEvent(QMouseEvent *event)
{
    int row, segment;
    if (lockAt(event->position().toPoint(), &row, &segment))
        setCursor(Qt::PointingHandCursor);
    else
        unsetCursor();
    QWidget::mouseMoveEvent(event);
}

QRect DiskMap::lockRectAt(int row, int segment) const
{
    const Span &span = m_rows[row].segments[segment].span;
    if (span.isFree() || !m_disks[row].volumes[span.volume].encrypted)
        return {};
    const QRect r = m_rows[row].segments[segment].rect.adjusted(segment ? 2 : 0, 0, 0, 0);
    if (r.width() < 56)
        return {};
    return QRect(r.right() - 23, r.top() + kStrip + 5, 18, 18);
}

bool DiskMap::lockAt(const QPoint &pos, int *row, int *segment) const
{
    return hitTest(pos, row, segment) && *segment >= 0 && lockRectAt(*row, *segment).contains(pos);
}

QRect DiskMap::lockRect(const QString &objectPath) const
{
    for (int r = 0; r < m_rows.size(); ++r) {
        for (int s = 0; s < m_rows[r].segments.size(); ++s) {
            const Span &span = m_rows[r].segments[s].span;
            if (!span.isFree() && m_disks[r].volumes[span.volume].objectPath == objectPath)
                return lockRectAt(r, s);
        }
    }
    return {};
}

void DiskMap::mouseDoubleClickEvent(QMouseEvent *event)
{
    int row, segment;
    if (hitTest(event->position().toPoint(), &row, &segment))
        emit activated();
}

void DiskMap::contextMenuEvent(QContextMenuEvent *event)
{
    int row, segment;
    if (hitTest(event->pos(), &row, &segment))
        emit contextMenuRequested(event->globalPos());
}

bool DiskMap::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        int row, segment;
        if (lockAt(help->pos(), &row, &segment)) {
            const Volume &v = m_disks[row].volumes[m_rows[row].segments[segment].span.volume];
            const QString text = v.cleartextPath.isEmpty() ? tr("Encrypted and locked. Click to unlock.")
                                                           : tr("Encrypted, unlocked as %1. Click to lock.").arg(shortDevice(v.cleartextDevice));
            QToolTip::showText(help->globalPos(), Qt::convertFromPlainText(text, Qt::WhiteSpaceNormal), this);
        } else if (hitTest(help->pos(), &row, &segment))
            QToolTip::showText(help->globalPos(), Qt::convertFromPlainText(toolTipAt(row, segment), Qt::WhiteSpaceNormal), this);
        else
            QToolTip::hideText();
        return true;
    }
    return QWidget::event(event);
}

QString DiskMap::toolTipAt(int row, int segment) const
{
    const Disk &d = m_disks[row];
    if (segment < 0) {
        QStringList lines = {d.model, d.device, tableName(d) + QStringLiteral(" · ") + diskKind(d)};
        if (d.isSystem)
            lines << tr("System disk (%1): read-only in this app").arg(d.systemReason);
        if (d.isRaid)
            lines << raidStatus(d);
        if (d.health.state != Health::State::Unknown) {
            QString health = tr("Health: %1").arg(d.health.summary);
            if (d.health.temperatureC > 0)
                health += QStringLiteral(", %1 °C").arg(qRound(d.health.temperatureC));
            lines << health;
        }
        return lines.join(QLatin1Char('\n'));
    }
    const Span &span = m_rows[row].segments[segment].span;
    if (span.isFree())
        return tr("Unallocated: %1\nStarts at %2 bytes").arg(formatSize(span.size)).arg(span.offset);
    const Volume &v = d.volumes[span.volume];
    QStringList lines = {volumeTitle(v), v.device, formatSize(v.size) + QLatin1Char(' ') + v.fsType, volumeStatus(v)};
    if (v.fsTotal)
        lines << tr("%1 free of %2").arg(formatSize(v.fsFree), formatSize(v.fsTotal));
    return lines.join(QLatin1Char('\n'));
}

void DiskMap::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), palette().color(QPalette::Window));
    if (m_disks.isEmpty()) {
        p.drawText(rect(), Qt::AlignCenter, tr("No disks found"));
        return;
    }
    for (int r = 0; r < m_rows.size(); ++r) {
        paintHeader(p, r);
        for (int s = 0; s < m_rows[r].segments.size(); ++s)
            paintSegment(p, r, s);
    }
}

void DiskMap::paintHeader(QPainter &p, int row) const
{
    const Disk &d = m_disks[row];
    const QRect r = m_rows[row].header;
    const QPalette pal = palette();
    const bool selected = isSelected(row, -1);

    p.fillRect(r, pal.color(QPalette::Button));
    if (selected) {
        QColor hatch = pal.color(QPalette::Highlight);
        hatch.setAlpha(110);
        p.fillRect(r, QBrush(hatch, Qt::BDiagPattern));
    }
    p.setPen(selected ? pal.color(QPalette::Highlight) : pal.color(QPalette::Mid));
    p.drawRect(r.adjusted(0, 0, -1, -1));

    const QRect body = r.adjusted(8, 6, -8, -6);
    QIcon::fromTheme(diskIconName(d), QIcon::fromTheme(QStringLiteral("drive-harddisk")))
        .paint(&p, QRect(body.left(), body.top(), 22, 22));
    if (d.isSystem)
        QIcon::fromTheme(QStringLiteral("object-locked")).paint(&p, QRect(body.right() - 16, body.top() + 3, 16, 16));

    QFont bold = font();
    bold.setBold(true);
    p.setFont(bold);
    p.setPen(pal.color(QPalette::ButtonText));
#ifdef Q_OS_WIN
    // Windows' own number ("Disk 3", as in Disk Management), wherever the map shows it.
    Q_UNUSED(row)
    p.drawText(QRect(body.left() + 28, body.top(), body.width() - 48, 22), Qt::AlignLeft | Qt::AlignVCenter, d.device);
#else
    p.drawText(QRect(body.left() + 28, body.top(), body.width() - 48, 22), Qt::AlignLeft | Qt::AlignVCenter,
               tr("Disk %1").arg(row));
#endif
    p.setFont(font());

    const QFontMetrics fm = fontMetrics();
    QString state = d.isSystem ? tr("System disk") : d.isLoop && d.readOnly ? tr("Read-only image") : tr("Online");
    QColor dot;
    switch (d.health.state) {
    case Health::State::Healthy: dot = Theme::instance().color(Theme::Role::Good); break;
    case Health::State::Warning: dot = Theme::instance().color(Theme::Role::Warning); break;
    case Health::State::Failing: dot = Theme::instance().color(Theme::Role::Danger); break;
    case Health::State::Unknown: break;
    }
    if (dot.isValid())
        state = d.health.summary;
    if (d.isRaid)
        state = raidStatus(d);
    const QStringList lines = {
        tableName(d) + QStringLiteral(" · ") + diskKind(d),
        formatSize(d.size),
        state,
    };
    int y = body.top() + 24;
    for (int i = 0; i < lines.size(); ++i) {
        int x = body.left();
        if (i == 2 && dot.isValid()) {
            p.save();
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(dot);
            p.drawEllipse(QRectF(x, y + fm.height() / 2.0 - 4, 8, 8));
            p.restore();
            x += 12;
        }
        p.drawText(QRect(x, y, body.right() - x, fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(lines[i], Qt::ElideRight, body.right() - x));
        y += fm.height();
    }
}

void DiskMap::paintSegment(QPainter &p, int row, int segment) const
{
    const Disk &d = m_disks[row];
    const Span &span = m_rows[row].segments[segment].span;
    const QPalette pal = palette();
    const bool selected = isSelected(row, segment);
    const QRect r = m_rows[row].segments[segment].rect.adjusted(segment ? 2 : 0, 0, 0, 0);

    p.fillRect(r, pal.color(QPalette::Base));
    const Theme &theme = Theme::instance();
    const bool locked = !span.isFree() && d.volumes[span.volume].encrypted && d.volumes[span.volume].cleartextPath.isEmpty();
    p.fillRect(QRect(r.left(), r.top(), r.width(), kStrip),
               span.isFree() ? theme.color(Theme::Role::Free)
               : locked      ? theme.color(Theme::Role::Encrypted)
                             : theme.partitionColor(d.volumes[span.volume].effectiveFsType()));
    const QRect body = r.adjusted(0, kStrip, 0, 0);
    if (selected) {
        QColor hatch = pal.color(QPalette::Highlight);
        hatch.setAlpha(110);
        p.fillRect(body, QBrush(hatch, Qt::BDiagPattern));
    }
    p.setPen(selected ? pal.color(QPalette::Highlight) : pal.color(QPalette::Mid));
    p.drawRect(r.adjusted(0, 0, -1, -1));
    if (selected)
        p.drawRect(r.adjusted(1, 1, -2, -2));

    QStringList lines;
    if (span.isFree()) {
        lines << formatSize(span.size) << (d.volumes.isEmpty() && d.tableType.isEmpty() ? tr("Not initialized") : tr("Unallocated"));
    } else {
        const Volume &v = d.volumes[span.volume];
        lines << volumeTitle(v)
              << (v.fsType.isEmpty() ? formatSize(v.size) : formatSize(v.size) + QLatin1Char(' ') + v.fsType)
              << volumeStatus(v, true);
    }

    const QRect lock = lockRectAt(row, segment);
    const QRect text = body.adjusted(6, 5, lock.isNull() ? -6 : -6 - lock.width() - 4, -4);
    p.save();
    p.setClipRect(text);
    p.setPen(pal.color(QPalette::Text));
    const QFontMetrics fm = fontMetrics();
    QFont bold = font();
    bold.setBold(true);
    int y = text.top();
    for (int i = 0; i < lines.size(); ++i) {
        p.setFont(i == 0 ? bold : font());
        const QFontMetrics lineFm(p.font());
        p.drawText(QRect(text.left(), y, text.width(), fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   lineFm.elidedText(lines[i], Qt::ElideRight, text.width()));
        y += fm.height();
    }
    p.restore();

    if (!lock.isNull()) {
        // A theme's lock icon when there is one, otherwise a small drawn padlock.
        const QString icon = locked ? QStringLiteral("object-locked") : QStringLiteral("object-unlocked");
        if (QIcon::hasThemeIcon(icon)) {
            QIcon::fromTheme(icon).paint(&p, lock);
        } else {
            p.save();
            p.setRenderHint(QPainter::Antialiasing);
            const QColor ink = pal.color(QPalette::Text);
            const QRectF box(lock.left() + 3, lock.top() + 8, lock.width() - 6, lock.height() - 9);
            p.setPen(QPen(ink, 1.6));
            p.setBrush(Qt::NoBrush);
            const qreal lift = locked ? 0 : 3; // an open shackle sits higher, off the right side
            const QRectF shackle(box.left() + 2.5, lock.top() + 2 - lift, box.width() - 5, 10);
            p.drawArc(shackle, 0, 180 * 16);
            p.drawLine(QPointF(shackle.left(), shackle.center().y()), QPointF(shackle.left(), box.top()));
            if (locked)
                p.drawLine(QPointF(shackle.right(), shackle.center().y()), QPointF(shackle.right(), box.top()));
            p.setPen(Qt::NoPen);
            p.setBrush(ink);
            p.drawRoundedRect(box, 2, 2);
            p.restore();
        }
    }
}
