// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QLoggingCategory>
#include <QString>

// The log DiskForge keeps when asked to (--log <file>, or DISKFORGE_LOG): every call to UDisks
// and how it went, every result shown, and Qt's own warnings, one timestamped line each.
// DiskForge Live turns it on, so its logs folder has what DiskForge did.
//
// What goes along with a call is never written down, since that's where passphrases are.
namespace applog {

// Starts writing to `path` (appending). False if the file can't be opened.
bool start(const QString &path);
bool enabled();
QString path();

} // namespace applog

Q_DECLARE_LOGGING_CATEGORY(lcOps)
