// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "noticebar.h"

#include "theme.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QStyle>

NoticeBar::NoticeBar(Level level, const QString &text, QWidget *parent)
    : QWidget(parent)
    , m_level(level)
    , m_text(new QLabel(text))
    , m_buttons(new QHBoxLayout)
{
    m_text->setTextFormat(Qt::PlainText); // drive names come from the drive
    m_text->setWordWrap(true);
    m_text->setTextInteractionFlags(Qt::TextSelectableByMouse);

    const char *name = level == Level::Danger ? "dialog-error" : level == Level::Warning ? "dialog-warning" : "dialog-information";
    const QStyle::StandardPixmap fallback = level == Level::Danger ? QStyle::SP_MessageBoxCritical
        : level == Level::Warning                                  ? QStyle::SP_MessageBoxWarning
                                                                   : QStyle::SP_MessageBoxInformation;
    const int size = style()->pixelMetric(QStyle::PM_SmallIconSize);
    auto *icon = new QLabel;
    icon->setPixmap(QIcon::fromTheme(QLatin1String(name), style()->standardIcon(fallback)).pixmap(size, size));

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 4, 4, 4);
    layout->addWidget(icon);
    layout->addWidget(m_text, 1);
    m_buttons->setSpacing(4);
    layout->addLayout(m_buttons);
    connect(&Theme::instance(), &Theme::changed, this, qOverload<>(&QWidget::update));
}

void NoticeBar::setText(const QString &text)
{
    m_text->setText(text);
}

QString NoticeBar::text() const
{
    return m_text->text();
}

QPushButton *NoticeBar::addButton(const QString &text)
{
    auto *button = new QPushButton(text);
    m_buttons->addWidget(button);
    return button;
}

void NoticeBar::paintEvent(QPaintEvent *)
{
    const Theme &theme = Theme::instance();
    const QColor accent = m_level == Level::Danger ? theme.color(Theme::Role::Danger)
        : m_level == Level::Warning                ? theme.color(Theme::Role::Warning)
                                                   : palette().color(QPalette::Highlight);
    // A light tint of the accent over the window color, so the text stays readable.
    const QColor window = palette().color(QPalette::Window);
    const qreal tint = 0.16;
    const QColor fill = QColor::fromRgbF(window.redF() * (1 - tint) + accent.redF() * tint, window.greenF() * (1 - tint) + accent.greenF() * tint,
                                         window.blueF() * (1 - tint) + accent.blueF() * tint);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(accent, 1));
    p.setBrush(fill);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
}
