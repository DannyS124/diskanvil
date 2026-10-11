// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "udiskssteps.h"

#include "applog.h"
#include "udisks.h"

#include <QTimer>

#include <utility>

UDisksSteps::UDisksSteps(UDisks *udisks, QObject *parent)
    : QObject(parent)
    , m_udisks(udisks)
{
}

void UDisksSteps::followResults()
{
    if (m_following)
        return;
    m_following = true;
    m_opConn = connect(m_udisks, &UDisks::operationFinished, this, [this](bool ok, const QString &message) {
        if (!m_following || !m_next)
            return;
        const std::function<void()> next = std::exchange(m_next, nullptr);
        if (!ok && !m_failureIsFine)
            return fail(message, true);
        next();
    });
}

void UDisksSteps::stopFollowing()
{
    disconnect(m_opConn);
    m_next = nullptr;
    if (m_waiting)
        *m_waiting = false;
    m_following = false;
}

void UDisksSteps::fail(const QString &message, bool shownAlready)
{
    stopFollowing();
    qCInfo(lcOps).noquote() << metaObject()->className() << "failed:" << message;
    emit failed(message, shownAlready);
}

void UDisksSteps::expect(const std::function<void()> &next, bool failureIsFine)
{
    m_next = next;
    m_failureIsFine = failureIsFine;
}

void UDisksSteps::waitFor(const std::function<bool()> &ready, int seconds, const std::function<void()> &then,
                          const QString &timeoutMessage)
{
    if (m_waiting)
        *m_waiting = false;
    auto alive = std::make_shared<bool>(true);
    m_waiting = alive;
    auto conn = std::make_shared<QMetaObject::Connection>();
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    auto check = [alive, ready, then, conn, timer] {
        if (!*alive || !ready())
            return;
        *alive = false;
        QObject::disconnect(*conn);
        timer->stop();
        timer->deleteLater();
        then();
    };
    *conn = connect(m_udisks, &UDisks::changed, this, check);
    connect(timer, &QTimer::timeout, this, [this, alive, conn, timer, timeoutMessage] {
        timer->deleteLater();
        if (!*alive)
            return;
        *alive = false;
        disconnect(*conn);
        fail(timeoutMessage);
    });
    timer->start(seconds * 1000);
    QTimer::singleShot(0, this, check); // it may be true already
}
