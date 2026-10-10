// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QHash>
#include <QStringList>
#include <QVector>

#include <functional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTreeWidget;
class UDisks;
struct Volume;

// Give New IDs: a copy of a drive gets its own disk ID, partition IDs and file system IDs,
// so it can be used next to the drive it was copied from. A Linux on it has its settings
// (/etc/fstab, boot entries) changed to the new IDs too, so it still starts.
class NewIdsDialog : public QDialog
{
    Q_OBJECT
public:
    NewIdsDialog(UDisks *udisks, const QString &diskPath, QWidget *parent = nullptr);

    // Starts once the device name is typed (the tests' way in).
    void start();

private:
    void updateState();
    void nextStep();
    void waitForNewTable();
    void fixLinuxSettings();
    void fixNext();
    void fixIn(const QString &objectPath, const QString &mount, bool mountedHere);
    void finish(bool ok, const QString &message);

    UDisks *m_udisks;
    QString m_disk;
    QString m_device;
    QTreeWidget *m_list;
    QCheckBox *m_linux;
    QLineEdit *m_confirm;
    QLabel *m_phase;
    QProgressBar *m_progress;
    QPushButton *m_start;

    QString m_oldTableId;
    QHash<int, QString> m_oldPartIds; // partition number, its ID before
    QHash<int, QString> m_oldUuids;   // partition number, its file system's ID before
    QHash<int, QString> m_newUuids;
    QList<std::function<bool()>> m_steps; // true: a UDisks operation was started; wait for it
    QMetaObject::Connection m_opConn;
    QStringList m_notes;
    QStringList m_fixTodo;           // file systems to look at for a Linux's settings
    QHash<QString, QString> m_renamed; // old ID, new ID
    QStringList m_fixed;             // settings files changed
    bool m_running = false;
};
