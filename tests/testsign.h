// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Boot programs signed the way Windows' are, by a throwaway certificate authority made with
// OpenSSL, and UEFI variables made up to go with them: for the boot tests and window tests.

#include <QByteArray>
#include <QString>
#include <QVector>

#include <openssl/evp.h>
#include <openssl/x509.h>

namespace testsign {

struct Signer {
    EVP_PKEY *key = nullptr;
    X509 *cert = nullptr;
};

Signer makeCertificate(const char *name, const Signer *issuer, bool authority);
void release(const Signer &signer);
QByteArray derOf(X509 *cert);
// The smallest program UEFI would look at: headers and one section of `code`.
QByteArray makeProgram(const QByteArray &code);
// Its Authenticode hash, worked out on its own (before it's signed).
QByteArray programHash(const QByteArray &pe);
// Signs it: PKCS #7 SignedData around an SpcIndirectDataContent, carrying `carried`.
QByteArray sign(QByteArray pe, const Signer &signer, const QVector<X509 *> &carried);

// UEFI variables: EFI_SIGNATURE_LISTs, a boot entry, and a variable file the way efivarfs has them.
QByteArray signatureList(const char *type, const QVector<QByteArray> &items);
QByteArray loadOption(const QString &description, const QString &partition, const QString &path);
void putVariable(const QString &dir, const QString &name, const char *guid, const QByteArray &data);
inline constexpr char kX509List[] = "a5c059a1-94e4-4aa7-87b5-ab155c2bf072";
inline constexpr char kSha256List[] = "c1c41626-504c-4092-aca9-41f936934328";

} // namespace testsign
