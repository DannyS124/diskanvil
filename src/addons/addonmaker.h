// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Make or edit an add-on in a window instead of writing JSON. What it saves is checked with
// the same parser everything else uses, so it can't save something DiskForge would refuse.

#include "addons.h"

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;

// A table of form fields ("ask") or settings: id, type, label, default and options.
class FieldTable : public QWidget
{
    Q_OBJECT
public:
    explicit FieldTable(QWidget *parent = nullptr);
    void setFields(const QJsonArray &fields);
    QJsonArray fields() const;
    QStringList ids() const;

signals:
    void changed();

private:
    void addRow(const QJsonObject &field);
    QTableWidget *m_table;
};

class AddonMaker : public QDialog
{
    Q_OBJECT
public:
    // Runs an action on the main window's selection, asking first and remembering nothing.
    using Tester = std::function<void(const Addon &, const AddonAction &)>;
    // existing: the add-on to edit (a copy is saved for one installed by a package), or null.
    AddonMaker(Addons *addons, const Addon *existing, Tester tester, QWidget *parent = nullptr);
    QByteArray json() const; // the add-on as it stands

private:
    QJsonObject manifest() const;
    void loadManifest(const QJsonObject &o);
    void showAction(int row);
    void storeAction();
    void refresh();
    void startFrom(int index);
    void insertPlaceholder(const QString &text);
    void save();
    void saveCopy();

    Addons *m_addons;
    Tester m_tester;
    QString m_originalId;
    bool m_keepOutsideFlag = false;
    bool m_loading = false;

    QComboBox *m_template;
    QLineEdit *m_name;
    QLineEdit *m_author;
    QLineEdit *m_version;
    QLineEdit *m_id;
    bool m_idEdited = false;
    QLineEdit *m_description;
    FieldTable *m_settings;

    QListWidget *m_actionList;
    QVector<QJsonObject> m_actions;
    int m_current = -1;
    QLineEdit *m_label;
    QComboBox *m_icon;
    QTableWidget *m_command;
    QComboBox *m_appliesTo;
    QMap<QString, QCheckBox *> m_when;
    QLineEdit *m_filesystems;
    QCheckBox *m_systemDisks;
    QComboBox *m_output;
    QCheckBox *m_lookOnly;
    QLineEdit *m_confirm;
    FieldTable *m_ask;

    QLabel *m_check;
    QPushButton *m_saveButton;
    QPushButton *m_testButton;
};
