// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hashlookup.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace hashlookup {

QString server = QStringLiteral("https://hashlookup.circl.lu");

QString describe(const QByteArray &json)
{
    // {"FileName": ..., "ProductCode": {"ProductName": ..., "ProductVersion": ...}, "source": "NSRL", ...}
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    const QString file = o.value(QLatin1String("FileName")).toString();
    const QJsonObject product = o.value(QLatin1String("ProductCode")).toObject();
    QString from = product.value(QLatin1String("ProductName")).toString();
    if (from.isEmpty())
        from = o.value(QLatin1String("PackageName")).toString();
    const QString version = product.value(QLatin1String("ProductVersion")).toString(o.value(QLatin1String("PackageVersion")).toString());
    if (!from.isEmpty() && !version.isEmpty())
        from += QLatin1Char(' ') + version;
    const QString source = o.value(QLatin1String("source")).toString();
    QString words = file.isEmpty() ? QCoreApplication::translate("hashlookup", "a known file") : file;
    if (!from.isEmpty())
        words = QCoreApplication::translate("hashlookup", "%1, part of %2").arg(words, from);
    if (!source.isEmpty())
        words += QStringLiteral(" (%1)").arg(source);
    return words;
}

Lookup::Lookup(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<hashlookup::Answer>();
}

void Lookup::ask(const QStringList &sha256)
{
    m_waiting = int(sha256.size());
    if (m_waiting == 0) {
        Q_EMIT finished();
        return;
    }
    for (const QString &hash : sha256) {
        QNetworkRequest request{QUrl(server + QStringLiteral("/lookup/sha256/") + hash)};
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("DiskForge/" APP_VERSION));
        request.setRawHeader("Accept", "application/json");
        request.setTransferTimeout(20000);
        QNetworkReply *reply = m_network.get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply, hash] {
            reply->deleteLater();
            Answer a;
            a.sha256 = hash;
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 200) {
                a.known = true;
                a.what = describe(reply->readAll());
            } else if (status != 404) {
                a.failed = true; // no internet, or the service is busy: not "unknown"
                a.what = reply->errorString();
            }
            Q_EMIT answered(a);
            if (--m_waiting == 0)
                Q_EMIT finished();
        });
    }
}

} // namespace hashlookup
