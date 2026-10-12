// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "efiboot.h"

#include <QtEndian>

namespace efiboot {

namespace {

int number16(const std::optional<QByteArray> &value)
{
    return value && value->size() >= 2 ? qFromLittleEndian<quint16>(value->constData()) : -1;
}

void put16(QByteArray &out, quint16 v)
{
    char b[2];
    qToLittleEndian<quint16>(v, b);
    out.append(b, 2);
}

void put32(QByteArray &out, quint32 v)
{
    char b[4];
    qToLittleEndian<quint32>(v, b);
    out.append(b, 4);
}

void put64(QByteArray &out, quint64 v)
{
    char b[8];
    qToLittleEndian<quint64>(v, b);
    out.append(b, 8);
}

} // namespace

State read()
{
    State s;
    s.uefi = efi::available();
    if (!s.uefi)
        return s;
    s.entries = efi::bootEntries();
    if (const std::optional<QByteArray> order = efi::variable(QStringLiteral("BootOrder"))) {
        for (qsizetype i = 0; i + 1 < order->size(); i += 2)
            s.order << qFromLittleEndian<quint16>(order->constData() + i);
    }
    s.next = number16(efi::variable(QStringLiteral("BootNext")));
    s.current = number16(efi::variable(QStringLiteral("BootCurrent")));
    return s;
}

QString hex4(int number)
{
    return QStringLiteral("%1").arg(number, 4, 16, QLatin1Char('0')).toUpper();
}

QByteArray loadOption(const QString &description, quint32 partitionNumber, quint64 start, quint64 size, const QString &partitionGuid,
                      const QString &path, bool active)
{
    // The device path: the partition (a hard drive node), the file, the end.
    QByteArray devicePath;
    devicePath.append(char(4)).append(char(1));
    put16(devicePath, 42);
    put32(devicePath, partitionNumber);
    put64(devicePath, start);
    put64(devicePath, size);
    devicePath.append(efi::guidBytes(partitionGuid).leftJustified(16, '\0', true));
    devicePath.append(char(2)); // a GPT partition
    devicePath.append(char(2)); // known by its GUID
    QString file = path;
    file.replace(QLatin1Char('/'), QLatin1Char('\\'));
    if (!file.startsWith(QLatin1Char('\\')))
        file.prepend(QLatin1Char('\\'));
    QByteArray name;
    for (const QChar c : file)
        put16(name, c.unicode());
    put16(name, 0);
    devicePath.append(char(4)).append(char(4));
    put16(devicePath, quint16(4 + name.size()));
    devicePath.append(name);
    devicePath.append(char(0x7f)).append(char(0xff));
    put16(devicePath, 4);

    QByteArray option;
    put32(option, active ? 1 : 0);
    put16(option, quint16(devicePath.size()));
    for (const QChar c : description)
        put16(option, c.unicode());
    put16(option, 0);
    option.append(devicePath);
    return option;
}

QByteArray orderValue(const QVector<int> &order)
{
    QByteArray out;
    for (int n : order)
        put16(out, quint16(n));
    return out;
}

QStringList orderCommand(const QVector<int> &order)
{
    QStringList numbers;
    for (int n : order)
        numbers << hex4(n);
    return {QStringLiteral("efibootmgr"), QStringLiteral("-o"), numbers.join(QLatin1Char(','))};
}

QStringList nextCommand(int number)
{
    return {QStringLiteral("efibootmgr"), QStringLiteral("-n"), hex4(number)};
}

QStringList activeCommand(int number, bool active)
{
    return {QStringLiteral("efibootmgr"), QStringLiteral("-b"), hex4(number), active ? QStringLiteral("-a") : QStringLiteral("-A")};
}

QStringList removeCommand(int number)
{
    return {QStringLiteral("efibootmgr"), QStringLiteral("-b"), hex4(number), QStringLiteral("-B")};
}

QStringList createCommand(const QString &disk, int partitionNumber, const QString &path, const QString &label)
{
    QString file = path;
    file.replace(QLatin1Char('/'), QLatin1Char('\\'));
    return {QStringLiteral("efibootmgr"), QStringLiteral("-c"), QStringLiteral("-d"), disk, QStringLiteral("-p"), QString::number(partitionNumber),
            QStringLiteral("-l"), file, QStringLiteral("-L"), label};
}

} // namespace efiboot
