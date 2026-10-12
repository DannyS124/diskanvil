// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Signs of ransomware among found files: the notes it leaves ("_readme.txt", "HOW_TO_DECRYPT")
// and the endings it adds to the files it locks ("photo.jpg.locky"). From those, which family it
// probably is, and whether a free decryptor for it has been made public (No More Ransom, the
// police and security companies' project, collects them). Our own table of well-known families;
// a family that isn't in it still shows up by its pattern: many files with the same new ending.

#include "lostscan.h"

#include <QString>
#include <QStringList>

namespace ransomware {

struct Finding {
    bool found() const { return locked > 0 || !notes.isEmpty(); }
    QString family;     // the best guess, empty when it isn't a family in the table
    QString ending;     // the ending it adds, as seen most (".locky")
    int locked = 0;     // files with that ending
    QStringList notes;  // the ransom notes found (folder/name)
    QStringList examples; // a few of the locked files
    QString decryptor;  // where a free decryptor has been made public for it, in a few words; empty if none is known
    int deletedOriginals = 0; // locked files whose original (the same name without the ending) is among the deleted files
};

Finding look(const QVector<lost::Found> &files);

} // namespace ransomware
