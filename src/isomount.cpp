// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "isomount.h"

#include "udisks.h"

#include <QFileInfo>

IsoMount::IsoMount(UDisks *udisks, const QString &isoPath, QObject *parent)
    : UDisksSteps(udisks, parent)
    , m_iso(isoPath)
{
    // A failed open is over too: a close() that was waiting for it can go ahead.
    connect(this, &UDisksSteps::failed, this, [this] {
        m_opening = false;
        if (m_closeWhenOpen)
            close();
    });
}

const Volume *IsoMount::volume() const
{
    // An ISO has no partition table, so its one file system is the whole loop device.
    const Disk *d = m_udisks->diskByPath(m_loop);
    for (const Volume &v : d ? d->volumes : QVector<Volume>()) {
        if (v.hasFilesystem)
            return &v;
    }
    return nullptr;
}

void IsoMount::open()
{
#ifdef Q_OS_WIN
    // Windows opens ISOs itself, UDF and all, with a drive letter of their own.
    m_opening = true;
    emit phase(tr("Opening %1…").arg(QFileInfo(m_iso).fileName()));
    m_udisks->openIso(m_iso, this, [this](const QString &root, bool openedHere, const QString &error) {
        if (!error.isEmpty())
            return fail(error);
        m_openedHere = openedHere;
        m_mountPoint = root;
        opened();
    });
    return;
#endif
    m_opening = true;
    followResults();
    emit phase(tr("Opening %1…").arg(QFileInfo(m_iso).fileName()));
    m_openedConn = connect(m_udisks, &UDisks::imageOpened, this, [this](const QString &loop) {
        disconnect(m_openedConn);
        m_loop = loop;
    });
    expect([this] {
        waitFor([this] { return volume() != nullptr; }, 30,
                [this] {
                    const Volume *v = volume();
                    if (!v->mounts().isEmpty()) {
                        m_mountPoint = v->mounts().first();
                        opened();
                        return;
                    }
                    const QString path = v->objectPath;
                    expect([this, path] {
                        waitFor([this] { return volume() && !volume()->mounts().isEmpty(); }, 15,
                                [this] {
                                    m_mountPoint = volume()->mounts().first();
                                    opened();
                                },
                                tr("The ISO didn't mount."));
                    }, true);
                    m_udisks->mount(*v);
                },
                tr("The ISO's files didn't show up. Is it really an ISO?"));
    });
    m_udisks->openImage(m_iso, true);
}

void IsoMount::opened()
{
#ifndef Q_OS_WIN
    stopFollowing();
#endif
    m_opening = false;
    if (m_closeWhenOpen)
        return close();
    emit ready(m_mountPoint);
}

void IsoMount::close()
{
    if (m_opening) {
        // Still opening: close it as soon as it's open, so the loop device doesn't stay behind.
        m_closeWhenOpen = true;
        return;
    }
#ifdef Q_OS_WIN
    // One the user had open already stays open.
    if (!m_openedHere || m_mountPoint.isEmpty()) {
        emit closed();
        return;
    }
    m_mountPoint.clear();
    m_udisks->closeIso(m_iso, this, [this] { emit closed(); });
    return;
#endif
    const Disk *d = m_loop.isEmpty() ? nullptr : m_udisks->diskByPath(m_loop);
    if (!d) {
        emit closed();
        return;
    }
    followResults();
    // Closing it is tidying up: whatever happens, it's done.
    expect([this] {
        stopFollowing();
        emit closed();
    }, true);
    m_udisks->detachImage(*d);
}
