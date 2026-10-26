// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Find Lost Files without its window, for the recovery benchmark: reads a disk image the way the
// window does (the quick scan of the file systems, then the deep scan for files by their contents,
// skipping what the quick scan already placed) and saves everything it found into a folder.
//
//   diskanvil-recoverbench <image> <output folder>
//
// Prints one line of counts at the end. The image is only ever read.

#include "lostfiles/lostscan.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QTextStream>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    const QStringList args = app.arguments();
    if (args.size() != 3) {
        out << "usage: diskanvil-recoverbench <image> <output folder>" << Qt::endl;
        return 2;
    }
    QString error;
    const std::shared_ptr<lost::Source> source = lost::Source::fromImage(args[1], &error);
    if (!source) {
        out << "can't open " << args[1] << ": " << error << Qt::endl;
        return 1;
    }
    QElapsedTimer clock;
    clock.start();

    QVector<lost::Found> files;
    int byName = 0, byContents = 0;
    lost::QuickScan quick(source);
    QObject::connect(&quick, &lost::QuickScan::found, [&](const QVector<lost::Found> &batch) {
        files += batch;
        byName += int(batch.size());
    });
    quick.run();

    lost::DeepScan deep(source);
    deep.skip(quick.knownRanges(), quick.knownStarts());
    QObject::connect(&deep, &lost::DeepScan::found, [&](const QVector<lost::Found> &batch) {
        files += batch;
        byContents += int(batch.size());
    });
    deep.run();
    const qint64 scanned = clock.elapsed();

    QDir().mkpath(args[2]);
    int saved = 0, failed = 0;
    lost::Saver saver(source, files, args[2]);
    QObject::connect(&saver, &lost::Saver::finished, [&](int s, int f, const QString &, const QString &) {
        saved = s;
        failed = f;
    });
    saver.run();

    out << "found " << files.size() << " (" << byName << " by name, " << byContents << " by contents), saved " << saved << ", failed "
        << failed << ", scan " << scanned << " ms, total " << clock.elapsed() << " ms" << Qt::endl;
    return 0;
}
