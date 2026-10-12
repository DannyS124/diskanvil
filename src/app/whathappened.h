// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// "What Happened?": for someone who's lost files and doesn't know where to start. What happened,
// which drive, then honest odds, what not to do, and the tools to use in order (triage.h).

#include "triage.h"

#include <QDialog>

#include <optional>

class QLabel;
class QListWidget;
class QStackedWidget;
class QVBoxLayout;

class WhatHappenedDialog : public QDialog
{
    Q_OBJECT
public:
    WhatHappenedDialog(QVector<Disk> disks, bool runningFromStick, QWidget *parent = nullptr);

    // For the tests: the drive's TRIM, instead of finding out.
    void setTrimForTests(triage::Trim trim) { m_testTrim = trim; }

Q_SIGNALS:
    // A tool to open ("lost-files", "rescue-copy", "health", "recover-partitions", "rescue-usb",
    // "virus-scan"), on that drive (its UDisks block path; empty when none was picked).
    void openTool(const QString &tool, const QString &diskBlockPath);

private:
    void pickSymptom(triage::Symptom symptom);
    void showAdvice();

    QVector<Disk> m_disks;
    bool m_fromStick;
    QStackedWidget *m_pages;
    QListWidget *m_drives;
    QLabel *m_odds;
    QLabel *m_dont;
    QLabel *m_steps;
    QVBoxLayout *m_toolButtons;
    triage::Symptom m_symptom = triage::Symptom::Deleted;
    std::optional<triage::Trim> m_testTrim;
};
