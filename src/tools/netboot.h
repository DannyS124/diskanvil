// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Start Other PCs from This One, on DiskAnvil Live: other PCs on the same network start DiskAnvil
// Live from this one, with nothing installed on them. dnsmasq (diskanvil-live-netboot, as root)
// answers their network start and hands over the boot files; the system itself (the big
// filesystem.squashfs) comes from FileServer, a small HTTP server of our own here.

#include <QObject>
#include <QString>
#include <QVector>

class QTcpServer;
class QTcpSocket;

namespace netboot {

struct Card {
    QString name;    // "enp3s0"
    QString address; // "192.168.1.23"
    bool wireless = false;
};
QVector<Card> cards(); // up, with an IPv4 address; wired first
bool available();      // DiskAnvil Live, with the helper and dnsmasq
QString medium();      // the stick (or CD) DiskAnvil Live started from, when its files are there
QStringList command(const QString &card, quint16 port);

// From a line of dnsmasq's messages: a PC asking to start ("PXE(eth0) 52:54:00:12:34:56 proxy"),
// or a file it was sent ("sent /…/core.0 to 192.168.1.50"). Empty when it's neither.
QString askedBy(const QString &line);
QString sentTo(const QString &line, QString *file = nullptr);

// Files under its folder's live/ over HTTP (GET), nothing else.
class FileServer : public QObject
{
    Q_OBJECT
public:
    explicit FileServer(const QString &root, QObject *parent = nullptr);
    bool listen(quint16 first = 8099, quint16 last = 8199);
    quint16 port() const;
    static bool allowed(const QString &path); // "/live/filesystem.squashfs", not "/live/../x"

signals:
    void progress(const QString &client, quint64 done, quint64 total);
    void finished(const QString &client, bool whole);

private:
    void serve(QTcpSocket *socket);

    QString m_root;
    QTcpServer *m_server;
};

// The tests use stand-ins.
inline QString helperForTest, mediumForTest;

} // namespace netboot
