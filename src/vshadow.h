// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Windows' shadow copies (Volume Shadow Copy: restore points, "previous versions", backups):
// earlier states of a whole NTFS volume, kept on the volume itself. A copy is the volume as it
// is now, with the 16 KiB blocks that changed since put back from where Windows saved them
// before they changed.
//
// Our own code, from the libyal project's documentation of the format. Where it leaves things
// open (how a store's block list adds up, reading through newer copies), libvshadow's behaviour
// was followed, and the result checked against what Windows itself reads from its copies.

#include "lostsource.h"

#include <QString>
#include <QVector>

#include <memory>

namespace vss {

struct Copy {
    QString id;         // the shadow copy's ID, the way Windows shows it: {8A21240D-8D48-...}
    qint64 created = 0; // ms since 1970
    quint64 size = 0;   // the volume's size then
};

// The shadow copies of the NTFS volume at `where` in `source`, oldest first. Empty when there
// are none; `problem` says why when there are some that can't be read.
QVector<Copy> copies(lost::Source &source, const lost::Extent &where, QString *problem = nullptr);

// Copy number `index` (as copies() lists them) as a volume of its own, read through `source`.
std::shared_ptr<lost::Source> open(const std::shared_ptr<lost::Source> &source, const lost::Extent &where, int index,
                                   QString *problem = nullptr);

} // namespace vss
