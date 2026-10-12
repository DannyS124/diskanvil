// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Find Lost Files' window for btrfs (btrfsrestore.h): the copies of the file tree btrfs still
// has, newest first; what one of them holds, with what isn't there any more marked; and copying
// what's ticked, or all of it, to a folder on another drive.

#include "btrfsrestore.h"

#include <QDialog>
#include <QTemporaryDir>

class QCheckBox;
class QComboBox;
class QLabel;
class QProcess;
class QPushButton;
class QTreeWidget;

class BtrfsDialog : public QDialog
{
    Q_OBJECT
public:
    // `device`: the file system's file, or a drive with it `offset` bytes in (`size` long), read
    // through a read-only loop device made with sudo (DiskAnvil Live: `privileged`).
    // `diskDevice`: the drive it's on ("/dev/sdb"), so nothing is copied onto it.
    BtrfsDialog(const QString &device, const QString &diskDevice, const QString &title, QWidget *parent = nullptr, bool privileged = false,
                quint64 offset = 0, quint64 size = 0);
    ~BtrfsDialog() override;

    // For the tests: where copies go, instead of asking.
    void setTargetForTests(const QString &folder) { m_testTarget = folder; }
    bool busy() const { return m_process != nullptr; }

private:
    // Runs a btrfs-progs program; `then` gets what it said.
    void run(const QStringList &command, const std::function<void(int exitCode, const QString &output)> &then);
    void findTrees();
    void listTree();
    void showList();
    void copy(bool tickedOnly);

    QString m_device, m_disk, m_testTarget;
    bool m_privileged = false;
    QString m_loop; // the loop device made for it, taken away when done
    QComboBox *m_trees;
    QPushButton *m_list;
    QCheckBox *m_onlyGone;
    QTreeWidget *m_files;
    QPushButton *m_copyTicked, *m_copyAll;
    QLabel *m_status;
    QProcess *m_process = nullptr;
    QVector<btrfsrestore::Tree> m_found;
    QStringList m_now, m_listed, m_folders; // what the tree in use has; what the chosen one has; which of those are folders
    QTemporaryDir m_nowhere; // where a dry run "restores" to
};
