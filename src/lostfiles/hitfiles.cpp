// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hitfiles.h"

#include "format.h"
#include "lostfs.h"
#include "lostscan.h"
#include "rescuecopy.h"

#include <QCoreApplication>

#include <algorithm>

namespace hitfiles {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("hitfiles", text);
}

QString typeName(const QString &type)
{
    if (type == QLatin1String("vfat"))
        return QStringLiteral("FAT");
    if (type == QLatin1String("exfat"))
        return QStringLiteral("exFAT");
    if (type == QLatin1String("ntfs"))
        return QStringLiteral("NTFS");
    return type;
}

} // namespace

Report find(lost::Source &copy, const RescueMap &map, const std::atomic<bool> &cancel, const std::function<void(const QString &)> &progress)
{
    Report r;
    r.missing = map.missing(0, map.size());
    if (r.missing == 0)
        return r;
    const QVector<lost::Extent> places = lost::partitionsOf(copy);
    QVector<lost::Extent> read; // where a file system was read
    for (int p = 0; p < places.size() && !cancel; ++p) {
        const lost::Extent where = places[p];
        // A partition already covered by one read before (the whole image, when it's one partition) is skipped.
        const bool inside = std::any_of(read.cbegin(), read.cend(), [&where](const lost::Extent &e) { return where.start >= e.start && where.end() <= e.end(); });
        if (inside)
            continue;
        lost::Volume volume;
        const bool ok = lost::readFileSystem(copy, where, [&](QVector<lost::Found> &&files) {
            for (const lost::Found &f : files) {
                if (f.nameOnly || f.extents.isEmpty())
                    continue;
                if (!f.deleted)
                    ++r.files;
                quint64 missing = 0;
                for (const lost::Extent &e : f.extents)
                    if (!e.zeros())
                        missing += map.missing(e.start, e.length);
                if (missing == 0)
                    continue;
                Hit h;
                h.path = f.folder.isEmpty() ? f.name : f.folder + QLatin1Char('/') + f.name;
                h.size = f.size;
                h.missing = f.packing.packed() ? missing : std::min(missing, f.size);
                h.deleted = f.deleted;
                h.guessed = f.piecesGuessed;
                h.packed = f.packing.packed();
                r.hits << h;
                if (!f.deleted)
                    r.inFiles += h.missing;
            }
            if (progress)
                progress(QCoreApplication::translate("hitfiles", "Reading %1: %n file(s) hit so far", nullptr, int(r.hits.size())).arg(volume.label));
        }, cancel, &volume);
        if (ok) {
            read << where;
            r.volumes << QStringLiteral("%1 (%2)").arg(volume.label, typeName(volume.type));
        }
    }
    // Bad spots on no file system that was read: a Btrfs or other partition, or the tables themselves.
    for (int p = 1; p < places.size(); ++p) {
        const lost::Extent where = places[p];
        const bool covered = std::any_of(read.cbegin(), read.cend(), [&where](const lost::Extent &e) { return where.start >= e.start && where.end() <= e.end(); });
        const quint64 there = map.missing(where.start, where.length);
        if (!covered && there > 0)
            r.unread << tr("partition %1 (%2 not read; DiskAnvil can't list the files of its file system)").arg(p).arg(formatSize(there));
    }
    if (read.isEmpty() && places.size() == 1)
        r.unread << tr("the whole copy: no FAT, exFAT, NTFS or ext4 file system was found on it (Btrfs and others can't be listed yet)");
    std::sort(r.hits.begin(), r.hits.end(), [](const Hit &a, const Hit &b) {
        if (a.deleted != b.deleted)
            return !a.deleted;
        return a.missing > b.missing;
    });
    return r;
}

QString asText(const Report &r, const QString &copyName)
{
    QStringList out;
    out << tr("Which files the bad spots hit, in %1").arg(copyName);
    out << tr("Not read: %1, of which %2 in files.").arg(formatSize(r.missing), formatSize(r.inFiles));
    out << tr("File systems read: %1").arg(r.volumes.isEmpty() ? tr("none") : r.volumes.join(QStringLiteral(", ")));
    for (const QString &u : r.unread)
        out << tr("Not listed: %1").arg(u);
    out << QString();
    int hit = 0;
    for (const Hit &h : r.hits)
        if (!h.deleted)
            ++hit;
    out << tr("Files that came off whole: %1. Files with parts missing (blank in the copy): %2.").arg(std::max(0, r.files - hit)).arg(hit);
    for (const Hit &h : r.hits) {
        QStringList notes;
        if (h.deleted)
            notes << tr("deleted");
        if (h.guessed)
            notes << tr("where its pieces are is a guess");
        if (h.packed)
            notes << tr("compressed by Windows: more of it may be lost");
        out << QStringLiteral("%1\t%2 of %3 missing%4").arg(h.path, formatSize(h.missing), formatSize(h.size),
                                                            notes.isEmpty() ? QString() : QStringLiteral(" (") + notes.join(QStringLiteral(", ")) + QLatin1Char(')'));
    }
    return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

} // namespace hitfiles
