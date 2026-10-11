// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QStringList>

// Running other programs, the same way inside and outside the Flatpak. In a Flatpak, programs
// that aren't part of it run on the host through flatpak-spawn, which needs the user's OK:
// flatpak override --user --talk-name=org.freedesktop.Flatpak <app id>
namespace host {

bool inFlatpak();
// Whether `name` (a program on PATH, or a full path) can be run.
bool programExists(const QString &name);
// Runs a command and waits, for quick checks. Returns its exit code.
int run(QStringList command);

} // namespace host
