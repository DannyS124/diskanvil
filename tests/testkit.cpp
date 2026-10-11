// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "testkit.h"

#include "../src/drives/udisks.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QThread>

QTextStream out(stdout);
int failures = 0;

void report(bool ok, const QString &step, const QString &detail)
{
    out << (ok ? "PASS  " : "FAIL  ") << step;
    if (!detail.isEmpty())
        out << "  (" << detail << ")";
    out << Qt::endl;
    if (!ok)
        ++failures;
}

QString sh(const QString &program, const QStringList &args, int *exitCode)
{
    QProcess p;
    p.start(program, args);
    p.waitForFinished(120000);
    if (exitCode)
        *exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    return QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
}

QString sha256File(const QString &path, qint64 offset, qint64 length)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(offset))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 left = length < 0 ? f.size() - offset : length;
    while (left > 0) {
        const QByteArray chunk = f.read(std::min<qint64>(left, 4 << 20));
        if (chunk.isEmpty())
            break;
        hash.addData(chunk);
        left -= chunk.size();
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool run(UDisks &udisks, const QString &step, const std::function<void()> &op, QString *message)
{
    bool done = false, ok = false;
    QString text;
    QEventLoop loop;
    auto conn = QObject::connect(&udisks, &UDisks::operationFinished, [&](bool success, const QString &m) {
        done = true;
        ok = success;
        text = m;
        loop.quit();
    });
    op();
    if (!done)
        loop.exec();
    QObject::disconnect(conn);
    if (message)
        *message = text;
    else
        report(ok, step, text);
    return ok;
}

QString loopSetup(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite)) {
        *error = file.errorString();
        return {};
    }
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.UDisks2"),
        QStringLiteral("/org/freedesktop/UDisks2/Manager"), QStringLiteral("org.freedesktop.UDisks2.Manager"),
        QStringLiteral("LoopSetup"));
    call << QVariant::fromValue(QDBusUnixFileDescriptor(file.handle()))
         << QVariantMap{{QStringLiteral("auth.no_user_interaction"), true}};
    const QDBusMessage reply = QDBusConnection::systemBus().call(call, QDBus::Block, 30000);
    if (reply.type() != QDBusMessage::ReplyMessage) {
        *error = reply.errorMessage();
        return {};
    }
    return reply.arguments().value(0).value<QDBusObjectPath>().path();
}

void loopDelete(const QString &objectPath)
{
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.UDisks2"), objectPath,
        QStringLiteral("org.freedesktop.UDisks2.Loop"), QStringLiteral("Delete"));
    call << QVariantMap{{QStringLiteral("auth.no_user_interaction"), true}};
    QDBusConnection::systemBus().call(call, QDBus::Block, 30000);
}

const Disk *diskWithFile(UDisks &udisks, const QString &file)
{
    for (const Disk &d : udisks.disks()) {
        if (d.isLoop && d.backingFile == file)
            return &d;
    }
    return nullptr;
}

const Disk *waitForDisk(UDisks &udisks, const QString &file, const std::function<bool(const Disk &)> &check, int quietMs)
{
    QElapsedTimer timer, quiet;
    timer.start();
    QString last;
    while (timer.elapsed() < 15000 + quietMs) {
        udisks.refresh();
        const Disk *d = diskWithFile(udisks, file);
        if (d && check(*d)) {
            QString now;
            for (const Volume &v : d->volumes)
                now += v.objectPath + v.uuid + QString::number(v.size) + QLatin1Char(';');
            if (now != last || !quiet.isValid()) {
                last = now;
                quiet.start();
            }
            if (quiet.elapsed() >= quietMs)
                return d;
        } else {
            quiet.invalidate();
        }
        QCoreApplication::processEvents();
        QThread::msleep(200);
    }
    return nullptr;
}

int openBlockFd(UDisks &udisks, const QString &objectPath, int mode)
{
    int fd = -2;
    QEventLoop loop;
    auto conn = QObject::connect(&udisks, &UDisks::deviceOpened, [&](const QString &path, int f) {
        if (path == objectPath) {
            fd = f;
            loop.quit();
        }
    });
    udisks.openBlock(objectPath, UDisks::OpenMode(mode));
    if (fd == -2)
        loop.exec();
    QObject::disconnect(conn);
    return fd;
}
