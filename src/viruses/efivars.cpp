// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "efivars.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QtEndian>

#ifdef Q_OS_WIN
#include "win/winutil.h"
#endif

namespace efi {

QString varsDir = QStringLiteral("/sys/firmware/efi/efivars");

namespace {

constexpr char kX509[] = "a5c059a1-94e4-4aa7-87b5-ab155c2bf072";   // EFI_CERT_X509_GUID
constexpr char kSha256[] = "c1c41626-504c-4092-aca9-41f936934328"; // EFI_CERT_SHA256_GUID

quint16 u16(const char *p)
{
    return qFromLittleEndian<quint16>(p);
}

quint32 u32(const char *p)
{
    return qFromLittleEndian<quint32>(p);
}

#ifdef Q_OS_WIN
// Reading firmware variables needs a privilege administrators have but don't hold switched on.
bool allowFirmwareAccess()
{
    static const bool allowed = [] {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
            return false;
        TOKEN_PRIVILEGES privileges = {};
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        const bool ok = LookupPrivilegeValueW(nullptr, SE_SYSTEM_ENVIRONMENT_NAME, &privileges.Privileges[0].Luid)
                     && AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
        CloseHandle(token);
        return ok;
    }();
    return allowed;
}
#endif

} // namespace

QString guidText(const char *raw)
{
    return QStringLiteral("%1-%2-%3-%4-%5")
        .arg(u32(raw), 8, 16, QLatin1Char('0'))
        .arg(u16(raw + 4), 4, 16, QLatin1Char('0'))
        .arg(u16(raw + 6), 4, 16, QLatin1Char('0'))
        .arg(QString::fromLatin1(QByteArray(raw + 8, 2).toHex()), QString::fromLatin1(QByteArray(raw + 10, 6).toHex()));
}

bool available()
{
#ifdef Q_OS_WIN
    FIRMWARE_TYPE type = FirmwareTypeUnknown;
    return GetFirmwareType(&type) && type == FirmwareTypeUefi;
#else
    return QFileInfo::exists(varsDir);
#endif
}

std::optional<QByteArray> variable(const QString &name, const char *guid)
{
#ifdef Q_OS_WIN
    if (!allowFirmwareAccess())
        return std::nullopt;
    QByteArray data(64 * 1024, '\0');
    for (int attempt = 0; attempt < 4; ++attempt) {
        const DWORD n = GetFirmwareEnvironmentVariableW(win::wide(name).c_str(), win::wide(QLatin1Char('{') + QLatin1String(guid) + QLatin1Char('}')).c_str(),
                                                        data.data(), DWORD(data.size()));
        if (n > 0)
            return data.left(qsizetype(n));
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return std::nullopt;
        data.resize(data.size() * 4);
    }
    return std::nullopt;
#else
    // efivarfs puts the variable's 4 attribute bytes in front.
    QFile f(varsDir + QLatin1Char('/') + name + QLatin1Char('-') + QLatin1String(guid));
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QByteArray all = f.readAll();
    if (all.size() < 4)
        return std::nullopt;
    return all.mid(4);
#endif
}

Signatures signatures(const QByteArray &lists)
{
    Signatures out;
    // EFI_SIGNATURE_LIST: type, list size, header size, size of each signature; each signature
    // is its owner's GUID, then the certificate or hash.
    for (qsizetype at = 0; at + 28 <= lists.size();) {
        const char *p = lists.constData() + at;
        const QString type = guidText(p);
        const quint32 listSize = u32(p + 16), headerSize = u32(p + 20), size = u32(p + 24);
        if (listSize < 28 || at + qsizetype(listSize) > lists.size() || size <= 16 || 28 + qsizetype(headerSize) > qsizetype(listSize))
            break;
        for (qsizetype s = 28 + headerSize; s + size <= listSize; s += size) {
            const QByteArray data(p + s + 16, size - 16);
            if (type == QLatin1String(kX509))
                out.certificates << data;
            else if (type == QLatin1String(kSha256) && data.size() == 32)
                out.sha256.insert(data);
        }
        at += listSize;
    }
    return out;
}

Trust trust()
{
    Trust t;
    const std::optional<QByteArray> db = variable(QStringLiteral("db"), kSecurity);
    const std::optional<QByteArray> dbx = variable(QStringLiteral("dbx"), kSecurity);
    const std::optional<QByteArray> secureBoot = variable(QStringLiteral("SecureBoot"));
    const std::optional<QByteArray> setupMode = variable(QStringLiteral("SetupMode"));
    t.known = db.has_value();
    t.secureBoot = secureBoot && !secureBoot->isEmpty() && secureBoot->at(0) == 1;
    t.setupMode = setupMode && !setupMode->isEmpty() && setupMode->at(0) == 1;
    if (db)
        t.allowed = signatures(*db);
    if (dbx)
        t.banned = signatures(*dbx);
    if (const std::optional<QByteArray> pk = variable(QStringLiteral("PK")))
        t.platformKeys = signatures(*pk).certificates;
    return t;
}

std::optional<BootEntry> parseLoadOption(int number, const QByteArray &option)
{
    // EFI_LOAD_OPTION: attributes, the device path's length, the name (UTF-16, ending in a
    // NUL), the device path, then data for the program.
    if (option.size() < 8)
        return std::nullopt;
    BootEntry e;
    e.number = number;
    e.active = u32(option.constData()) & 0x1;
    const quint16 pathLength = u16(option.constData() + 4);
    qsizetype at = 6;
    while (at + 1 < option.size() && u16(option.constData() + at) != 0) {
        e.description += QChar(char16_t(u16(option.constData() + at)));
        at += 2;
    }
    at += 2;
    const qsizetype end = std::min<qsizetype>(option.size(), at + pathLength);
    // Device path nodes: type, subtype, length.
    while (at + 4 <= end) {
        const uchar type = uchar(option[at]), subtype = uchar(option[at + 1]);
        const quint16 length = u16(option.constData() + at + 2);
        if (length < 4 || at + length > end || type == 0x7f)
            break;
        const char *node = option.constData() + at + 4;
        // A hard disk's partition: number, start, size, its signature, the table's kind, and
        // the signature's kind (2: a GPT partition's GUID).
        if (type == 4 && subtype == 1 && length >= 42 && uchar(node[37]) == 2)
            e.partition = guidText(node + 20);
        else if (type == 4 && subtype == 4) { // a file
            QString path;
            for (qsizetype i = 0; i + 1 < length - 4; i += 2) {
                const quint16 c = u16(node + i);
                if (c == 0)
                    break;
                path += QChar(char16_t(c));
            }
            e.path += path;
        }
        at += length;
    }
    return e;
}

QVector<BootEntry> bootEntries()
{
    QVector<int> order;
    if (const std::optional<QByteArray> bootOrder = variable(QStringLiteral("BootOrder"))) {
        for (qsizetype i = 0; i + 1 < bootOrder->size(); i += 2)
            order << u16(bootOrder->constData() + i);
    }
    // The ones not in BootOrder too, from efivarfs' file names (Windows can't list variables).
#ifndef Q_OS_WIN
    static const QRegularExpression name(QStringLiteral("^Boot([0-9A-F]{4})-") + QLatin1String(kGlobal) + QLatin1Char('$'));
    for (const QString &file : QDir(varsDir).entryList(QDir::Files)) {
        const QRegularExpressionMatch m = name.match(file);
        if (m.hasMatch() && !order.contains(m.captured(1).toInt(nullptr, 16)))
            order << m.captured(1).toInt(nullptr, 16);
    }
#endif
    QVector<BootEntry> entries;
    for (int number : std::as_const(order)) {
        const QString var = QStringLiteral("Boot%1").arg(number, 4, 16, QLatin1Char('0')).toUpper().replace(QLatin1String("BOOT"), QLatin1String("Boot"));
        if (const std::optional<QByteArray> option = variable(var)) {
            if (std::optional<BootEntry> e = parseLoadOption(number, *option))
                entries << *e;
        }
    }
    return entries;
}

} // namespace efi
