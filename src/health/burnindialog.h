// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Burn In a New Drive (burnin.h): the drive picked and confirmed by its name, then each step in
// turn, with the report at the end, as a PDF too.

#include "burnin.h"
#include "jobui.h"

#include <QDialog>
#include <QPointer>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QThread;
class QTimer;
class QTreeWidget;
class UDisks;
namespace stickcheck {
class Checker;
struct Result;
}

class BurnInDialog : public QDialog
{
    Q_OBJECT
public:
    BurnInDialog(UDisks *udisks, const QString &preferredDisk = {}, QWidget *parent = nullptr);
    ~BurnInDialog() override;

    static bool allowLoopDevicesForTest;
    const burnin::Report &report() const { return m_report; }

signals:
    void ended(); // the burn-in finished or stopped (the tests)

protected:
    void reject() override;

private:
    void fillTargets(const QString &preferred);
    const Disk *target() const;
    void updateState();
    void start();
    void next();
    void runSelftest(const QString &type);
    void pollSelftest();
    void runWrite();
    void writeDone(const stickcheck::Result &r);
    void stepDone(burnin::StepResult::Outcome outcome, const QString &detail);
    void finish();
    void showSteps();

    UDisks *m_udisks;
    QComboBox *m_targets;
    QCheckBox *m_twice;
    QLabel *m_warning;
    QLineEdit *m_confirm;
    QPushButton *m_start;
    QPushButton *m_pdf;
    QTreeWidget *m_steps;
    QProgressBar *m_progress;
    QLabel *m_phase;
    QLabel *m_result;
    PhaseProgress m_meter;
    QTimer *m_poll;

    QString m_diskPath;
    int m_step = -1;
    bool m_running = false;
    bool m_stopping = false;
    bool m_hasSmart = false;
    QVector<SmartAttribute> m_before;
    burnin::Report m_report;
    // A self-test being waited for: seen running yet, and when it was asked for.
    bool m_seenRunning = false;
    qint64 m_askedAt = 0;
    QPointer<stickcheck::Checker> m_checker;
    QThread *m_thread = nullptr;
};
