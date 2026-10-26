// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "historygraph.h"

#include "format.h"
#include "healthwatch.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>

HistoryGraph::HistoryGraph(const healthhistory::Series &series, QWidget *parent)
    : QWidget(parent)
    , m_series(series)
{
    setMinimumSize(200, 90);
    setMaximumHeight(150);
    QStringList tip;
    for (const auto &[day, v] : std::as_const(m_series.values))
        tip << QLocale().toString(day, QLocale::ShortFormat) + QStringLiteral(": ") + valueText(v);
    // the newest ones, in the tooltip
    setToolTip(m_series.name + QLatin1Char('\n') + tip.mid(std::max<qsizetype>(0, tip.size() - 14)).join(QLatin1Char('\n')));
}

QString HistoryGraph::valueText(double v) const
{
    if (m_series.unit == QLatin1String("bytes"))
        return formatSize(quint64(v));
    if (m_series.unit == QStringLiteral("°C"))
        return QStringLiteral("%1 °C").arg(qRound(v));
    if (m_series.unit == QLatin1String("%"))
        return QStringLiteral("%1%").arg(qRound(v));
    return QLocale().toString(qint64(v));
}

void HistoryGraph::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    const QFontMetrics fm = fontMetrics();
    const auto &values = m_series.values;
    const double last = values.isEmpty() ? 0 : values.last().second;
    // What it shows: up where up is bad, in the warning colour.
    const bool rose = m_series.badWhenUp && m_series.unit != QStringLiteral("°C") && values.size() > 1 && last > values.first().second;
    const QColor line = rose ? QColor(0xd9, 0x3f, 0x3f) : pal.color(QPalette::Highlight);

    QFont bold = font();
    bold.setBold(true);
    p.setFont(bold);
    p.setPen(pal.color(QPalette::WindowText));
    p.drawText(QRect(0, 0, width(), fm.height()), Qt::AlignLeft | Qt::AlignVCenter, m_series.name);
    p.drawText(QRect(0, 0, width(), fm.height()), Qt::AlignRight | Qt::AlignVCenter, valueText(last));
    p.setFont(font());

    const QRectF area(4, fm.height() + 4, width() - 8, height() - 2 * fm.height() - 10);
    p.setPen(QPen(pal.color(QPalette::Mid), 1));
    p.drawLine(area.bottomLeft(), area.bottomRight());
    if (values.isEmpty())
        return;
    const qint64 first = values.first().first.toJulianDay(), span = std::max<qint64>(1, values.last().first.toJulianDay() - first);
    double lo = values.first().second, hi = lo;
    for (const auto &[day, v] : values) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    // Counts start at 0, so a counter that stays at 0 is a flat line at the bottom.
    if (m_series.unit != QStringLiteral("°C"))
        lo = std::min(lo, 0.0);
    if (hi - lo < 1e-9)
        hi = lo + 1;
    auto at = [&](const QPair<QDate, double> &v) {
        const double x = values.size() == 1 ? area.center().x() : area.left() + area.width() * double(v.first.toJulianDay() - first) / double(span);
        return QPointF(x, area.bottom() - area.height() * (v.second - lo) / (hi - lo));
    };
    QPainterPath path;
    path.moveTo(at(values.first()));
    for (qsizetype i = 1; i < values.size(); ++i)
        path.lineTo(at(values[i]));
    p.setPen(QPen(line, 2));
    p.drawPath(path);
    p.setBrush(line);
    p.drawEllipse(at(values.last()), 2.5, 2.5);

    p.setPen(pal.color(QPalette::PlaceholderText));
    const QRect bottom(0, height() - fm.height(), width(), fm.height());
    p.drawText(bottom, Qt::AlignLeft | Qt::AlignVCenter, QLocale().toString(values.first().first, QLocale::ShortFormat));
    if (values.size() > 1)
        p.drawText(bottom, Qt::AlignRight | Qt::AlignVCenter, QLocale().toString(values.last().first, QLocale::ShortFormat));
}

HealthHistoryDialog::HealthHistoryDialog(const Disk &disk, QWidget *parent)
    : QDialog(parent)
    , m_daily(new QCheckBox(tr("Check my drives every day, and tell me when one gets worse")))
    , m_dailyNote(new QLabel)
{
    setWindowTitle(tr("Health History: %1").arg(disk.model.isEmpty() ? disk.device : disk.model));
    const healthhistory::History history = healthhistory::load(disk.health.key);
    auto *intro = new QLabel;
    intro->setWordWrap(true);
    if (history.points.isEmpty()) {
        intro->setText(tr("Nothing kept yet. DiskAnvil notes this drive's health once a day whenever it runs (or with the daily "
                          "check below), and the graphs fill in from there."));
    } else {
        intro->setText(tr("What DiskAnvil saw of this drive on %n day(s), from %1 to %2. Hover over a graph for the numbers.", nullptr,
                          int(history.points.size()))
                           .arg(QLocale().toString(history.points.first().day, QLocale::ShortFormat),
                                QLocale().toString(history.points.last().day, QLocale::ShortFormat)));
    }
    auto *grid = new QGridLayout;
    for (const healthhistory::Series &s : healthhistory::series(history)) {
        auto *g = new HistoryGraph(s);
        grid->addWidget(g, m_graphs / 2, m_graphs % 2);
        ++m_graphs;
    }
    grid->setRowStretch(grid->rowCount(), 1);
    auto *graphs = new QWidget;
    graphs->setLayout(grid);
    auto *scroll = new QScrollArea;
    scroll->setWidget(graphs);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    m_daily->setObjectName(QStringLiteral("daily"));
    m_dailyNote->setWordWrap(true);
    const QString why = healthwatch::unavailableReason();
    {
        const QSignalBlocker block(m_daily);
        m_daily->setChecked(why.isEmpty() && healthwatch::enabled());
    }
    m_daily->setEnabled(why.isEmpty());
    m_dailyNote->setText(!why.isEmpty() ? why
                                        : tr("It runs by itself once a day, even when DiskAnvil isn't open, only reads, and shows a "
                                             "notification when a drive got worse (new bad sectors, cable errors, wear)."));
    connect(m_daily, &QCheckBox::toggled, this, [this](bool on) {
        QString message;
        const bool ok = healthwatch::setEnabled(on, &message);
        if (!ok) {
            const QSignalBlocker block(m_daily);
            m_daily->setChecked(!on);
        }
        m_dailyNote->setText(message);
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(scroll, 1);
    layout->addWidget(m_daily);
    layout->addWidget(m_dailyNote);
    layout->addWidget(buttons);
    resize(620, 560);
}
