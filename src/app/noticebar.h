// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A bar across the top of the main window: a drive that's failing, a job that can be
// stopped. Plain text only, colors from the theme.

#include <QWidget>

class QHBoxLayout;
class QLabel;
class QPushButton;

class NoticeBar : public QWidget
{
    Q_OBJECT
public:
    enum class Level { Info, Warning, Danger };
    NoticeBar(Level level, const QString &text, QWidget *parent = nullptr);
    void setText(const QString &text);
    QString text() const;
    QPushButton *addButton(const QString &text);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Level m_level;
    QLabel *m_text;
    QHBoxLayout *m_buttons;
};
