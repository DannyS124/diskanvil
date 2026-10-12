// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "oldname.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace oldname {

Moved moveOver()
{
    Moved moved;
    for (const QString &file : {QStringLiteral("diskanvil"), QStringLiteral("addons")}) {
        QSettings now(QStringLiteral("diskanvil"), file);
        const QSettings before(QStringLiteral("diskforge"), file == QLatin1String("diskanvil") ? QStringLiteral("diskforge") : file);
        if (!now.allKeys().isEmpty() || before.allKeys().isEmpty())
            continue;
        for (const QString &key : before.allKeys()) {
            QVariant value = before.value(key);
            if (key == QLatin1String("theme") && value.toString() == QLatin1String("diskforge-live"))
                value = QStringLiteral("diskanvil-live");
            now.setValue(key, value);
        }
        now.sync();
        moved.settings = true;
    }
    // On Linux both are ~/.local/share/diskanvil; on Windows the first is in Local, the second in Roaming.
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString app = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QVector<QPair<QString, QString>> folders = {{data + QStringLiteral("/diskforge"), data + QStringLiteral("/diskanvil")}};
    if (!app.isEmpty() && QFileInfo(app).fileName() == QLatin1String("diskanvil")) {
        const QString before = QFileInfo(app).path() + QStringLiteral("/diskforge");
        if (QDir::cleanPath(before) != QDir::cleanPath(folders.first().first))
            folders.append({before, app});
    }
    for (const auto &[from, to] : std::as_const(folders)) {
        if (!QFileInfo(from).isDir() || QFileInfo::exists(to))
            continue;
        QDir().mkpath(QFileInfo(to).path());
        if (QDir().rename(from, to))
            moved.folders << to;
    }
    return moved;
}

} // namespace oldname
