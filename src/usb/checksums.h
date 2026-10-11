// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QCryptographicHash>
#include <QObject>
#include <QStringList>

#include <atomic>

// Checksums for Write Image to USB: what the download page gives, pasted (MD5, SHA-1, SHA-256
// or SHA-512, told apart by their length) or as a checksum file (SHA256SUMS, *.sha256, or the
// BSD style "SHA256 (file) = ..."). It's checked against the file as it was downloaded, so it
// works for compressed images too.
namespace checksums {

struct Expected {
    QCryptographicHash::Algorithm algorithm = QCryptographicHash::Sha256;
    QByteArray hex; // lower case; empty: nothing to check
    QString error;  // what's wrong with what was given
    bool isSet() const { return !hex.isEmpty(); }
};

// "SHA-256"
QString name(QCryptographicHash::Algorithm algorithm);
// A pasted checksum. A "sha256:" in front, spaces, or the file name after it are fine.
Expected parse(const QString &text);
// The line for `fileName` in a checksum file (or its only line).
Expected fromFile(const QByteArray &contents, const QString &fileName);

QList<QCryptographicHash::Algorithm> algorithms(); // MD5, SHA-1, SHA-256, SHA-512

// Checksums of a file, all in one pass: all four, or just the ones asked for. Runs in a worker
// thread.
class Hasher : public QObject
{
    Q_OBJECT
public:
    explicit Hasher(const QString &path, const QList<QCryptographicHash::Algorithm> &which = algorithms());
    void cancel() { m_cancel = true; }

public slots:
    void run();

signals:
    void progress(quint64 done, quint64 total);
    // In the order they were asked for.
    void finished(bool ok, const QStringList &hex, const QString &error);

private:
    QString m_path;
    QList<QCryptographicHash::Algorithm> m_which;
    std::atomic<bool> m_cancel{false};
};

} // namespace checksums
