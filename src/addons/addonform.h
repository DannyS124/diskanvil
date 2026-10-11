// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The form an add-on asks for before an action runs, or its settings. DiskForge draws it
// from the field list in the add-on file; labels and values are shown as plain text.

#include "addons.h"

#include <QDialog>
#include <QMap>
#include <QWidget>

#include <functional>

class AddonFormWidget : public QWidget
{
    Q_OBJECT
public:
    AddonFormWidget(const QVector<AddonField> &fields, const QMap<QString, QString> &values, QWidget *parent = nullptr);
    QMap<QString, QString> values() const; // by field id

private:
    QVector<QPair<QString, std::function<QString()>>> m_readers;
};

class AddonFormDialog : public QDialog
{
    Q_OBJECT
public:
    AddonFormDialog(const QString &title, const QString &intro, const QVector<AddonField> &fields,
                    const QMap<QString, QString> &values, const QString &okText, QWidget *parent = nullptr);
    QMap<QString, QString> values() const { return m_form->values(); }

private:
    AddonFormWidget *m_form;
};
