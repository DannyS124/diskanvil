// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "netboot.h"

#include "rescuestick.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>

#include <memory>

namespace netboot {

namespace {

QString helper()
{
    return helperForTest.isEmpty() ? QStringLiteral("/usr/local/bin/diskanvil-live-netboot") : helperForTest;
}

constexpr qint64 kChunk = 1024 * 1024;
constexpr qint64 kAhead = 4 * kChunk; // what's queued for a PC at once

} // namespace

QVector<Card> cards()
{
    QVector<Card> wired, wireless;
    for (const QNetworkInterface &nic : QNetworkInterface::allInterfaces()) {
        const auto flags = nic.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) || (flags & QNetworkInterface::IsLoopBack)
            || nic.type() == QNetworkInterface::Virtual)
            continue;
        for (const QNetworkAddressEntry &entry : nic.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            Card card{nic.name(), entry.ip().toString(), nic.type() == QNetworkInterface::Wifi || QFileInfo::exists(QStringLiteral("/sys/class/net/%1/wireless").arg(nic.name()))};
            (card.wireless ? wireless : wired) << card;
            break;
        }
    }
    return wired + wireless;
}

QString medium()
{
    const QString m = mediumForTest.isEmpty() ? QStringLiteral("/run/live/medium") : mediumForTest;
    return QFileInfo(m + QStringLiteral("/live/filesystem.squashfs")).isFile() && QFileInfo(m + QStringLiteral("/boot/pxelinux/pxelinux.0")).isFile() ? m : QString();
}

bool available()
{
    if (!helperForTest.isEmpty())
        return true;
    return rescue::runningInRescue() && QFileInfo(helper()).isExecutable() && !QStandardPaths::findExecutable(QStringLiteral("dnsmasq"), {QStringLiteral("/usr/sbin"), QStringLiteral("/sbin"), QStringLiteral("/usr/bin")}).isEmpty();
}

QStringList command(const QString &card, quint16 port)
{
    const QStringList args = {helper(), QStringLiteral("start"), card, QString::number(port)};
    return helperForTest.isEmpty() ? QStringList{QStringLiteral("sudo"), QStringLiteral("-n")} + args : args;
}

QString askedBy(const QString &line)
{
    static const QRegularExpression pxe(QStringLiteral("PXE\\(\\S+\\) ([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}) "));
    return pxe.match(line).captured(1).toLower();
}

QString sentTo(const QString &line, QString *file)
{
    static const QRegularExpression sent(QStringLiteral("sent (\\S+) to ([0-9.]+)"));
    const QRegularExpressionMatch m = sent.match(line);
    if (file)
        *file = m.captured(1).section(QLatin1Char('/'), -1);
    return m.captured(2);
}

FileServer::FileServer(const QString &root, QObject *parent)
    : QObject(parent)
    , m_root(root)
    , m_server(new QTcpServer(this))
{
    connect(m_server, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket *socket = m_server->nextPendingConnection())
            serve(socket);
    });
}

bool FileServer::listen(quint16 first, quint16 last)
{
    for (quint32 port = first; port <= last; ++port) {
        if (m_server->listen(QHostAddress::AnyIPv4, quint16(port)))
            return true;
    }
    return false;
}

quint16 FileServer::port() const
{
    return m_server->serverPort();
}

bool FileServer::allowed(const QString &path)
{
    static const QRegularExpression ok(QStringLiteral("^/live/[A-Za-z0-9_-][A-Za-z0-9._-]*$"));
    return ok.match(path).hasMatch() && !path.contains(QLatin1String(".."));
}

void FileServer::serve(QTcpSocket *socket)
{
    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    auto request = std::make_shared<QByteArray>();
    auto file = std::make_shared<QFile>();
    auto sent = std::make_shared<qint64>(0);
    const QString client = socket->peerAddress().toString().remove(QStringLiteral("::ffff:"));
    const auto reply = [socket](const QByteArray &status) {
        socket->write("HTTP/1.1 " + status + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        socket->disconnectFromHost();
    };
    const auto more = [this, socket, file, sent, client] {
        if (!file->isOpen())
            return;
        while (socket->bytesToWrite() < kAhead && !file->atEnd()) {
            const QByteArray chunk = file->read(kChunk);
            if (chunk.isEmpty())
                break;
            socket->write(chunk);
            *sent += chunk.size();
        }
        Q_EMIT progress(client, quint64(*sent), quint64(file->size()));
        if (file->atEnd() && socket->bytesToWrite() == 0) {
            file->close();
            Q_EMIT finished(client, true);
            socket->disconnectFromHost();
        }
    };
    connect(socket, &QTcpSocket::bytesWritten, this, more);
    connect(socket, &QTcpSocket::disconnected, this, [this, file, client] {
        if (file->isOpen()) {
            file->close();
            Q_EMIT finished(client, false);
        }
    });
    connect(socket, &QTcpSocket::readyRead, this, [this, socket, request, file, reply, more] {
        if (file->isOpen())
            return;
        *request += socket->readAll();
        if (request->size() > 16384)
            return reply("431 Request Header Fields Too Large");
        const qsizetype end = request->indexOf("\r\n\r\n");
        if (end < 0)
            return;
        const QList<QByteArray> words = request->left(request->indexOf("\r\n")).split(' ');
        if (words.size() != 3 || !words[2].startsWith("HTTP/1."))
            return reply("400 Bad Request");
        if (words[0] != "GET" && words[0] != "HEAD")
            return reply("405 Method Not Allowed");
        const QString path = QString::fromLatin1(words[1]);
        file->setFileName(m_root + path);
        if (!allowed(path) || !QFileInfo(file->fileName()).isFile() || !file->open(QIODevice::ReadOnly))
            return reply("404 Not Found");
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " + QByteArray::number(file->size())
                      + "\r\nConnection: close\r\n\r\n");
        if (words[0] == "HEAD") {
            file->close();
            socket->disconnectFromHost();
            return;
        }
        more();
    });
}

} // namespace netboot
