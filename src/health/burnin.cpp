// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "burnin.h"

#include <QCoreApplication>
#include <QHash>
#include <QLocale>

namespace burnin {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("burnin", text);
}

struct Watched {
    int id;            // ATA attribute
    const char *key;   // NVMe
    const char *name;
    Change::Weight weight;
    const char *meaning;
};

// What a new drive should never gain, and what only points at the cable or the heat.
const Watched kWatched[] = {
    {5, nullptr, QT_TRANSLATE_NOOP("burnin", "Reallocated sectors"), Change::Weight::Bad,
     QT_TRANSLATE_NOOP("burnin", "Parts of the disk that failed and were swapped for spares. A new drive gaining any under test is one to send back.")},
    {196, nullptr, QT_TRANSLATE_NOOP("burnin", "Reallocation events"), Change::Weight::Bad,
     QT_TRANSLATE_NOOP("burnin", "Times it had to swap in a spare.")},
    {197, nullptr, QT_TRANSLATE_NOOP("burnin", "Pending sectors"), Change::Weight::Bad,
     QT_TRANSLATE_NOOP("burnin", "Parts it couldn't read and is waiting to rewrite.")},
    {198, nullptr, QT_TRANSLATE_NOOP("burnin", "Uncorrectable sectors"), Change::Weight::Bad,
     QT_TRANSLATE_NOOP("burnin", "Parts it couldn't read even with its error correction.")},
    {187, nullptr, QT_TRANSLATE_NOOP("burnin", "Reported uncorrectable errors"), Change::Weight::Bad,
     QT_TRANSLATE_NOOP("burnin", "Reads it couldn't correct.")},
    {10, nullptr, QT_TRANSLATE_NOOP("burnin", "Spin-up retries"), Change::Weight::Bad,
     QT_TRANSLATE_NOOP("burnin", "Times the motor needed more than one try to get the disks spinning.")},
    {188, nullptr, QT_TRANSLATE_NOOP("burnin", "Command timeouts"), Change::Weight::Warning,
     QT_TRANSLATE_NOOP("burnin", "Commands that took too long: often the power or the cable, sometimes the drive.")},
    {199, nullptr, QT_TRANSLATE_NOOP("burnin", "Cable errors (UDMA CRC)"), Change::Weight::Warning,
     QT_TRANSLATE_NOOP("burnin", "Data spoiled on its way through the cable. The drive is usually fine: try another cable or port.")},
    {0, "media_errors", QT_TRANSLATE_NOOP("burnin", "Media errors"), Change::Weight::Bad,
     QT_TRANSLATE_NOOP("burnin", "Data the drive couldn't read back intact.")},
    {0, "num_err_log_entries", QT_TRANSLATE_NOOP("burnin", "Error log entries"), Change::Weight::Note,
     QT_TRANSLATE_NOOP("burnin", "Errors the drive noted. Many drives log harmless ones (a command it doesn't know); worth a look only with other signs.")},
    {0, "avail_spare", QT_TRANSLATE_NOOP("burnin", "Available spare"), Change::Weight::Bad,
     QT_TRANSLATE_NOOP("burnin", "Spare space for worn-out flash. A new drive's shouldn't drop under test.")},
    {0, "critical_temp_time", QT_TRANSLATE_NOOP("burnin", "Minutes above critical temperature"), Change::Weight::Warning,
     QT_TRANSLATE_NOOP("burnin", "It got too hot under load: it needs more air (a heatsink, a fan, a better case).")},
    {0, "warning_temp_time", QT_TRANSLATE_NOOP("burnin", "Minutes above warning temperature"), Change::Weight::Note,
     QT_TRANSLATE_NOOP("burnin", "It got warm under load. Fine now and then; if it's often, give it more air.")},
};

QString translated(const char *text)
{
    return QCoreApplication::translate("burnin", text);
}

} // namespace

QVector<Change> compare(const QVector<SmartAttribute> &before, const QVector<SmartAttribute> &after)
{
    auto find = [](const QVector<SmartAttribute> &list, const Watched &w) -> const SmartAttribute * {
        for (const SmartAttribute &a : list)
            if ((w.id && a.id == w.id) || (w.key && a.key == QLatin1String(w.key)))
                return &a;
        return nullptr;
    };
    QVector<Change> out;
    for (const Watched &w : kWatched) {
        const SmartAttribute *b = find(before, w), *a = find(after, w);
        if (!b || !a || b->rawValue < 0 || a->rawValue < 0)
            continue;
        // Spare goes down when it's bad; everything else goes up.
        const bool worse = w.key && qstrcmp(w.key, "avail_spare") == 0 ? a->rawValue < b->rawValue : a->rawValue > b->rawValue;
        if (!worse)
            continue;
        Change c;
        c.name = translated(w.name);
        c.before = b->rawValue;
        c.after = a->rawValue;
        c.weight = w.weight;
        c.meaning = translated(w.meaning);
        out << c;
    }
    return out;
}

