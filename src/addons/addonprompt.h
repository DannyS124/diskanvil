// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The questions asked before an add-on is installed or run. Everything in them comes from
// the add-on file or the drive, so it's shown as plain text (or escaped).

#include "addons.h"

class QWidget;

bool askInstallAddon(QWidget *parent, const Addon &addon);
// remember is set if they ticked "Don't ask again", which is only offered when that's safe.
bool askRunAddon(QWidget *parent, const Addon &addon, const AddonAction &action, const QStringList &argv, bool *remember);

// What an action can do (admin power, a shell, the network...) and where it's offered, as HTML.
QString addonNotes(const AddonAction &action);
QString addonNotes(const AddonAction &action, const AddonRisks &risks);
// The warning for an add-on that got into the folder without going through DiskForge, as HTML.
QString outsideNote();
