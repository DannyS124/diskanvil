// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Who signed a Windows program or UEFI boot file (Authenticode), and whether that's someone a
// given set of certificates vouches for: the PC's own Secure Boot list (db), for boot files.
// Our own code on OpenSSL's PKCS #7 support (osslsigncode isn't packaged everywhere).
//
// Authenticode in short: the file's hash, leaving out the checksum, the pointer to the
// signature and the signature itself, is kept in a PKCS #7 SignedData whose content is
// Microsoft's SpcIndirectDataContent. The signature covers that content without its outer
// SEQUENCE header, which is the one thing OpenSSL needs to be told.

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace authenticode {

struct Result {
    enum class State {
        NotProgram,   // not a Windows/UEFI program at all
        Unsigned,
        Unreadable,   // there's a signature, but it can't be read
        Changed,      // signed, but the file was changed after it was signed
        BadSignature, // the signature itself doesn't hold up
        Signed,
    };
    State state = State::NotProgram;
    QString signer; // the signing certificate's name: "Microsoft Windows"
    QString issuer; // what issued it: "Microsoft Windows Production PCA 2011"
    QVector<QByteArray> certificates; // the signer and the certificates above it in the signature (DER)
    QByteArray sha256;                // the file's Authenticode hash, as Secure Boot's lists have them
    bool trusted = false;             // the signer goes back to one of the trusted certificates
    QString trustedBy;
};

// `trusted`: certificates (DER) trusted as they are, the way UEFI's db works: any of them can
// end the chain, and dates don't matter.
Result check(const QByteArray &file, const QVector<QByteArray> &trusted);

// A certificate's name (its common name, else the whole subject).
QString certificateName(const QByteArray &der);

} // namespace authenticode
