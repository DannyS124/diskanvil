// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hexview.h"

#include <QCoreApplication>
#include <QFile>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>

namespace {

constexpr int kPerLine = 16;
constexpr qint64 kMostInMemory = 256 * 1024 * 1024; // when the file can't be mapped

} // namespace

HexView::HexView(QWidget *parent)
    : QAbstractScrollArea(parent)
{
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setCursor(Qt::IBeamCursor);
}

HexView::~HexView()
{
    blockSignals(true); // what it's in is going away too
    close();
}

bool HexView::open(const QString &path, QString *why)
{
    close();
    m_file = new QFile(path);
    if (!m_file->open(QIODevice::ReadOnly)) {
        if (why)
            *why = m_file->errorString();
        close();
        return false;
    }
    m_size = quint64(m_file->size());
    m_data = m_size ? m_file->map(0, qint64(m_size)) : nullptr;
    if (!m_data && m_size) {
        m_memory = m_file->read(kMostInMemory);
        m_size = quint64(m_memory.size());
        m_data = reinterpret_cast<const uchar *>(m_memory.constData());
    }
    layoutScroll();
    viewport()->update();
    return true;
}

void HexView::close()
{
    if (m_file) {
        if (m_data && m_memory.isEmpty())
            m_file->unmap(const_cast<uchar *>(m_data));
        delete m_file;
        m_file = nullptr;
    }
    m_memory.clear();
    m_data = nullptr;
    m_size = 0;
    m_markAt = m_markSize = 0;
    m_cursor = 0;
    m_edits.clear();
    m_undo.clear();
    verticalScrollBar()->setValue(0);
    Q_EMIT editsChanged(0);
}

quint8 HexView::byteAt(quint64 at) const
{
    const auto edit = m_edits.constFind(at);
    if (edit != m_edits.cend())
        return *edit;
    return at < m_size ? m_data[at] : 0;
}

int HexView::lineHeight() const
{
    return fontMetrics().height() + 2;
}

int HexView::hexX(int column) const
{
    const int ch = fontMetrics().horizontalAdvance(QLatin1Char('0'));
    return 8 + 12 * ch + column * 3 * ch + (column >= 8 ? ch : 0);
}

int HexView::textX(int column) const
{
    const int ch = fontMetrics().horizontalAdvance(QLatin1Char('0'));
    return hexX(kPerLine) + 2 * ch + column * ch;
}

void HexView::layoutScroll()
{
    const quint64 lines = (m_size + kPerLine - 1) / kPerLine;
    const int visible = std::max(1, viewport()->height() / lineHeight());
    verticalScrollBar()->setRange(0, int(std::min<quint64>(lines > quint64(visible) ? lines - quint64(visible) + 1 : 0, INT_MAX)));
    verticalScrollBar()->setPageStep(visible);
    verticalScrollBar()->setSingleStep(1);
    horizontalScrollBar()->setRange(0, std::max(0, textX(kPerLine) + 8 - viewport()->width()));
    horizontalScrollBar()->setPageStep(viewport()->width());
}

void HexView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    layoutScroll();
}

void HexView::reveal(quint64 at)
{
    const int line = int(std::min<quint64>(at / kPerLine, INT_MAX));
    const int top = verticalScrollBar()->value(), visible = verticalScrollBar()->pageStep();
    if (line < top || line >= top + visible)
        verticalScrollBar()->setValue(std::max(0, line - visible / 3));
}

void HexView::mark(quint64 at, quint64 size)
{
    m_markAt = at;
    m_markSize = size;
    reveal(at);
    viewport()->update();
}

void HexView::setEditing(bool on)
{
    m_editing = on;
    m_lowNibble = false;
    if (on)
        setFocus();
    viewport()->update();
}

void HexView::moveTo(quint64 at)
{
    m_cursor = m_size ? std::min(at, m_size - 1) : 0;
    m_lowNibble = false;
    reveal(m_cursor);
    viewport()->update();
}

void HexView::undo()
{
    if (m_undo.isEmpty())
        return;
    const Undo u = m_undo.takeLast();
    if (u.had)
        m_edits.insert(u.at, u.was);
    else
        m_edits.remove(u.at);
    moveTo(u.at);
    Q_EMIT editsChanged(int(m_edits.size()));
}

void HexView::write(quint8 value)
{
    if (m_cursor >= m_size)
        return;
    const auto old = m_edits.constFind(m_cursor);
    m_undo.push_back({m_cursor, old != m_edits.cend(), old != m_edits.cend() ? *old : quint8(0)});
    if (value == m_data[m_cursor])
        m_edits.remove(m_cursor); // back to what the file has
    else
        m_edits.insert(m_cursor, value);
    Q_EMIT editsChanged(int(m_edits.size()));
}

