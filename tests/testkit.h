// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Shared by the self-test suites.

#include <QStringList>
#include <QTextStream>

#include <functional>

class UDisks;
struct Disk;

extern QTextStream out;
extern int failures;

void report(bool ok, const QString &step, const QString &detail = {});
// Runs a program and returns its trimmed standard output.
QString sh(const QString &program, const QStringList &args, int *exitCode = nullptr);
QString sha256File(const QString &path, qint64 offset = 0, qint64 length = -1);

// UDisks helpers. run() waits for operationFinished and reports it as a step (or hands
// the message back without reporting).
bool run(UDisks &udisks, const QString &step, const std::function<void()> &op, QString *message = nullptr);
QString loopSetup(const QString &path, QString *error);
void loopDelete(const QString &objectPath);
const Disk *diskWithFile(UDisks &udisks, const QString &file);
// Polls UDisks until `check` holds for the loop device backed by `file` (15 s at most).
// quietMs: also wait until its partitions have stopped changing for that long (udev
// re-reads a partition table a moment after a program that wrote to the disk closes it).
const Disk *waitForDisk(UDisks &udisks, const QString &file, const std::function<bool(const Disk &)> &check, int quietMs = 0);
// Opens a disk or partition through UDisks and returns the fd (-1 on failure).
int openBlockFd(UDisks &udisks, const QString &objectPath, int mode);

// Suites in their own files.
void gptTests();
void copyTests();
void blockMapTests();
void usageTests();
void backupTests();
void rescueTests();
void rescueMapTests();
void cloneTests();
void cleanupTests();
void snapperTests();
void btrfsTests();
void optimizeTests();
void catalogTests();
void fuzzTests();
void healthTests();
void jobsTests();
void recoverTests();
void rescueUsbTests();
void stickCheckTests();
void imageTests();
void isoModeTests();
void windowsTests();
void lostFilesTests();
void virusTests();
void registryTests();
void startupTests();
void bootTests();
void newIdsTests();
int scanImage(const QString &path);
void raidTests();
void lvmTests();
void stopTests();
