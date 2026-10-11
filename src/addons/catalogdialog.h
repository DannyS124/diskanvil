// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "addons.h"

#include <QDialog>

#include <functional>

class QLabel;
class QNetworkAccessManager;
class QPushButton;
class QTextBrowser;
class QTreeWidget;

// The online add-on list. Goes online only while this is open, fetches only from the
// catalog repository, and installs an add-on only if it matches the list's checksum.
class CatalogDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CatalogDialog(Addons *addons, QWidget *parent = nullptr);

private:
    void load();
    void fill();
    void showDetails();
    void install();
    void fetch(const QString &url, const std::function<void(const QByteArray &data, const QString &error)> &done);
    const CatalogEntry *current() const;

    Addons *m_addons;
    QNetworkAccessManager *m_net;
    QTreeWidget *m_list;
    QTextBrowser *m_details;
    QLabel *m_status;
    QPushButton *m_install;
    QVector<CatalogEntry> m_entries;
};
