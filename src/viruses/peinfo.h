// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What a Windows program or driver says about itself: the strings in its version resource
// (CompanyName "NVIDIA Corporation", FileDescription "NVIDIA Kernel Mode Driver"...). Our own
// reader of the PE file's resource tree.

#include <QByteArray>
#include <QHash>
#include <QString>

namespace peinfo {

// Its version strings, by name; empty when it has none (or isn't a PE file).
QHash<QString, QString> versionStrings(const QByteArray &file);

} // namespace peinfo
