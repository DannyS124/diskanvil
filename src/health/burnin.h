// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Burn In a New Drive: what people who keep a lot of data do with every new (or "shucked", or
// second-hand) drive before trusting it, in one go. The drive's own short self-test, then every
// byte written and read back (Check a USB Stick's full check: it also catches a drive smaller than
// it says), then its long self-test, and its health counters compared from before to after. A
// weak drive usually shows itself in the first hours of hard use: better now than with your files
// on it. It erases the drive.

#include "udisks.h"

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

namespace burnin {

// A counter that went up, and how much that matters.
struct Change {
    QString name;  // in plain words: "Reallocated sectors"
    qint64 before = 0;
    qint64 after = 0;
    enum class Weight { Bad, Warning, Note } weight = Weight::Note;
    QString meaning;
};

// The counters that matter, compared. ATA ones by number, NVMe ones by key.
QVector<Change> compare(const QVector<SmartAttribute> &before, const QVector<SmartAttribute> &after);

enum class Step { Before, ShortTest, Write, LongTest, After };
inline constexpr int kSteps = 5;
QString stepName(Step step);

struct StepResult {
    Step step = Step::Before;
    enum class Outcome { NotRun, Passed, Failed, Skipped, Stopped } outcome = Outcome::NotRun;
    QString detail;   // what happened, in plain words
    QDateTime started, finished;
};

struct Report {
    QString drive;   // "WDC WD80EMAZ (8.0 TB), serial ..."
    QDateTime started, finished;
    QVector<StepResult> steps;
    QVector<Change> changes;
    quint64 writeErrors = 0, readErrors = 0, wrongData = 0;
    bool fake = false;
    bool completed = false; // every step ran
};

enum class Verdict { Passed, Warning, Failed, NotFinished };
Verdict verdict(const Report &report);
QString verdictText(const Report &report);
// The report as a page of HTML (for the PDF and the window).
QString html(const Report &report);

} // namespace burnin
