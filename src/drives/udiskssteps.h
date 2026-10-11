// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>

#include <functional>
#include <memory>

class UDisks;

// The way DiskForge chains UDisks calls: each one reports through operationFinished, and the
// next step often has to wait until UDisks shows the result (a new partition, a mount point).
// UsbPrep and IsoMount are built on it.
class UDisksSteps : public QObject
{
    Q_OBJECT
public:
    explicit UDisksSteps(UDisks *udisks, QObject *parent = nullptr);

signals:
    void phase(const QString &text);
    // A step failed. `shownAlready`: it was a UDisks error, which the main window shows itself.
    void failed(const QString &message, bool shownAlready);

protected:
    // Starts following UDisks results (once; calling it again does nothing).
    void followResults();
    void stopFollowing();
    // The next UDisks result goes to `next`. A failure ends it all, unless `failureIsFine`
    // (then `next` runs anyway and checks for itself).
    void expect(const std::function<void()> &next, bool failureIsFine = false);
    // Runs `then` once `ready()` holds (checked whenever UDisks reports a change), or fails with
    // `timeoutMessage` after `seconds`.
    void waitFor(const std::function<bool()> &ready, int seconds, const std::function<void()> &then,
                 const QString &timeoutMessage);
    void fail(const QString &message, bool shownAlready = false);

    UDisks *m_udisks;

private:
    std::function<void()> m_next;
    bool m_failureIsFine = false;
    bool m_following = false;
    QMetaObject::Connection m_opConn;
    std::shared_ptr<bool> m_waiting;
};