void HexView::keyPressEvent(QKeyEvent *event)
{
    if (!m_size) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    const auto move = [this](qint64 by) {
        const qint64 to = std::clamp<qint64>(qint64(m_cursor) + by, 0, qint64(m_size) - 1);
        moveTo(quint64(to));
    };
    if (event->matches(QKeySequence::Undo)) {
        undo();
        return;
    }
    switch (event->key()) {
    case Qt::Key_Left:
        move(-1);
        return;
    case Qt::Key_Right:
        move(1);
        return;
    case Qt::Key_Up:
        move(-kPerLine);
        return;
    case Qt::Key_Down:
        move(kPerLine);
        return;
    case Qt::Key_PageUp:
        move(-qint64(kPerLine) * verticalScrollBar()->pageStep());
        return;
    case Qt::Key_PageDown:
        move(qint64(kPerLine) * verticalScrollBar()->pageStep());
        return;
    case Qt::Key_Home:
        event->modifiers() & Qt::ControlModifier ? moveTo(0) : moveTo(m_cursor - m_cursor % kPerLine);
        return;
    case Qt::Key_End:
        event->modifiers() & Qt::ControlModifier ? moveTo(m_size - 1) : moveTo(m_cursor - m_cursor % kPerLine + kPerLine - 1);
        return;
    case Qt::Key_Tab:
        m_inText = !m_inText;
        m_lowNibble = false;
        viewport()->update();
        return;
    default:
        break;
    }
    if (!m_editing || event->text().isEmpty()) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    const QChar c = event->text().at(0);
    if (m_inText) {
        if (c.unicode() >= 0x20 && c.unicode() < 0x7F) {
            write(quint8(c.unicode()));
            move(1);
        }
        return;
    }
    const int nibble = c.isDigit() ? c.digitValue() : (c.toLower() >= QLatin1Char('a') && c.toLower() <= QLatin1Char('f')) ? c.toLower().unicode() - 'a' + 10 : -1;
    if (nibble < 0)
        return;
    const quint8 now = byteAt(m_cursor);
    write(m_lowNibble ? quint8((now & 0xF0) | nibble) : quint8((nibble << 4) | (now & 0x0F)));
    if (m_lowNibble) {
        m_lowNibble = false;
        move(1);
    } else {
        m_lowNibble = true;
        viewport()->update();
    }
}

void HexView::mousePressEvent(QMouseEvent *event)
{
    const int x = event->position().toPoint().x() + horizontalScrollBar()->value();
    const quint64 line = quint64(verticalScrollBar()->value()) + quint64(event->position().toPoint().y() / lineHeight());
    int column = -1;
    for (int c = 0; c < kPerLine && column < 0; ++c) {
        if (x >= hexX(c) && x < hexX(c) + fontMetrics().horizontalAdvance(QStringLiteral("00 "))) {
            column = c;
            m_inText = false;
        } else if (x >= textX(c) && x < textX(c + 1)) {
            column = c;
            m_inText = true;
        }
    }
    if (column >= 0)
        moveTo(line * kPerLine + quint64(column));
    setFocus();
}

void HexView::paintEvent(QPaintEvent *)
{
    QPainter p(viewport());
    p.translate(-horizontalScrollBar()->value(), 0);
    const QPalette &pal = palette();
    p.fillRect(viewport()->rect().translated(horizontalScrollBar()->value(), 0), pal.base());
    const int height = lineHeight(), ascent = fontMetrics().ascent() + 1;
    const int ch = fontMetrics().horizontalAdvance(QLatin1Char('0'));
    const QColor quiet = pal.color(QPalette::PlaceholderText);
    QColor marked = pal.color(QPalette::Highlight);
    marked.setAlpha(70);
    const QColor changed(0xff, 0x33, 0x55);
    const quint64 first = quint64(verticalScrollBar()->value());
    const int lines = viewport()->height() / height + 1;
    for (int row = 0; row < lines; ++row) {
        const quint64 line = first + quint64(row);
        const quint64 start = line * kPerLine;
        if (start >= m_size)
            break;
        const int y = row * height;
        p.setPen(quiet);
        p.drawText(8, y + ascent, QStringLiteral("%1").arg(start, 10, 16, QLatin1Char('0')));
        for (int c = 0; c < kPerLine && start + quint64(c) < m_size; ++c) {
            const quint64 at = start + quint64(c);
            const quint8 v = byteAt(at);
            const bool inMark = m_markSize > 0 && at >= m_markAt && at - m_markAt < m_markSize;
            if (inMark) {
                p.fillRect(QRect(hexX(c) - ch / 2, y, 3 * ch, height), marked);
                p.fillRect(QRect(textX(c), y, ch, height), marked);
            }
            if (m_editing && at == m_cursor) {
                QColor cursor = pal.color(QPalette::Highlight);
                p.fillRect(QRect(m_inText ? textX(c) : hexX(c) + (m_lowNibble ? ch : 0), y, ch, height), cursor);
                cursor.setAlpha(60);
                p.fillRect(QRect(m_inText ? hexX(c) : textX(c), y, m_inText ? 2 * ch : ch, height), cursor);
            }
            const bool edited = m_edits.contains(at);
            p.setPen(edited ? changed : v == 0 ? quiet : pal.color(QPalette::Text));
            p.drawText(hexX(c), y + ascent, QStringLiteral("%1").arg(v, 2, 16, QLatin1Char('0')));
            p.drawText(textX(c), y + ascent, v >= 0x20 && v < 0x7F ? QString(QChar(v)) : QStringLiteral("·"));
        }
    }
}
