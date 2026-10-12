// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Restore from Clonezilla (partclone.h): one of a Clonezilla folder's partition images onto the
// partition picked in the window.

#include "partclone.h"
#include "udisks.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QThread;

class ClonezillaDialog : public QDialog
{
    Q_OBJECT
public:
    ClonezillaDialog(UDisks *udisks, const Volume &target, QWidget *parent = nullptr);
    ~ClonezillaDialog() override;
    void openFolder(const QString &folder);

protected:
    void reject() override;

private:
    void updateState();
    void start();

    UDisks *m_udisks;
    Volume m_target;
    QVector<partclone::Image> m_images;
    QString m_opened; // the folder they're from
    QLineEdit *m_folder;
    QListWidget *m_list;
    QLabel *m_note;
    QLineEdit *m_confirm;
    QPushButton *m_restore;
    QProgressBar *m_bar;
    QThread *m_thread = nullptr;
    partclone::Restore *m_job = nullptr;
};
