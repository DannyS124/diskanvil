// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A drive's activity, in time order: every time Find Lost Files knows of each file (made,
// written, opened, changed, deleted, the date a photo was taken) as events of their own, to see
// what happened when.

#include "lostscan.h"

#include <QString>
#include <QVector>

namespace timeline {

struct Event {
    enum class Kind { Created, Written, Opened, Changed, Deleted, Taken };
    qint64 when = 0; // ms since 1970
    Kind kind = Kind::Written;
    int file = -1;   // in the list the events were made from
    bool dayOnly = false; // only the day is known (FAT's "opened")
};

// The events, oldest first. Times before 1980 or more than a day in the future are left out:
// they're not real (an unset clock, a damaged record).
QVector<Event> events(const QVector<lost::Found> &files, qint64 now);
QString kindName(Event::Kind kind);
// As CSV (a spreadsheet opens it): when, what happened, name, folder, size, deleted or not.
QByteArray csv(const QVector<Event> &events, const QVector<lost::Found> &files);

} // namespace timeline
