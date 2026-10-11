// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Make a theme add-on by picking colors, with a live preview. It's saved like any other
// add-on, and the same checks apply (a "danger" that isn't red gets replaced, and so on).

#include "addons.h"

#include <QColor>
#include <QDialog>
#include <QMap>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

// A small sample of DiskForge drawn with a theme's colors.
class ThemePreview : public QWidget
{
    Q_OBJECT
public:
    explicit ThemePreview(QWidget *parent = nullptr);
    void setTheme(const AddonTheme &theme);
    QSize sizeHint() const override { return QSize(420, 190); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    AddonTheme m_theme;
};

class ThemeMaker : public QDialog
{
    Q_OBJECT
public:
    explicit ThemeMaker(Addons *addons, QWidget *parent = nullptr);
    AddonTheme theme() const; // as picked, before the checks

private:
    void addColor(const QString &role, const QString &label, const QColor &start, bool window);
    void refresh();
    void save();

    Addons *m_addons;
    QLineEdit *m_name;
    QLineEdit *m_author;
    QCheckBox *m_ownWindow;
    QMap<QString, QColor> m_window; // palette roles
    QMap<QString, QColor> m_colors; // DiskForge's own roles
    QMap<QString, QPushButton *> m_buttons;
    QWidget *m_windowBox;
    ThemePreview *m_preview;
    QLabel *m_notes;
};
