// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Write-Protected Drive: pick the stick, card or drive that won't let anything be saved; it says
// why (writeprotect::diagnose), and fixes what a program can fix, then looks again.

#include "writeprotect.h"

#include <QDialog>
#include <functional>
#include <optional>

class QComboBox;
class QLabel;
class QPushButton;
class QVBoxLayout;
class UDisks;

class WriteProtectDialog : public QDialog
{
    Q_OBJECT
public:
    explicit WriteProtectDialog(UDisks *udisks, QWidget *parent = nullptr);

    // The tests: look at this instead of the drive, and run root commands through this.
    void setStateForTest(const writeprotect::State &state) { m_testState = state; }
    std::function<bool(const QStringList &command, QString *said)> runAsRoot;
    const QVector<writeprotect::Finding> &findings() const { return m_findings; }
    void look();
    void pick(const QString &blockPath); // the drive to start with
    void fix(int index);

signals:
    void looked(); // the findings are shown

private:
    void fillDrives();
    void showFindings(const writeprotect::State &state);
    void reopen(const QString &objectPath, bool repairFirst);

    UDisks *m_udisks;
    QComboBox *m_drives;
    QPushButton *m_look;
    QLabel *m_status;
    QVBoxLayout *m_results;
    QWidget *m_resultsBox;
    QVector<writeprotect::Finding> m_findings;
    writeprotect::State m_state;
    std::optional<writeprotect::State> m_testState;
    std::function<void(bool ok, const QString &message)> m_afterOperation;
    QString m_waitingFor; // a repair's volume, then opened again
};
