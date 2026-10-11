// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "updates.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QVersionNumber>

namespace {

QString stripV(QString version)
{
    if (version.startsWith(QLatin1Char('v')))
        version.remove(0, 1);
    return version;
}

} // namespace

bool isNewerVersion(const QString &latest, const QString &current)
{
    return QVersionNumber::fromString(stripV(latest)) > QVersionNumber::fromString(stripV(current));
}

QString parseLatestRelease(const QByteArray &json, QString *error)
{
    const QString tag = QJsonDocument::fromJson(json).object().value(QStringLiteral("tag_name")).toString();
    static const QRegularExpression version(QStringLiteral("^v?\\d{1,4}(\\.\\d{1,4}){1,3}$"));
    if (!version.match(tag).hasMatch()) {
        *error = QObject::tr("GitHub sent an unexpected reply");
        return {};
    }
    return stripV(tag);
}

QString releasePageUrl(const QString &version)
{
    return QStringLiteral(APP_HOMEPAGE "/releases/tag/v") + version;
}

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
{
}

void UpdateChecker::check()
{
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/DannyS124/diskforge/releases/latest")));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "diskforge/" APP_VERSION); // GitHub rejects requests without one
    request.setTransferTimeout(15000);

    QNetworkReply *reply = m_network.get(request);
    // The answer is a few kilobytes; anything far bigger isn't from GitHub's API.
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64) {
        if (received > 1024 * 1024)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::ContentNotFoundError) {
            emit finished({}, {}, tr("There's no release on GitHub right now."));
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            emit finished({}, {}, reply->errorString());
            return;
        }
        QString error;
        const QString version = parseLatestRelease(reply->readAll(), &error);
        if (version.isEmpty()) {
            emit finished({}, {}, error);
            return;
        }
        emit finished(version, releasePageUrl(version), {});
    });
}
