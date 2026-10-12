// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "btrfsrestore.h"

#include "hostprocess.h"
#include "lostfs.h"
#include "lostscan.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace btrfsrestore {

bool available()
{
    return host::programExists(QStringLiteral("btrfs")) && host::programExists(QStringLiteral("btrfs-find-root"));
}

QVector<lost::Extent> volumes(lost::Source &source)
{
    QVector<lost::Extent> out;
    QVector<lost::Extent> places = lost::partitionsOf(source);
    places.prepend({0, source.size()});
    QSet<quint64> seen;
    for (const lost::Extent &where : std::as_const(places)) {
        // The superblock is 64 KiB in; its magic 64 bytes into that.
        char magic[8];
        if (seen.contains(where.start) || where.length < 16ull * 1024 * 1024 || source.read(where.start + 0x10040, magic, 8) != 8
            || memcmp(magic, "_BHRfS_M", 8) != 0)
            continue;
        seen.insert(where.start);
        out << where;
    }
    // The whole source and its first partition can be the same thing: a partition image.
    return out;
}

QVector<Tree> parseTrees(const QString &output)
{
    static const QRegularExpression current(QStringLiteral("Found tree root at (\\d+) gen (\\d+)"));
    static const QRegularExpression older(QStringLiteral("Well block (\\d+)\\s*\\(gen: (\\d+)"));
    QVector<Tree> out;
    quint64 now = 0;
    if (const QRegularExpressionMatch m = current.match(output); m.hasMatch()) {
        now = m.captured(2).toULongLong();
        out << Tree{0, now};
    }
    QVector<Tree> earlier;
    QSet<quint64> roots;
    for (auto it = older.globalMatch(output); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const Tree t{m.captured(1).toULongLong(), m.captured(2).toULongLong()};
        if (!roots.contains(t.at) && (now == 0 || t.generation < now)) {
            roots.insert(t.at);
            earlier << t;
        }
    }
    std::stable_sort(earlier.begin(), earlier.end(), [](const Tree &a, const Tree &b) { return a.generation > b.generation; });
    return out + earlier;
}

QStringList parseRestored(const QString &output, const QString &target)
{
    QStringList out;
    const QString prefix = QStringLiteral("Restoring ") + target;
    for (QString line : output.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (!line.startsWith(prefix))
            continue;
        QString path = line.mid(prefix.size());
        while (path.startsWith(QLatin1String("//")))
            path.remove(0, 1);
        if (!path.startsWith(QLatin1Char('/')))
            path.prepend(QLatin1Char('/'));
        if (path.size() > 1 && !out.contains(path))
            out << path;
    }
    return out;
}

QStringList findTreesCommand(const QString &device)
{
    return {QStringLiteral("btrfs-find-root"), device};
}

QStringList restoreCommand(const QString &device, quint64 tree, const QString &target, bool dryRun, const QStringList &only)
{
    QStringList command = {QStringLiteral("btrfs"), QStringLiteral("restore"), QStringLiteral("-vv"), QStringLiteral("-i")};
    if (dryRun)
        command << QStringLiteral("-D");
    if (tree)
        command << QStringLiteral("-t") << QString::number(tree);
    if (!only.isEmpty())
        command << QStringLiteral("--path-regex") << pathRegex(only);
    return command << device << target;
}

QString pathRegex(const QStringList &paths)
{
    // Every folder on the way down has to match too, the root as "/".
    QSet<QString> allowed;
    for (const QString &path : paths) {
        const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QString so;
        for (const QString &part : parts) {
            so += QLatin1Char('/') + part;
            allowed.insert(so.mid(1));
        }
    }
    QStringList sorted(allowed.cbegin(), allowed.cend());
    std::sort(sorted.begin(), sorted.end());
    // An extended regular expression: its special characters escaped.
    QStringList alternatives = {QString()};
    for (const QString &s : std::as_const(sorted)) {
        QString escaped;
        for (const QChar c : s) {
            if (QStringLiteral(".[]()*+?{}|^$\\").contains(c))
                escaped += QLatin1Char('\\');
            escaped += c;
        }
        alternatives << escaped;
    }
    return QStringLiteral("^/(%1)$").arg(alternatives.join(QLatin1Char('|')));
}

} // namespace btrfsrestore
