// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

class QQmlApplicationEngine;

// diskanvil-shell --selftest: the shell used with real windows in it; 0 when every step passed.
int runSelfTest(QQmlApplicationEngine &engine);
