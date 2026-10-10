// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Windows shortcuts (.lnk files): what they start. Our own reader of the parts that say so
// (Microsoft's [MS-SHLLINK]), for the startup folders of a Windows that isn't running.

#include <QByteArray>
#include <QString>

namespace shelllink {

struct Link {
    QString target;      // the program or file, as Windows has it: "C:\Windows\notepad.exe"
    QString arguments;
    QString workingDir;
    QString relativePath; // from the shortcut's own folder, when that's all there is
};

// False when it isn't a shortcut or is too damaged to say what it starts.
bool parse(const QByteArray &bytes, Link *link);

} // namespace shelllink
