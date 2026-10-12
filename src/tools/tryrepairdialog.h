// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Try a Repair First (tryrepair.h): the repair on a stand-in, its result to look at, then the
// real repair or nothing.

#include <QDialog>

#include <functional>

class QLabel;
class QPlainTextEdit;
class QProcess;
class QPushButton;

class TryRepairDialog : public QDialog
{
    Q_OBJECT
public:
    // `device`: the partition (unmounted); `fsType` its file system.
    TryRepairDialog(const QString &device, const QString &fsType, const QString &title, QWidget *parent = nullptr);
    ~TryRepairDialog() override;
    // Whether DiskAnvil can do it here (DiskAnvil Live, or run as root) for this file system.
    static bool possible(const QString &fsType);
    // Accepted: repair it for real.

protected:
    void reject() override;

private:
    void run(const QString &script, const std::function<void(const QString &output)> &then);
    void stop();

    QString m_device, m_fsType;
    QLabel *m_status;
    QPlainTextEdit *m_output;
    QPushButton *m_try, *m_look, *m_real, *m_away;
    QProcess *m_process = nullptr;
    bool m_started = false; // the stand-in is there, to take away at the end
};
