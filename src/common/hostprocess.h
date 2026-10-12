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
#ifdef Q_OS_WIN
// Where a program is on Windows: next to diskanvil.exe (sqlite3.exe comes with it), on the PATH,
// or where its own installer puts it (smartmontools); the bare name when it's nowhere.
QString windowsProgram(const QString &name);
#endif
// Runs a command and waits, for quick checks. Returns its exit code.
int run(QStringList command);
// The command to give QProcess for it: as it is, or through flatpak-spawn in the Flatpak.
QStringList command(QStringList command);

} // namespace host
