// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "productkey.h"

#include "reghive.h"

#include <QCoreApplication>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace productkey {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("productkey", text);
}

QString grouped(const QString &chars)
{
    QStringList parts;
    for (int i = 0; i < chars.size(); i += 5)
        parts << chars.mid(i, 5);
    return parts.join(QLatin1Char('-'));
}

} // namespace

QString fromMsdm(const QByteArray &table)
{
    // The ACPI header (36 bytes), then the version, a reserved word, the data type, another
    // reserved word, the data's length and the data: the key, 29 characters.
    if (table.size() < 56 + 29 || !table.startsWith("MSDM"))
        return {};
    const QString key = QString::fromLatin1(table.mid(56, 29));
    static const QRegularExpression shape(QStringLiteral("^[A-Z0-9]{5}(-[A-Z0-9]{5}){4}$"));
    return shape.match(key).hasMatch() ? key : QString();
}

QString decode(const QByteArray &value)
{
    // The key is 15 bytes at 52, a number written in base 24 with these characters; since
    // Windows 8, bit 3 of its last byte says an "N" goes in it, where its first character says.
    if (value.size() < 67)
        return {};
    QByteArray key = value.mid(52, 15);
    const bool newer = quint8(key[14]) & 0x08;
    key[14] = char(quint8(key[14]) & 0xF7);
    static const char chars[] = "BCDFGHJKMPQRTVWXY2346789";
    QString out;
    int last = 0;
    for (int i = 24; i >= 0; --i) {
        int current = 0;
        for (int x = 14; x >= 0; --x) {
            current = current * 256 + quint8(key[x]);
            key[x] = char(current / 24);
            current %= 24;
        }
        out.prepend(QLatin1Char(chars[current]));
        last = current;
    }
    if (newer) {
        out.remove(0, 1);
        out.insert(last, QLatin1Char('N'));
    }
    return grouped(out);
}

bool isGeneric(const QString &key)
{
    // Microsoft's default keys for Windows 10 and 11 Home and Pro, and what an empty value
    // decodes to.
    static const QStringList generic = {QStringLiteral("TX9XD-98N7V-6WMQ6-BX7FG-H8Q99"), QStringLiteral("VK7JG-NPHTM-C97JM-9MPGT-3V66T"),
                                        QStringLiteral("BBBBB-BBBBB-BBBBB-BBBBB-BBBBB")};
    return generic.contains(key.trimmed().toUpper());
}

Installed fromHive(const QString &path)
{
    Installed out;
    reg::Hive hive;
    QString error;
    if (!hive.open(path, &error)) {
        out.problem = error;
        return out;
    }
    const reg::Key key = hive.key(QStringLiteral("Microsoft\\Windows NT\\CurrentVersion"));
    if (!key) {
        out.problem = tr("its registry doesn't say");
        return out;
    }
    out.edition = hive.value(key, QStringLiteral("ProductName")).value_or(reg::Value()).text();
    // Windows 11 still calls itself Windows 10 there; its build number says which.
    if (hive.value(key, QStringLiteral("CurrentBuildNumber")).value_or(reg::Value()).text().toInt() >= 22000)
        out.edition.replace(QStringLiteral("Windows 10"), QStringLiteral("Windows 11"));
    out.key = decode(hive.value(key, QStringLiteral("DigitalProductId")).value_or(reg::Value()).data);
    if (out.key.isEmpty())
        out.problem = tr("its registry has no key");
    return out;
}

#ifdef Q_OS_WIN
QString firmwareKey()
{
    const DWORD signature = 0x41435049; // 'ACPI'
    const DWORD table = 0x4D44534D;     // "MSDM", its bytes the other way round, as the API wants them
    const UINT size = GetSystemFirmwareTable(signature, table, nullptr, 0);
    if (size == 0)
        return {};
    QByteArray data(int(size), '\0');
    if (GetSystemFirmwareTable(signature, table, data.data(), size) != size)
        return {};
    return fromMsdm(data);
}

Installed running()
{
    Installed out;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        out.problem = tr("its registry couldn't be read");
        return out;
    }
    auto read = [key](const wchar_t *name) {
        DWORD type = 0, size = 0;
        if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || size == 0)
            return QByteArray();
        QByteArray data(int(size), '\0');
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE *>(data.data()), &size) != ERROR_SUCCESS)
            return QByteArray();
        return data.left(int(size));
    };
    auto text = [&read](const wchar_t *name) {
        const QByteArray d = read(name);
        return QString::fromWCharArray(reinterpret_cast<const wchar_t *>(d.constData()), d.size() / 2).section(QChar(0), 0, 0);
    };
    out.edition = text(L"ProductName");
    if (text(L"CurrentBuildNumber").toInt() >= 22000)
        out.edition.replace(QStringLiteral("Windows 10"), QStringLiteral("Windows 11"));
    out.key = decode(read(L"DigitalProductId"));
    RegCloseKey(key);
    if (out.key.isEmpty())
        out.problem = tr("its registry has no key");
    return out;
}
#endif

} // namespace productkey