QString stepName(Step step)
{
    switch (step) {
    case Step::Before:
        return tr("Health before");
    case Step::ShortTest:
        return tr("The drive's short self-test");
    case Step::Write:
        return tr("Every byte written and read back");
    case Step::LongTest:
        return tr("The drive's long self-test");
    case Step::After:
        return tr("Health after, compared");
    }
    return {};
}

Verdict verdict(const Report &r)
{
    bool failed = r.fake || r.writeErrors || r.readErrors || r.wrongData;
    bool warning = false;
    for (const StepResult &s : r.steps)
        failed |= s.outcome == StepResult::Outcome::Failed;
    for (const Change &c : r.changes) {
        failed |= c.weight == Change::Weight::Bad;
        warning |= c.weight == Change::Weight::Warning;
    }
    if (failed)
        return Verdict::Failed;
    if (!r.completed)
        return Verdict::NotFinished;
    return warning ? Verdict::Warning : Verdict::Passed;
}

QString verdictText(const Report &r)
{
    bool skipped = false;
    for (const StepResult &s : r.steps)
        skipped |= s.outcome == StepResult::Outcome::Skipped;
    switch (verdict(r)) {
    case Verdict::Passed:
        return skipped ? tr("Passed: every byte was written and read back without an error. Its own health couldn't be checked (it doesn't "
                            "report it this way), so that part is unknown.")
                       : tr("Passed: it did everything asked of it without a single error. It's ready for your files.");
    case Verdict::Warning:
        return tr("Passed, with something to see to: the drive itself did fine, but read the notes (usually the cable or the heat).");
    case Verdict::Failed:
        return r.fake ? tr("Failed: it holds less than it says. It's a fake: send it back.")
                      : tr("Failed: it had errors under test. A new drive shouldn't: send it back while you can, and don't trust it with "
                           "files you can't lose.");
    case Verdict::NotFinished:
        return tr("Not finished: only the steps that ran are shown.");
    }
    return {};
}

QString html(const Report &r)
{
    const QLocale locale;
    auto esc = [](const QString &s) { return s.toHtmlEscaped(); };
    QString out = QStringLiteral("<h2>%1</h2><p><b>%2</b></p><p>%3</p>")
                      .arg(esc(tr("Burn-in report")), esc(r.drive), esc(verdictText(r)));
    out += QStringLiteral("<p>%1</p>").arg(esc(tr("Started %1, finished %2.")
                                                  .arg(locale.toString(r.started.toLocalTime(), QLocale::ShortFormat),
                                                       r.finished.isValid() ? locale.toString(r.finished.toLocalTime(), QLocale::ShortFormat) : tr("not yet"))));
    out += QStringLiteral("<table border=\"1\" cellspacing=\"0\" cellpadding=\"4\"><tr><th>%1</th><th>%2</th><th>%3</th></tr>")
               .arg(esc(tr("Step")), esc(tr("Result")), esc(tr("What happened")));
    for (const StepResult &s : r.steps) {
        QString outcome;
        switch (s.outcome) {
        case StepResult::Outcome::NotRun:
            outcome = tr("Not run");
            break;
        case StepResult::Outcome::Passed:
            outcome = tr("Passed");
            break;
        case StepResult::Outcome::Failed:
            outcome = tr("Failed");
            break;
        case StepResult::Outcome::Skipped:
            outcome = tr("Skipped");
            break;
        case StepResult::Outcome::Stopped:
            outcome = tr("Stopped");
            break;
        }
        out += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td></tr>").arg(esc(stepName(s.step)), esc(outcome), esc(s.detail));
    }
    out += QStringLiteral("</table>");
    if (!r.changes.isEmpty()) {
        out += QStringLiteral("<h3>%1</h3><ul>").arg(esc(tr("Health counters that went up")));
        for (const Change &c : r.changes)
            out += QStringLiteral("<li><b>%1</b>: %2 → %3. %4</li>").arg(esc(c.name)).arg(c.before).arg(c.after).arg(esc(c.meaning));
        out += QStringLiteral("</ul>");
    } else if (r.completed) {
        out += QStringLiteral("<p>%1</p>").arg(esc(tr("None of its health counters that matter went up.")));
    }
    return out;
}

} // namespace burnin
