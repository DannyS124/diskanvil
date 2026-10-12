// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "usbinfo.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace usbinfo {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("usbinfo", text);
}

QString read(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
}

// A name from the pci.ids list: the vendor, then its device (indented by a tab under it).
QString pciName(const QString &ids, QString vendor, QString device)
{
    vendor = vendor.remove(QStringLiteral("0x")).toLower();
    device = device.remove(QStringLiteral("0x")).toLower();
    QFile f(ids);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QString vendorName;
    bool inVendor = false;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine();
        if (line.startsWith('#') || line.trimmed().isEmpty())
            continue;
        if (!line.startsWith('\t')) {
            if (inVendor)
                break;
            inVendor = line.startsWith(vendor.toLatin1() + "  ");
            if (inVendor)
                vendorName = QString::fromUtf8(line.mid(6)).trimmed();
        } else if (inVendor && !line.startsWith("\t\t") && line.mid(1).startsWith(device.toLatin1() + "  ")) {
            return vendorName + QLatin1Char(' ') + QString::fromUtf8(line.mid(7)).trimmed();
        }
    }
    return vendorName;
}

} // namespace

QString speedName(int mbps)
{
    if (mbps <= 12)
        return tr("USB 1 (%1 Mbit/s)").arg(mbps);
    if (mbps <= 480)
        return tr("USB 2 (480 Mbit/s)");
    if (mbps <= 5000)
        return tr("USB 3 (5 Gbit/s)");
    if (mbps <= 10000)
        return tr("USB 3 (10 Gbit/s)");
    return tr("USB 3 (%1 Gbit/s)").arg(mbps / 1000);
}

Info of(const QString &device, const QString &sysfs, const QString &pciIds)
{
    Info info;
    const QString name = device.section(QLatin1Char('/'), -1);
    // From the drive up: the first folder with a speed is its USB device, the hub above it says
    // what the port can do, and the PCI device above that is the controller.
    QString at = QFileInfo(sysfs + QStringLiteral("/block/") + name).canonicalFilePath();
    while (!at.isEmpty() && at != sysfs && at != QLatin1String("/")) {
        if (!info.known && QFile::exists(at + QStringLiteral("/speed")) && QFile::exists(at + QStringLiteral("/idVendor"))) {
            info.known = true;
            info.speedMbps = read(at + QStringLiteral("/speed")).toInt();
            info.version = read(at + QStringLiteral("/version"));
            info.maker = read(at + QStringLiteral("/manufacturer"));
            info.product = read(at + QStringLiteral("/product"));
            info.vendorId = read(at + QStringLiteral("/idVendor"));
            info.productId = read(at + QStringLiteral("/idProduct"));
            info.port = QFileInfo(at).fileName();
        } else if (info.known && info.portMbps == 0 && QFile::exists(at + QStringLiteral("/speed"))) {
            info.portMbps = read(at + QStringLiteral("/speed")).toInt();
        }
        if (info.known && QFile::exists(at + QStringLiteral("/class")) && read(at + QStringLiteral("/class")).startsWith(QLatin1String("0x0c03"))) {
            info.controller = pciName(pciIds, read(at + QStringLiteral("/vendor")), read(at + QStringLiteral("/device")));
            break;
        }
        at = QFileInfo(at).path();
    }
    return info;
}

QString describe(const Info &info)
{
    if (!info.known)
        return {};
    QString text = tr("Runs at %1, on port %2").arg(speedName(info.speedMbps), info.port);
    if (!info.controller.isEmpty())
        text += tr(" of %1").arg(info.controller);
    text += QLatin1Char('.');
    const bool madeFor3 = info.version.toDouble() >= 3.0;
    if (madeFor3 && info.speedMbps <= 480)
        text += QLatin1Char(' ') + tr("It's made for USB 3 but runs at USB 2 speed: in a USB 3 port (often blue, or marked SS) and with a "
                                      "USB 3 cable, it's up to ten times faster.");
    else if (!madeFor3 && info.speedMbps <= 480 && info.portMbps > 480)
        text += QLatin1Char(' ') + tr("It's a USB 2 drive: the port could go faster, the drive can't.");
    return text;
}

} // namespace usbinfo
