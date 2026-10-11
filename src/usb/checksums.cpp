// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "checksums.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("checksums", text);
}

bool isHex(const QByteArray &text)
{
    return !text.isEmpty() && std::all_of(text.cbegin(), text.cend(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

// By length: 32 hex digits is MD5, 40 SHA-1, 64 SHA-256, 128 SHA-512.
checksums::Expected fromHex(QByteArray hex)
{
    checksums::Expected e;
    hex = hex.toLower();
    if (!isHex(hex)) {
        e.error = tr("That's not a checksum: only the digits 0-9 and letters a-f go in one.");
        return e;
    }
    switch (hex.size()) {
    case 32: e.algorithm = QCryptographicHash::Md5; break;
    case 40: e.algorithm = QCryptographicHash::Sha1; break;
    case 64: e.algorithm = QCryptographicHash::Sha256; break;
    case 128: e.algorithm = QCryptographicHash::Sha512; break;
    default:
        e.error = tr("That's not a checksum DiskForge knows: it takes MD5, SHA-1, SHA-256 or SHA-512.");
        return e;
    }
    e.hex = hex;
    return e;
}

} // namespace

QList<QCryptographicHash::Algorithm> checksums::algorithms()
{
    return {QCryptographicHash::Md5, QCryptographicHash::Sha1, QCryptographicHash::Sha256, QCryptographicHash::Sha512};
}

QString checksums::name(QCryptographicHash::Algorithm algorithm)
{
    switch (algorithm) {
    case QCryptographicHash::Md5: return QStringLiteral("MD5");
    case QCryptographicHash::Sha1: return QStringLiteral("SHA-1");
    case QCryptographicHash::Sha512: return QStringLiteral("SHA-512");
    default: return QStringLiteral("SHA-256");
    }
}

checksums::Expected checksums::parse(const QString &text)
{
    QString t = text.trimmed();
    if (t.isEmpty())
        return {};
    // "sha256:abc...", "SHA256 abc..." or "abc...  file.iso": the hex digits are the longest word.
    static const QRegularExpression words(QStringLiteral("[\\s:=()*]+"));
    QString best;
    for (const QString &w : t.split(words, Qt::SkipEmptyParts)) {
        if (w.size() > best.size() && isHex(w.toLower().toLatin1()))
            best = w;
    }
    if (best.isEmpty())
        return fromHex(t.toLatin1()); // says what's wrong with it
    return fromHex(best.toLatin1());
}

checksums::Expected checksums::fromFile(const QByteArray &contents, const QString &fileName)
{
    // GNU:  <hex>  file.iso   or  <hex> *file.iso
    // BSD:  SHA256 (file.iso) = <hex>
    static const QRegularExpression gnu(QStringLiteral("^([0-9a-fA-F]{32,128})\\s+\\*?(.+)$"));
    static const QRegularExpression bsd(QStringLiteral("^(MD5|SHA1|SHA256|SHA512)\\s*\\((.+)\\)\\s*=\\s*([0-9a-fA-F]{32,128})$"),
                                        QRegularExpression::CaseInsensitiveOption);
    const QString wanted = QFileInfo(fileName).fileName();
    QList<QPair<QString, QByteArray>> lines; // file name, hex
    for (const QByteArray &raw : contents.split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        QRegularExpressionMatch m = gnu.match(line);
        if (m.hasMatch()) {
            lines.append({QFileInfo(m.captured(2).trimmed()).fileName(), m.captured(1).toLatin1()});
            continue;
        }
        m = bsd.match(line);
        if (m.hasMatch())
            lines.append({QFileInfo(m.captured(2).trimmed()).fileName(), m.captured(3).toLatin1()});
    }
    for (const auto &l : std::as_const(lines)) {
        if (l.first == wanted)
            return fromHex(l.second);
    }
    if (lines.size() == 1)
        return fromHex(lines.first().second);
    Expected e;
    e.error = lines.isEmpty() ? tr("There are no checksums in that file.") : tr("The checksum file doesn't list %1.").arg(wanted);
    return e;
}

checksums::Hasher::Hasher(const QString &path, const QList<QCryptographicHash::Algorithm> &which)
    : m_path(path)
    , m_which(which)
{
}

void checksums::Hasher::run()
{
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        emit finished(false, {}, file.errorString());
        return;
    }
    QList<QCryptographicHash *> hashes;
    for (QCryptographicHash::Algorithm a : std::as_const(m_which))
        hashes.append(new QCryptographicHash(a));
    const quint64 total = quint64(file.size());
    quint64 done = 0;
    QByteArray buffer(4 * 1024 * 1024, Qt::Uninitialized);
    bool ok = true;
    QString error;
    for (;;) {
        if (m_cancel) {
            ok = false;
            error = tr("Stopped.");
            break;
        }
        const qint64 n = file.read(buffer.data(), buffer.size());
        if (n < 0) {
            ok = false;
            error = file.errorString();
            break;
        }
        if (n == 0)
            break;
        for (QCryptographicHash *h : std::as_const(hashes))
            h->addData(QByteArrayView(buffer.constData(), n));
        done += quint64(n);
        emit progress(done, total);
    }
    QStringList hex;
    for (QCryptographicHash *h : std::as_const(hashes)) {
        if (ok)
            hex.append(QString::fromLatin1(h->result().toHex()));
        delete h;
    }
    emit finished(ok, hex, error);
}
