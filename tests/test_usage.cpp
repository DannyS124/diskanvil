// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Disk usage: the folder walk against du, and the treemap layout. The nested-mount
// check runs only as root.

#include "testkit.h"

#include "../src/tools/fswalk.h"
#include "../src/tools/squarify.h"

#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>

#include <unistd.h>

namespace {

void writeFile(const QString &path, qint64 size)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        QByteArray data(size, Qt::Uninitialized);
        QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(data.data()), size / 4);
        f.write(data);
    }
}

std::shared_ptr<UsageNode> scan(const QString &root, int *unreadable = nullptr)
{
    std::shared_ptr<UsageNode> result;
    UsageScan s(root);
    QObject::connect(&s, &UsageScan::finished, [&](bool, std::shared_ptr<UsageNode> r, int u) {
        result = r;
        if (unreadable)
            *unreadable = u;
    });
    s.run();
    return result;
}

const UsageNode *child(const UsageNode &node, const QString &name)
{
    for (const UsageNode &c : node.children) {
        if (c.name == name)
            return &c;
    }
    return nullptr;
}

} // namespace

void usageTests()
{
    QTemporaryDir dir;
    const QString root = dir.path();
    QDir(root).mkpath(QStringLiteral("videos/2026"));
    QDir(root).mkpath(QStringLiteral("docs"));
    QDir(root).mkpath(QStringLiteral("many"));
    writeFile(root + QStringLiteral("/videos/2026/big.bin"), 6 << 20);
    writeFile(root + QStringLiteral("/videos/small.bin"), 300 << 10);
    writeFile(root + QStringLiteral("/docs/a.txt"), 5000);
    for (int i = 0; i < 100; ++i)
        writeFile(root + QStringLiteral("/many/f%1").arg(i), 1024 * (i + 1)); // 5 MB in all, less than videos
    // A hard link to the big file: du counts it once, so must we.
    const bool linked = ::link(QFile::encodeName(root + QStringLiteral("/videos/2026/big.bin")).constData(),
                               QFile::encodeName(root + QStringLiteral("/docs/big-link.bin")).constData()) == 0;
    QFile::link(root + QStringLiteral("/videos"), root + QStringLiteral("/docs/shortcut")); // a symlink isn't followed

    int unreadable = 0;
    const auto tree = scan(root, &unreadable);
    const quint64 du = sh(QStringLiteral("du"), {QStringLiteral("-sxB1"), root}).section(QLatin1Char('\t'), 0, 0).toULongLong();
    report(tree && tree->size == du, QStringLiteral("total matches du -sx"), QStringLiteral("%1 vs %2").arg(tree ? tree->size : 0).arg(du));
    report(linked && tree && tree->files == 104, QStringLiteral("a hard-linked file is counted once"), QString::number(tree ? tree->files : 0));
    auto sorted = [](const UsageNode *n) {
        if (!n)
            return false;
        for (qsizetype i = 1; i < n->children.size(); ++i) {
            if (n->children[i].size > n->children[i - 1].size)
                return false;
        }
        return true;
    };
    report(sorted(tree.get()), QStringLiteral("biggest folder comes first"));
    const UsageNode *many = tree ? child(*tree, QStringLiteral("many")) : nullptr;
    const UsageNode *folded = nullptr;
    for (const UsageNode &c : many ? many->children : QVector<UsageNode>()) {
        if (c.name.contains(QLatin1String("smaller")))
            folded = &c;
    }
    report(many && many->children.size() == UsageScan::kFilesPerFolder + 1 && many->files == 100 && folded
               && folded->files == quint64(100 - UsageScan::kFilesPerFolder),
           QStringLiteral("small files are folded into one entry"), folded ? folded->name : QString());
    report(sorted(many), QStringLiteral("biggest file comes first"));
    report(unreadable == 0, QStringLiteral("no folder was unreadable"));

    int code = -1;
    if (geteuid() == 0) {
        QDir(root).mkpath(QStringLiteral("mnt"));
        sh(QStringLiteral("mount"), {QStringLiteral("-t"), QStringLiteral("tmpfs"), QStringLiteral("-o"), QStringLiteral("size=16m"),
                                     QStringLiteral("none"), root + QStringLiteral("/mnt")}, &code);
    }
    if (geteuid() == 0 && code != 0) {
        // Root inside a container usually can't mount anything.
        out << "SKIP  couldn't mount a tmpfs, so the nested mount isn't tested" << Qt::endl;
    } else if (geteuid() == 0) {
        writeFile(root + QStringLiteral("/mnt/elsewhere.bin"), 4 << 20);
        const auto again = scan(root);
        const UsageNode *mnt = again ? child(*again, QStringLiteral("mnt")) : nullptr;
        report(code == 0 && mnt && mnt->skipped && mnt->size == 0, QStringLiteral("a file system mounted inside is skipped"));
        report(again && again->size - tree->size < 64 * 1024, QStringLiteral("and adds nothing to the total"),
               QString::number(again ? qint64(again->size - tree->size) : -1));
        sh(QStringLiteral("umount"), {root + QStringLiteral("/mnt")});
    }

    // Treemap layout.
    const QVector<double> sizes = {500, 300, 120, 50, 20, 6, 3, 1, 0};
    const QRectF box(0, 0, 400, 250);
    const QVector<QRectF> rects = squarify(sizes, box);
    double area = 0, worstAspect = 0;
    bool inside = true, overlap = false, proportional = true;
    for (int i = 0; i < 8; ++i) {
        area += rects[i].width() * rects[i].height();
        inside = inside && box.adjusted(-0.01, -0.01, 0.01, 0.01).contains(rects[i]);
        proportional = proportional && std::abs(rects[i].width() * rects[i].height() / (400.0 * 250) - sizes[i] / 1000) < 0.001;
        worstAspect = std::max(worstAspect, std::max(rects[i].width() / rects[i].height(), rects[i].height() / rects[i].width()));
        for (int j = 0; j < i; ++j)
            overlap = overlap || rects[i].adjusted(0.01, 0.01, -0.01, -0.01).intersects(rects[j]);
    }
    report(inside && !overlap && std::abs(area - 400.0 * 250) < 1, QStringLiteral("treemap boxes fill the space without overlapping"));
    report(proportional, QStringLiteral("each box's area matches its size"));
    report(worstAspect < 4, QStringLiteral("boxes stay roughly square"), QString::number(worstAspect, 'f', 2));
    report(rects[8].isEmpty(), QStringLiteral("an empty folder gets no box"));
}
