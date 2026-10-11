// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "applog.h"

#include <QDateTime>
#include <QFile>
#include <QMutex>

#include <cstdio>

// Off (warnings only) until start() turns it on.
Q_LOGGING_CATEGORY(lcOps, "diskforge.ops", QtWarningMsg)

namespace {

QMutex lock; // workers log from their own threads
QFile *file = nullptr;
QtMessageHandler previous = nullptr;

const char *levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg: return "debug";
    case QtInfoMsg: return "info";
    case QtWarningMsg: return "warning";
    case QtCriticalMsg: return "critical";
    case QtFatalMsg: return "fatal";
    }
    return "?";
}

void handler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    {
        QMutexLocker locker(&lock);
        if (file) {
            QString line = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz "));
            line += QLatin1String(levelName(type));
            if (context.category && qstrcmp(context.category, "default") != 0)
                line += QLatin1Char(' ') + QLatin1String(context.category);
            line += QLatin1String(": ") + message + QLatin1Char('\n');
            file->write(line.toUtf8());
            file->flush(); // it's for when things go wrong, crashes included
        }
    }
    // DiskForge's own notes only go to the file; everything else also goes where it always did.
    const bool ours = context.category && qstrcmp(context.category, "diskforge.ops") == 0 && type < QtWarningMsg;
    if (previous && !ours)
        previous(type, context, message);
}

} // namespace

bool applog::start(const QString &path)
{
    auto *f = new QFile(path);
    if (!f->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        std::fprintf(stderr, "Can't write the log to %s: %s\n", qPrintable(path), qPrintable(f->errorString()));
        delete f;
        return false;
    }
    QMutexLocker locker(&lock);
    delete file;
    file = f;
    if (!previous)
        previous = qInstallMessageHandler(handler);
    const_cast<QLoggingCategory &>(lcOps()).setEnabled(QtInfoMsg, true);
    return true;
}

bool applog::enabled()
{
    QMutexLocker locker(&lock);
    return file != nullptr;
}

QString applog::path()
{
    QMutexLocker locker(&lock);
    return file ? file->fileName() : QString();
}
