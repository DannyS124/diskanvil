// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QStringList>

class QFileSystemModel;
class QLabel;
class QLineEdit;
class QListWidget;
class QTreeView;
class QAction;
class UDisks;

// Files: DiskAnvil Live's file window (it has no desktop file manager). The drives on the left,
// opened when picked; a folder's files; copy, move, rename, delete, a new folder; pictures and
// films opened in Look Closer; a terminal there. On a drive that's read-only or write-protected
// it says so and offers nothing that would change it.
class FilesDialog : public QDialog
{
    Q_OBJECT
public:
    explicit FilesDialog(UDisks *udisks, QWidget *parent = nullptr);

    void go(const QString &folder);
    QString folder() const { return m_folder; }
    // The tests: what Paste puts where, and answering the questions it would ask.
    void copyPaths(const QStringList &paths, bool move) { m_clipboard = paths; m_move = move; }
    bool paste(); // waits until it's done; false when something wasn't copied
    // Files dropped on it (from Find Lost Files, another Files window, a browser's downloads...):
    // copied (moved with Shift) into `folder`, or the one open. False when nothing could be.
    bool drop(const QList<QUrl> &urls, bool move, const QString &folder = {});
    void setAnswerYes(bool yes) { m_answerYes = yes; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    bool paste(const QString &into);
    QString dropFolder(const QPoint &at) const; // the folder under the pointer, or the one open, if it can be changed
    void fillPlaces();
    void openedNow();
    QStringList picked() const;
    void updateActions();
    void newFolder();
    void rename();
    void remove();
    void openPicked();
    bool ask(const QString &question);

    UDisks *m_udisks;
    QListWidget *m_places;
    QLineEdit *m_path;
    QTreeView *m_view;
    QFileSystemModel *m_model;
    QLabel *m_note;
    QAction *m_up, *m_newFolder, *m_copy, *m_cut, *m_paste, *m_rename, *m_delete, *m_look, *m_terminal;
    QString m_folder;
    QString m_opening; // the drive asked to open (UDisks object), shown once it is
    QStringList m_clipboard;
    bool m_move = false;
    bool m_answerYes = false;
};
