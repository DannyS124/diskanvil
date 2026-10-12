// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Let Someone Help, on DiskAnvil Live: someone on the same network sees this screen and can use it
// (x11vnc, run as the live user), or works in a terminal (SSH, switched on by the
// diskanvil-live-remote helper). Passwords are made up each time, and stop working when it's
// switched off. On a PC, the system's own tools do this (Quick Assist, the desktop's screen sharing).

#include <QString>
#include <QStringList>

namespace remotehelp {

// From letters and digits that can't be mixed up when read out (no 0 and o, no 1, l and i).
QString makePassword(int length);
// This PC's addresses others on the network can reach it at, IPv4 first.
QStringList addresses();
bool available(); // DiskAnvil Live, with x11vnc and the helper

// x11vnc's arguments: this screen, a password read from `passwordFile` (and removed), more than one
// viewer, until it's stopped. It prints the port it took ("PORT=5900").
QStringList vncArguments(const QString &passwordFile);
// From a line of x11vnc's messages: the address of a viewer that connected ("Got connection from
// client 192.168.1.5"), or -1 viewers when it says how many are left ("client_count: 0").
QString connectedFrom(const QString &line);
int clientsLeft(const QString &line);

// The helper, as root: "ssh-on" (the password on its input) or "ssh-off".
QStringList sshCommand(const QString &what);

// The tests use their own stand-ins.
inline QString x11vncForTest, helperForTest;
QString x11vnc();

} // namespace remotehelp
