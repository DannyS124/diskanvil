// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "signature.h"

#include <QCoreApplication>
#include <QCryptographicHash>

#include <openssl/evp.h>

#include <memory>

// The format is OpenSSH's PROTOCOL.sshsig: a "SSHSIG" blob holding the public key, the
// namespace, the hash algorithm and the signature, over a second blob that holds the
// namespace, the hash algorithm and the hash of the data.

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("signature", text);
}

// SSH's wire format: big-endian 32-bit lengths in front of strings.
class Reader
{
public:
    explicit Reader(const QByteArray &data)
        : m_data(data)
    {
    }

    bool bytes(qsizetype n, QByteArray *out)
    {
        if (n < 0 || m_data.size() - m_pos < n)
            return false;
        *out = m_data.mid(m_pos, n);
        m_pos += n;
        return true;
    }
    bool uint32(quint32 *out)
    {
        QByteArray b;
        if (!bytes(4, &b))
            return false;
        const auto *p = reinterpret_cast<const uchar *>(b.constData());
        *out = quint32(p[0]) << 24 | quint32(p[1]) << 16 | quint32(p[2]) << 8 | quint32(p[3]);
        return true;
    }
    bool string(QByteArray *out)
    {
        quint32 n = 0;
        return uint32(&n) && n <= quint32(m_data.size() - m_pos) && bytes(qsizetype(n), out);
    }
    bool atEnd() const { return m_pos == m_data.size(); }

private:
    const QByteArray m_data;
    qsizetype m_pos = 0;
};

QByteArray sshString(const QByteArray &s)
{
    const auto n = quint32(s.size());
    const char length[4] = {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
    return QByteArray(length, 4) + s;
}

QByteArray unarmor(const QByteArray &armored)
{
    static const QByteArray begin = "-----BEGIN SSH SIGNATURE-----";
    static const QByteArray end = "-----END SSH SIGNATURE-----";
    const QByteArray text = armored.trimmed();
    if (!text.startsWith(begin) || !text.endsWith(end) || text.size() < begin.size() + end.size())
        return {};
    QByteArray body = text.mid(begin.size(), text.size() - begin.size() - end.size());
    body.replace('\n', QByteArray()).replace('\r', QByteArray()).replace(' ', QByteArray());
    const auto decoded = QByteArray::fromBase64Encoding(body, QByteArray::AbortOnBase64DecodingErrors);
    return decoded ? *decoded : QByteArray();
}

// "ssh-ed25519 AAAA... comment" -> the key in wire format, as it appears in a signature.
QByteArray keyBlob(const QString &line)
{
    const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() < 2 || parts[0] != QLatin1String("ssh-ed25519"))
        return {};
    const auto decoded = QByteArray::fromBase64Encoding(parts[1].toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    return decoded ? *decoded : QByteArray();
}

bool verifyEd25519(const QByteArray &key, const QByteArray &sig, const QByteArray &message)
{
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pkey(
        EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char *>(key.constData()), size_t(key.size())),
        EVP_PKEY_free);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    return pkey && ctx && EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, pkey.get()) == 1
        && EVP_DigestVerify(ctx.get(), reinterpret_cast<const unsigned char *>(sig.constData()), size_t(sig.size()),
                            reinterpret_cast<const unsigned char *>(message.constData()), size_t(message.size())) == 1;
}

} // namespace

bool signature::verify(const QByteArray &data, const QByteArray &armored, const QString &ns, const QStringList &keys, QString *error)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    const QByteArray blob = unarmor(armored);
    Reader r(blob);
    QByteArray magic, publicKey, sigNamespace, reserved, hashName, sigBlob;
    quint32 version = 0;
    if (blob.isEmpty() || !r.bytes(6, &magic) || magic != "SSHSIG" || !r.uint32(&version) || version != 1 || !r.string(&publicKey)
        || !r.string(&sigNamespace) || !r.string(&reserved) || !r.string(&hashName) || !r.string(&sigBlob) || !r.atEnd())
        return fail(tr("the signature isn't in the right format"));
    if (sigNamespace != ns.toUtf8())
        return fail(tr("the signature is for something else"));

    bool known = false;
    for (const QString &line : keys)
        known = known || (!publicKey.isEmpty() && keyBlob(line) == publicKey);
    if (!known)
        return fail(tr("it's signed with a key DiskForge doesn't know"));

    Reader k(publicKey);
    QByteArray keyType, key;
    if (!k.string(&keyType) || keyType != "ssh-ed25519" || !k.string(&key) || key.size() != 32 || !k.atEnd())
        return fail(tr("the signature isn't in the right format"));
    QCryptographicHash::Algorithm algorithm;
    if (hashName == "sha512")
        algorithm = QCryptographicHash::Sha512;
    else if (hashName == "sha256")
        algorithm = QCryptographicHash::Sha256;
    else
        return fail(tr("the signature uses a hash DiskForge doesn't know"));
    Reader s(sigBlob);
    QByteArray sigType, sig;
    if (!s.string(&sigType) || sigType != "ssh-ed25519" || !s.string(&sig) || sig.size() != 64 || !s.atEnd())
        return fail(tr("the signature isn't in the right format"));

    const QByteArray signedData = QByteArray("SSHSIG") + sshString(sigNamespace) + sshString(reserved) + sshString(hashName)
        + sshString(QCryptographicHash::hash(data, algorithm));
    if (!verifyEd25519(key, sig, signedData))
        return fail(tr("the signature doesn't match"));
    return true;
}
