// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// How a USB drive is connected: the speed it runs at and the one it's made for, its port, and the
// controller (Linux: sysfs, and the pci.ids list for names). A USB 3 stick in a USB 2 port runs at
// a tenth of its speed: worth saying.

#include <QString>

namespace usbinfo {

struct Info {
    bool known = false;
    int speedMbps = 0;   // what it runs at now: 480 (USB 2), 5000, 10000, 20000
    QString version;     // what it's made for ("3.20")
    int portMbps = 0;    // what the port's hub can do
    QString maker, product, vendorId, productId;
    QString port;        // "2-3": bus 2, port 3
    QString controller;  // "Advanced Micro Devices, Inc. [AMD] ... USB 3.1 xHCI Controller"
};

// For a drive (/dev/sdb). `sysfs`: where /sys is (the tests use their own).
Info of(const QString &device, const QString &sysfs = QStringLiteral("/sys"), const QString &pciIds = QStringLiteral("/usr/share/hwdata/pci.ids"));
QString speedName(int mbps); // "USB 2 (480 Mbit/s)"
// In plain words, with what to do when it's slower than it could be.
QString describe(const Info &info);

} // namespace usbinfo
