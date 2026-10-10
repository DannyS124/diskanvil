// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "authenticode.h"

#include <QtEndian>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/pkcs7.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include <cstring>
#include <memory>

namespace authenticode {

namespace {

constexpr char kIndirectData[] = "1.3.6.1.4.1.311.2.1.4"; // SPC_INDIRECT_DATA_OBJID
constexpr quint16 kSignedData = 0x0002;                   // WIN_CERT_TYPE_PKCS_SIGNED_DATA

template <typename T, void (*Free)(T *)>
struct Deleter {
    void operator()(T *p) const { Free(p); }
};
using Pkcs7 = std::unique_ptr<PKCS7, Deleter<PKCS7, PKCS7_free>>;
using Bio = std::unique_ptr<BIO, Deleter<BIO, BIO_free_all>>;
using Store = std::unique_ptr<X509_STORE, Deleter<X509_STORE, X509_STORE_free>>;
using StoreCtx = std::unique_ptr<X509_STORE_CTX, Deleter<X509_STORE_CTX, X509_STORE_CTX_free>>;
using Cert = std::unique_ptr<X509, Deleter<X509, X509_free>>;
using MdCtx = std::unique_ptr<EVP_MD_CTX, Deleter<EVP_MD_CTX, EVP_MD_CTX_free>>;

void freeCerts(STACK_OF(X509) *s)
{
    sk_X509_free(s);
}
using Certs = std::unique_ptr<STACK_OF(X509), Deleter<STACK_OF(X509), freeCerts>>;

quint16 u16(const QByteArray &d, qsizetype at)
{
    return qFromLittleEndian<quint16>(d.constData() + at);
}

quint32 u32(const QByteArray &d, qsizetype at)
{
    return qFromLittleEndian<quint32>(d.constData() + at);
}

QString nameOf(X509_NAME *name)
{
    if (!name)
        return {};
    char text[256] = {};
    if (X509_NAME_get_text_by_NID(name, NID_commonName, text, sizeof text) > 0)
        return QString::fromUtf8(text);
    char *line = X509_NAME_oneline(name, nullptr, 0);
    const QString out = QString::fromUtf8(line);
    OPENSSL_free(line);
    return out;
}

QByteArray der(X509 *cert)
{
    unsigned char *out = nullptr;
    const int length = i2d_X509(cert, &out);
    const QByteArray bytes = length > 0 ? QByteArray(reinterpret_cast<const char *>(out), length) : QByteArray();
    OPENSSL_free(out);
    return bytes;
}

// DER, just enough of it: a tag and its length.
struct Tlv {
    int tag = -1;
    qsizetype header = 0; // tag and length bytes
    qsizetype length = 0;
};

Tlv readTlv(const unsigned char *p, qsizetype size)
{
    Tlv t;
    if (size < 2)
        return t;
    t.tag = p[0];
    qsizetype at = 1;
    const int first = p[at++];
    if (first < 0x80) {
        t.length = first;
    } else {
        const int bytes = first & 0x7f;
        if (bytes == 0 || bytes > 4 || at + bytes > size)
            return {};
        for (int i = 0; i < bytes; ++i)
            t.length = (t.length << 8) | p[at++];
    }
    t.header = at;
    if (t.length < 0 || t.header + t.length > size)
        return {};
    return t;
}

// The parts of the file the hash covers: all of it except the checksum, the pointer to the
// signature, and the signature at the end.
struct Layout {
    qsizetype checksum = 0;
    qsizetype certificateEntry = 0;
    qsizetype signatureAt = 0;
    qsizetype signatureSize = 0;
};

bool readLayout(const QByteArray &file, Layout *out)
{
    if (file.size() < 0x40 || !file.startsWith("MZ"))
        return false;
    const qsizetype pe = u32(file, 0x3C);
    if (pe < 0x40 || pe > file.size() - 24 || std::memcmp(file.constData() + pe, "PE\0\0", 4) != 0)
        return false;
    const qsizetype optional = pe + 24;
    const quint16 optionalSize = u16(file, pe + 20);
    if (optional + 2 > file.size())
        return false;
    const quint16 magic = u16(file, optional);
    const bool plus = magic == 0x20b;
    if (!plus && magic != 0x10b)
        return false;
    const qsizetype directories = optional + (plus ? 112 : 96);
    const qsizetype count = optional + (plus ? 108 : 92);
    if (directories + 5 * 8 > file.size() || directories + 5 * 8 > optional + optionalSize || u32(file, count) < 5)
        return false;
    out->checksum = optional + 64;
    out->certificateEntry = directories + 4 * 8;
    out->signatureAt = u32(file, out->certificateEntry);
    out->signatureSize = u32(file, out->certificateEntry + 4);
    if (out->signatureSize == 0 || out->signatureAt == 0) {
        out->signatureAt = file.size();
        out->signatureSize = 0;
    }
    return out->signatureAt <= file.size() && out->signatureSize <= file.size() - out->signatureAt && out->signatureAt >= out->certificateEntry + 8;
}

QByteArray fileHash(const QByteArray &file, const Layout &l, const EVP_MD *md)
{
    MdCtx ctx(EVP_MD_CTX_new());
    if (!ctx || !EVP_DigestInit_ex(ctx.get(), md, nullptr))
        return {};
    const char *p = file.constData();
    EVP_DigestUpdate(ctx.get(), p, size_t(l.checksum));
    EVP_DigestUpdate(ctx.get(), p + l.checksum + 4, size_t(l.certificateEntry - l.checksum - 4));
    EVP_DigestUpdate(ctx.get(), p + l.certificateEntry + 8, size_t(l.signatureAt - l.certificateEntry - 8));
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    EVP_DigestFinal_ex(ctx.get(), digest, &length);
    return QByteArray(reinterpret_cast<const char *>(digest), int(length));
}

// The signature: the first WIN_CERTIFICATE holding a PKCS #7 SignedData.
QByteArray signatureBlob(const QByteArray &file, const Layout &l)
{
    for (qsizetype at = l.signatureAt; at + 8 <= l.signatureAt + l.signatureSize;) {
        const quint32 length = u32(file, at);
        const quint16 type = u16(file, at + 6);
        if (length < 8 || at + qsizetype(length) > l.signatureAt + l.signatureSize)
            break;
        if (type == kSignedData)
            return file.mid(at + 8, qsizetype(length) - 8);
        at += (qsizetype(length) + 7) / 8 * 8;
    }
    return {};
}

} // namespace

QString certificateName(const QByteArray &bytes)
{
    const unsigned char *p = reinterpret_cast<const unsigned char *>(bytes.constData());
    Cert cert(d2i_X509(nullptr, &p, long(bytes.size())));
    return cert ? nameOf(X509_get_subject_name(cert.get())) : QString();
}

Result check(const QByteArray &file, const QVector<QByteArray> &trusted)
{
    Result r;
    Layout layout;
    if (!readLayout(file, &layout))
        return r;
    r.sha256 = fileHash(file, layout, EVP_sha256());
    if (layout.signatureSize == 0) {
        r.state = Result::State::Unsigned;
        return r;
    }
    r.state = Result::State::Unreadable;
    const QByteArray blob = signatureBlob(file, layout);
    const unsigned char *p = reinterpret_cast<const unsigned char *>(blob.constData());
    Pkcs7 p7(d2i_PKCS7(nullptr, &p, long(blob.size())));
    ERR_clear_error();
    if (!p7 || !PKCS7_type_is_signed(p7.get()) || !p7->d.sign || !p7->d.sign->contents)
        return r;
    PKCS7 *content = p7->d.sign->contents;
    char oid[64] = {};
    OBJ_obj2txt(oid, sizeof oid, content->type, 1);
    if (std::strcmp(oid, kIndirectData) != 0 || !content->d.other || content->d.other->type != V_ASN1_SEQUENCE || !content->d.other->value.sequence)
        return r;
    // SpcIndirectDataContent ::= SEQUENCE { data, messageDigest DigestInfo }
    const ASN1_STRING *sequence = content->d.other->value.sequence;
    const unsigned char *spc = ASN1_STRING_get0_data(sequence);
    const qsizetype spcSize = ASN1_STRING_length(sequence);
    const Tlv outer = readTlv(spc, spcSize);
    if (outer.tag != 0x30)
        return r;
    const unsigned char *inner = spc + outer.header;
    const Tlv data = readTlv(inner, outer.length);
    if (data.tag != 0x30)
        return r;
    const unsigned char *digestInfo = inner + data.header + data.length;
    const Tlv info = readTlv(digestInfo, outer.length - data.header - data.length);
    if (info.tag != 0x30)
        return r;
    const Tlv algorithm = readTlv(digestInfo + info.header, info.length);
    if (algorithm.tag != 0x30)
        return r;
    const unsigned char *algorithmBody = digestInfo + info.header + algorithm.header;
    const Tlv algorithmOid = readTlv(algorithmBody, algorithm.length);
    const unsigned char *digestAt = digestInfo + info.header + algorithm.header + algorithm.length;
    const Tlv digest = readTlv(digestAt, info.length - algorithm.header - algorithm.length);
    if (algorithmOid.tag != 0x06 || digest.tag != 0x04)
        return r;
    const unsigned char *oidBytes = algorithmBody;
    std::unique_ptr<ASN1_OBJECT, Deleter<ASN1_OBJECT, ASN1_OBJECT_free>> object(
        d2i_ASN1_OBJECT(nullptr, &oidBytes, long(algorithmOid.header + algorithmOid.length)));
    const EVP_MD *md = object ? EVP_get_digestbyobj(object.get()) : nullptr;
    if (!md)
        return r;

    // The signer and what's above it, as far as the signature carries them.
    STACK_OF(X509) *included = p7->d.sign->cert;
    Certs signers(PKCS7_get0_signers(p7.get(), nullptr, 0));
    ERR_clear_error();
    X509 *signer = signers && sk_X509_num(signers.get()) > 0 ? sk_X509_value(signers.get(), 0) : nullptr;
    if (!signer)
        return r;
    r.signer = nameOf(X509_get_subject_name(signer));
    r.issuer = nameOf(X509_get_issuer_name(signer));
    X509 *at = signer;
    for (int depth = 0; at && depth < 6; ++depth) {
        r.certificates << der(at);
        X509 *next = nullptr;
        for (int i = 0; included && i < sk_X509_num(included); ++i) {
            X509 *c = sk_X509_value(included, i);
            if (c != at && X509_check_issued(c, at) == X509_V_OK) {
                next = c;
                break;
            }
        }
        at = next == at ? nullptr : next;
    }

    // The file as it is now against the hash that was signed.
    const QByteArray now = fileHash(file, layout, md);
    if (now.size() != digest.length || std::memcmp(now.constData(), digestAt + digest.header, size_t(digest.length)) != 0) {
        r.state = Result::State::Changed;
        return r;
    }
    // The signature over that, on the content without its own SEQUENCE header.
    Bio signedPart(BIO_new_mem_buf(inner, int(outer.length)));
    Store none(X509_STORE_new());
    if (PKCS7_verify(p7.get(), nullptr, none.get(), signedPart.get(), nullptr, PKCS7_NOVERIFY | PKCS7_BINARY) != 1) {
        ERR_clear_error();
        r.state = Result::State::BadSignature;
        return r;
    }
    r.state = Result::State::Signed;

    // Trusted: a chain from the signer to one of `trusted`, through what the signature carries.
    if (!trusted.isEmpty()) {
        Store store(X509_STORE_new());
        for (const QByteArray &bytes : trusted) {
            const unsigned char *q = reinterpret_cast<const unsigned char *>(bytes.constData());
            Cert c(d2i_X509(nullptr, &q, long(bytes.size())));
            if (c)
                X509_STORE_add_cert(store.get(), c.get()); // the store keeps its own reference
        }
        ERR_clear_error();
        X509_STORE_set_flags(store.get(), X509_V_FLAG_PARTIAL_CHAIN | X509_V_FLAG_NO_CHECK_TIME);
        StoreCtx ctx(X509_STORE_CTX_new());
        if (ctx && X509_STORE_CTX_init(ctx.get(), store.get(), signer, included) == 1) {
            if (X509_verify_cert(ctx.get()) == 1) {
                r.trusted = true;
                STACK_OF(X509) *chain = X509_STORE_CTX_get0_chain(ctx.get());
                if (chain && sk_X509_num(chain) > 0)
                    r.trustedBy = nameOf(X509_get_subject_name(sk_X509_value(chain, sk_X509_num(chain) - 1)));
            }
        }
        ERR_clear_error();
    }
    return r;
}

} // namespace authenticode
