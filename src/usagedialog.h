// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What's taking up the space on a mounted drive: a treemap (one box per folder or file,
// sized by how much it uses) next to a list, clicking through folders.

#include "fswalk.h"

#include <QDialog>

#include <memory>

class QLabel;
class QProgressBar;
class QPushButton;
class QThread;
class QTreeWidget;

class TreemapWidget : public QWidget
{
    Q_OBJECT
public:
    explicit TreemapWidget(QWidget *parent = nullptr);
    void setNode(const UsageNode *node);
    QSize sizeHint() const override { return {560, 420}; }

signals:
    void clicked(int child);
    void menuRequested(int child, const QPoint &globalPos);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    bool event(QEvent *event) override;

private:
    void layoutBoxes();
    int childAt(const QPointF &pos) const;
    QColor colour(int child) const;

    const UsageNode *m_node = nullptr;
    QVector<QRectF> m_boxes;
    int m_hover = -1;
};

class UsageDialog : public QDialog
{
    Q_OBJECT
public:
    UsageDialog(const QString &mountPoint, const QString &title, QWidget *parent = nullptr);
    ~UsageDialog() override;

private:
    void scan();
    void showLevel(int depth);
    void enter(int child);
    void openChild(int child, const QPoint &globalPos);
    QString pathOf(int depth, int child = -1) const;

    QString m_root;
    std::shared_ptr<UsageNode> m_tree;
    QVector<const UsageNode *> m_stack; // the folders clicked into, from the top
    int m_unreadable = 0;
    bool m_completed = false;
    QLabel *m_crumbs;
    QPushButton *m_up;
    TreemapWidget *m_map;
    QTreeWidget *m_list;
    QLabel *m_status;
    QProgressBar *m_busy;
    QThread *m_thread = nullptr;
    UsageScan *m_scan = nullptr;
};
