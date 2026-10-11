// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QNetworkAccessManager>
#include <QObject>

// Asks GitHub for the latest release. Only runs when the user asks; there is no
// background network access.
class UpdateChecker : public QObject
{
    Q_OBJECT
public:
    explicit UpdateChecker(QObject *parent = nullptr);
    void check();

signals:
    // On failure, error is set and the other fields are empty.
    void finished(const QString &latestVersion, const QString &releaseUrl, const QString &error);

private:
    QNetworkAccessManager m_network;
};

// True if `latest` (e.g. "v0.5.0") is newer than `current` ("0.4.0").
bool isNewerVersion(const QString &latest, const QString &current);

// Reads GitHub's answer about the latest release and returns its version ("0.5.0"), or
// sets error. Only a plain version number is accepted.
QString parseLatestRelease(const QByteArray &json, QString *error);
// The release page for a version. Built here rather than taken from GitHub's answer.
QString releasePageUrl(const QString &version);
