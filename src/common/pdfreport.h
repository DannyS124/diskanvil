// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A page of HTML saved as a PDF (A4), with Qt's own PDF writer: certificates and reports.

#include <QString>

class QWidget;

namespace pdfreport {

bool save(const QString &html, const QString &path, const QString &title, QString *error);
// Asks where (suggesting `name`), saves, and says if it couldn't. True when it was saved.
bool saveAs(QWidget *parent, const QString &html, const QString &name, const QString &title);

} // namespace pdfreport
