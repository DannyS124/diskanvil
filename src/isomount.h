// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "udiskssteps.h"

struct Volume;

// Opens an ISO read-only as a loop device and mounts it, to read its files: Windows ISOs keep
// theirs in UDF, which only the kernel reads (isofs reads ISO 9660 with Joliet names).
// close() unmounts it and lets the loop device go again. On Windows, Windows opens it itself.
class IsoMount : public UDisksSteps
{
    Q_OBJECT
public:
    IsoMount(UDisks *udisks, const QString &isoPath, QObject *parent = nullptr);

    void open();
    void close();
    QString mountPoint() const { return m_mountPoint; }

signals:
    void ready(const QString &mountPoint);
    void closed();

private:
    const Volume *volume() const;
    void opened();

    QString m_iso;
    QString m_loop;
    QString m_mountPoint;
    bool m_opening = false;
    bool m_closeWhenOpen = false; // close() came while it was still opening
    bool m_openedHere = false;    // Windows: not one the user had open already
    QMetaObject::Connection m_openedConn;
};
