// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "testsign.h"

#include <QCryptographicHash>
#include <QFile>
#include <QtEndian>

#include <openssl/objects.h>
#include <openssl/pkcs7.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cstring>

namespace testsign {

namespace {

void put16(char *p, quint16 v)
{
    qToLittleEndian(v, p);
}

void put32(char *p, quint32 v)
{
    qToLittleEndian(v, p);
}

// The smallest program UEFI would look at: headers and one section of `code`.

QByteArray tlv(uchar tag, const QByteArray &content)
{
    QByteArray out(1, char(tag));
    const qsizetype n = content.size();
    if (n < 0x80) {
        out += char(n);
    } else if (n < 0x100) {
        out += char(0x81);
        out += char(n);
    } else {
        out += char(0x82);
        out += char(n >> 8);
        out += char(n & 0xff);
    }
    return out + content;
}

QByteArray oid(const char *text)
{
    ASN1_OBJECT *o = OBJ_txt2obj(text, 1);
    unsigned char *out = nullptr;
    const int n = i2d_ASN1_OBJECT(o, &out);
    QByteArray bytes(reinterpret_cast<const char *>(out), n);
    OPENSSL_free(out);
    ASN1_OBJECT_free(o);
    return bytes;
}

QByteArray rawGuid(const QString &text)
{
    const QString hex = QString(text).remove(QLatin1Char('-'));
    QByteArray b = QByteArray::fromHex(hex.toLatin1());
    std::reverse(b.begin(), b.begin() + 4);
    std::reverse(b.begin() + 4, b.begin() + 6);
    std::reverse(b.begin() + 6, b.begin() + 8);
    return b;
}

QByteArray utf16(const QString &s)
{
    QByteArray out;
    for (QChar c : s) {
        out += char(c.unicode() & 0xff);
        out += char(c.unicode() >> 8);
    }
    return out + QByteArray(2, '\0');
}

} // namespace

QByteArray makeProgram(const QByteArray &code)
{
    QByteArray pe(0x400, '\0');
    char *p = pe.data();
    p[0] = 'M';
    p[1] = 'Z';
    put32(p + 0x3C, 0x40);
    std::memcpy(p + 0x40, "PE\0\0", 4);
    char *coff = p + 0x44;
    put16(coff, 0x8664);
    put16(coff + 2, 1);
    put16(coff + 16, 240);
    put16(coff + 18, 0x22);
    char *optional = coff + 20;
    put16(optional, 0x20b);
    put32(optional + 16, 0x1000);
    put32(optional + 56, 0x2000);
    put32(optional + 60, 0x200);
    put16(optional + 68, 10); // an EFI application
    put32(optional + 108, 16);
    char *section = optional + 240;
    std::memcpy(section, ".text", 5);
    put32(section + 8, 0x200);
    put32(section + 12, 0x1000);
    put32(section + 16, 0x200);
    put32(section + 20, 0x200);
    std::memcpy(p + 0x200, code.constData(), size_t(std::min<qsizetype>(code.size(), 0x200)));
    return pe;
}

// DER: a tag and its contents.

QByteArray derOf(X509 *cert)
{
    unsigned char *out = nullptr;
    const int n = i2d_X509(cert, &out);
    QByteArray bytes(reinterpret_cast<const char *>(out), n);
    OPENSSL_free(out);
    return bytes;
}

Signer makeCertificate(const char *name, const Signer *issuer, bool authority)
{
    static long serial = 1;
    Signer s;
    s.key = EVP_RSA_gen(2048);
    s.cert = X509_new();
    X509_set_version(s.cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(s.cert), serial++);
    X509_gmtime_adj(X509_getm_notBefore(s.cert), -3600);
    X509_gmtime_adj(X509_getm_notAfter(s.cert), 3600L * 24 * 365);
    X509_set_pubkey(s.cert, s.key);
    X509_NAME_add_entry_by_txt(X509_get_subject_name(s.cert), "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char *>(name), -1, -1, 0);
    X509_set_issuer_name(s.cert, issuer ? X509_get_subject_name(issuer->cert) : X509_get_subject_name(s.cert));
    if (authority) {
        X509V3_CTX ctx;
        X509V3_set_ctx_nodb(&ctx);
        X509V3_set_ctx(&ctx, issuer ? issuer->cert : s.cert, s.cert, nullptr, nullptr, 0);
        X509_EXTENSION *ext = X509V3_EXT_conf_nid(nullptr, &ctx, NID_basic_constraints, "critical,CA:TRUE");
        X509_add_ext(s.cert, ext, -1);
        X509_EXTENSION_free(ext);
    }
    X509_sign(s.cert, issuer ? issuer->key : s.key, EVP_sha256());
    return s;
}

// The Authenticode hash, worked out here on its own: all but the checksum and the pointer
// to the signature (the program has none yet).

QByteArray programHash(const QByteArray &pe)
{
    const qsizetype checksum = 0x58 + 64, entry = 0x58 + 112 + 32;
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(QByteArrayView(pe).first(checksum));
    h.addData(QByteArrayView(pe).sliced(checksum + 4, entry - checksum - 4));
    h.addData(QByteArrayView(pe).sliced(entry + 8));
    return h.result();
}

// Signs the way Windows' tools do: PKCS #7 SignedData around an SpcIndirectDataContent.

QByteArray sign(QByteArray pe, const Signer &signer, const QVector<X509 *> &carried)
{
    const QByteArray spcData = tlv(0x30, oid("1.3.6.1.4.1.311.2.1.15") + tlv(0xa0, tlv(0x30, tlv(0x03, QByteArray(1, '\0')))));
    const QByteArray digestInfo = tlv(0x30, tlv(0x30, oid("2.16.840.1.101.3.4.2.1") + tlv(0x05, {})) + tlv(0x04, programHash(pe)));
    const QByteArray spc = tlv(0x30, spcData + digestInfo);
    const qsizetype header = spc.size() - spcData.size() - digestInfo.size();

    PKCS7 *p7 = PKCS7_new();
    PKCS7_set_type(p7, NID_pkcs7_signed);
    PKCS7_SIGNER_INFO *si = PKCS7_add_signature(p7, signer.cert, signer.key, EVP_sha256());
    PKCS7_add_signed_attribute(si, NID_pkcs9_contentType, V_ASN1_OBJECT, OBJ_txt2obj("1.3.6.1.4.1.311.2.1.4", 1));
    for (X509 *c : carried)
        PKCS7_add_certificate(p7, c);
    // OpenSSL signs only "data" by itself: signed as that, then the content swapped for the
    // SpcIndirectDataContent it really is.
    PKCS7_content_new(p7, NID_pkcs7_data);
    BIO *bio = PKCS7_dataInit(p7, nullptr);
    BIO_write(bio, spc.constData() + header, int(spc.size() - header));
    PKCS7_dataFinal(p7, bio);
    BIO_free_all(bio);
    PKCS7 *inner = PKCS7_new();
    inner->type = OBJ_txt2obj("1.3.6.1.4.1.311.2.1.4", 1);
    ASN1_STRING *sequence = ASN1_STRING_new();
    ASN1_STRING_set(sequence, spc.constData(), int(spc.size()));
    ASN1_TYPE *other = ASN1_TYPE_new();
    ASN1_TYPE_set(other, V_ASN1_SEQUENCE, sequence);
    inner->d.other = other;
    PKCS7_set_content(p7, inner);
    unsigned char *out = nullptr;
    const int n = i2d_PKCS7(p7, &out);
    QByteArray blob(reinterpret_cast<const char *>(out), n);
    OPENSSL_free(out);
    PKCS7_free(p7);

    // WIN_CERTIFICATE: its length, revision 2.0, PKCS #7, the blob; padded to 8 bytes.
    QByteArray entry(8, '\0');
    put32(entry.data(), quint32(8 + blob.size()));
    put16(entry.data() + 4, 0x0200);
    put16(entry.data() + 6, 0x0002);
    entry += blob;
    entry.append((8 - entry.size() % 8) % 8, '\0');
    const qsizetype at = pe.size();
    put32(pe.data() + 0x58 + 112 + 32, quint32(at));
    put32(pe.data() + 0x58 + 112 + 36, quint32(entry.size()));
    return pe + entry;
}

QByteArray signatureList(const char *type, const QVector<QByteArray> &items)
{
    QByteArray out;
    for (const QByteArray &item : items) {
        QByteArray list = rawGuid(QLatin1String(type)) + QByteArray(12, '\0');
        const quint32 size = quint32(16 + item.size());
        put32(list.data() + 16, 28 + size);
        put32(list.data() + 20, 0);
        put32(list.data() + 24, size);
        list += QByteArray(16, '\0') + item;
        out += list;
    }
    return out;
}

QByteArray loadOption(const QString &description, const QString &partition, const QString &path)
{
    QByteArray disk(42, '\0');
    disk[0] = 4;
    disk[1] = 1;
    put16(disk.data() + 2, 42);
    put32(disk.data() + 4, 1);
    disk.replace(24, 16, rawGuid(partition));
    disk[40] = 2;
    disk[41] = 2;
    const QByteArray name = utf16(path);
    QByteArray file(4, '\0');
    file[0] = 4;
    file[1] = 4;
    put16(file.data() + 2, quint16(4 + name.size()));
    file += name;
    const QByteArray end("\x7f\xff\x04\x00", 4);
    const QByteArray devicePath = disk + file + end;
    QByteArray option(6, '\0');
    put32(option.data(), 1);
    put16(option.data() + 4, quint16(devicePath.size()));
    return option + utf16(description) + devicePath;
}

void release(const Signer &signer)
{
    X509_free(signer.cert);
    EVP_PKEY_free(signer.key);
}

void putVariable(const QString &dir, const QString &name, const char *guid, const QByteArray &data)
{
    QFile f(dir + QLatin1Char('/') + name + QLatin1Char('-') + QLatin1String(guid));
    if (f.open(QIODevice::WriteOnly))
        f.write(QByteArray("\x07\x00\x00\x00", 4) + data);
}

} // namespace testsign
