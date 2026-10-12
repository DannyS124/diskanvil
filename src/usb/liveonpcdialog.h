// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Put DiskAnvil Live on This PC (liveonpc.h): the window. It finds the ISO the way Make a DiskAnvil
// Live USB does, makes the partition in free space, copies DiskAnvil Live there (checking every
// file), and adds it to the PC's boot menu. Update replaces it with the ISO's version, keeping its
// logs; Take It Off removes the entry and the partition.

#include "udisks.h"

#include <QDialog>

#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QThread;
namespace rescue {
class StickWriter;
}

class LiveOnPcDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LiveOnPcDialog(UDisks *udisks, QWidget *parent = nullptr);
    ~LiveOnPcDialog() override;

    // For the tests: loop devices count as the PC's drives, the PC counts as UEFI, and what would
    // run as root (efibootmgr) is handed here instead (this PC's boot menu is never touched).
    static inline bool allowLoopDevicesForTest = false;
    static inline std::function<bool(const QString &script, const QStringList &args, QString *said)> rootForTest;

protected:
    void reject() override;

private:
    void fillTargets();
    void inspectImage();
    void updateState();
    void put();
    void update();
    void takeOff();
    void whenMounted(const QString &volumePath, const std::function<void(const QString &root)> &then);
    void copyInto(const QString &root, bool fresh);
    void addEntry();
    bool runAsRoot(const QString &script, const QStringList &args, QString *said);
    void finish(bool ok, const QString &message, bool alreadyShown = false);
    const Disk *target() const;
    bool uefi() const;

    UDisks *m_udisks;
    QLineEdit *m_image;
    QLabel *m_imageInfo;
    QComboBox *m_targets;
    QLabel *m_plan;
    QPushButton *m_put;
    QPushButton *m_update;
    QPushButton *m_takeOff;
    QProgressBar *m_progress;
    QLabel *m_phase;

    QString m_inspectedPath;
    quint64 m_imageBytes = 0;
    QString m_imageError;
    QString m_diskPath;   // the drive it's going on
    QString m_volumePath; // its partition, once there
    QThread *m_thread = nullptr;
    rescue::StickWriter *m_writer = nullptr;
    QMetaObject::Connection m_waiting, m_waitingFailed;
    bool m_running = false;
};
