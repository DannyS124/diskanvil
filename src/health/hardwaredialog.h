// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Hardware Check (hardware.h): what's in the PC and its battery's wear, the processor worked hard
// with its temperature drawn, every key pressed lit up, the screen in solid colours to find dead
// pixels, and the camera's picture. Then a report, as a PDF too.

#include "hardware.h"

#include <QDialog>
#include <QPointer>
#include <QSet>

class QComboBox;
class QLabel;
class QPushButton;
class QTabWidget;
class QThread;
class QWidget;

namespace hardware {
class Chart;
class Keys;
}

class HardwareDialog : public QDialog
{
    Q_OBJECT
public:
    explicit HardwareDialog(QWidget *parent = nullptr);
    ~HardwareDialog() override;

    // The tests.
    void startStress(int seconds);
    const hardware::StressResult &stress() const { return m_stress; }
    int keysPressed() const;
    QString reportHtml() const;

signals:
    void stressDone();

private:
    QWidget *summaryTab();
    QWidget *processorTab();
    QWidget *keyboardTab();
    QWidget *screenTab();
    QWidget *cameraTab();
    void stressFinished(const hardware::StressResult &r);

    QTabWidget *m_tabs;
    hardware::Summary m_summary;
    QComboBox *m_minutes = nullptr;
    QPushButton *m_go = nullptr;
    QLabel *m_stressText = nullptr;
    hardware::Chart *m_chart = nullptr;
    hardware::Keys *m_keys = nullptr;
    QPointer<hardware::Stress> m_job;
    QThread *m_thread = nullptr;
    hardware::StressResult m_stress;
    bool m_screenShown = false;
    QString m_camera; // what the camera check found
};
