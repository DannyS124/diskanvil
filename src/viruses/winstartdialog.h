// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Windows Won't Start: a stuck update undone, and a damaged registry file put back from an
// earlier day (winstart.h), for the Windows picked (mounted so it can be changed).

#include "winstart.h"

#include <QDialog>

#include <memory>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
class UDisks;

class WinStartDialog : public QDialog
{
    Q_OBJECT
public:
    // `root`: where that Windows is mounted. `udisks` finds its partition, for the shadow copies
    // (null: none are looked for).
    WinStartDialog(UDisks *udisks, const QString &root, QWidget *parent = nullptr);
    ~WinStartDialog() override;

signals:
    void looked(); // everything's been read (the tests)

private:
    void look();
    void showUpdate();
    void showHives();
    void showCopies();
    void undoUpdate();
    void putBack();
    void readShadowCopies();
    void say(const QStringList &lines);

    UDisks *m_udisks;
    QString m_root;
    QLabel *m_update;
    QPushButton *m_undo;
    QTreeWidget *m_hives;
    QListWidget *m_copies;
    QLabel *m_copiesNote;
    QPushButton *m_putBack;
    QPlainTextEdit *m_log;
    winstart::StuckUpdate m_stuck;
    QVector<winstart::HiveCheck> m_checks;
    QVector<winstart::EarlierCopy> m_earlier;
    QString m_shadowProblem;
    bool m_shadowRead = false;
    std::shared_ptr<std::atomic<bool>> m_cancel = std::make_shared<std::atomic<bool>>(false);
};
