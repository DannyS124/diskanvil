// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jobui.h"

#include "blockio.h"
#include "format.h"

#include <QLabel>
#include <QLayout>
#include <QPointer>
#include <QProgressBar>
#include <QScreen>

#include <algorithm>
#include <memory>

QString durationText(double seconds)
{
    if (seconds < 90)
        return QObject::tr("about a minute");
    if (seconds < 90 * 60)
        return QObject::tr("about %n minute(s)", nullptr, qRound(seconds / 60));
    return QObject::tr("about %n hour(s)", nullptr, qRound(seconds / 3600));
}

namespace {

bool isOne(const Job &job, std::initializer_list<const char *> operations)
{
    for (const char *op : operations) {
        if (job.operation == QLatin1String(op))
            return true;
    }
    return false;
}

bool selfErasing(const Job &job)
{
    return isOne(job, {"ata-secure-erase", "ata-enhanced-secure-erase", "nvme-format-ns", "nvme-sanitize"});
}

int percent(const Job &job)
{
    return int(std::clamp(job.progress, 0.0, 1.0) * 100);
}

} // namespace

bool jobSelfErasing(const Job &job)
{
    return selfErasing(job);
}

bool jobShown(const Job &job)
{
    return selfErasing(job)
        || isOne(job, {"format-erase", "format-mkfs", "filesystem-check", "filesystem-repair", "filesystem-resize", "encrypted-resize"});
}

QString jobVerb(const Job &job)
{
    if (job.operation == QLatin1String("format-erase"))
        return QObject::tr("Wiping");
    if (job.operation == QLatin1String("filesystem-check"))
        return QObject::tr("Checking");
    if (job.operation == QLatin1String("filesystem-repair"))
        return QObject::tr("Repairing");
    if (isOne(job, {"filesystem-resize", "encrypted-resize"}))
        return QObject::tr("Resizing");
    if (job.operation == QLatin1String("format-mkfs"))
        return QObject::tr("Formatting");
    if (selfErasing(job))
        return QObject::tr("Erasing");
    return QObject::tr("Working on");
}

QString jobCantStop(const Job &job)
{
    if (isOne(job, {"format-erase", "filesystem-check"}))
        return job.cancelable ? QString() : QObject::tr("UDisks doesn't let this one be stopped.");
    if (selfErasing(job))
        return QObject::tr("The drive is erasing itself and can't be interrupted. Don't unplug it or turn off the PC, or it can stay locked.");
    return QObject::tr("Stopping it halfway would damage the file system, so it finishes on its own.");
}

QString jobProgress(const Job &job, quint64 nowUsec)
{
    QStringList parts;
    if (job.progressValid)
        parts << QStringLiteral("%1%").arg(percent(job));
    if (job.rate)
        parts << QObject::tr("%1/s").arg(formatSize(job.rate));
    double left = -1;
    const double done = std::clamp(job.progress, 0.0, 1.0);
    if (job.expectedEnd > nowUsec)
        left = double(job.expectedEnd - nowUsec) / 1e6;
    else if (job.progressValid && job.rate && job.bytes && done < 1)
        left = double(job.bytes) * (1 - done) / double(job.rate);
    else if (job.progressValid && done > 0.01 && done < 1 && job.started && nowUsec > job.started)
        left = double(nowUsec - job.started) / 1e6 * (1 - done) / done;
    if (left >= 0)
        parts << QObject::tr("%1 left").arg(durationText(left));
    return parts.join(QStringLiteral(", "));
}

QString jobStopQuestion(const Job &job, const QString &name, bool wholeDrive)
{
    if (job.operation == QLatin1String("filesystem-check"))
        return QObject::tr("Stop checking %1?\n\nChecking only reads, so nothing has been changed.").arg(name);
    return wholeDrive ? QObject::tr("Stop wiping %1?\n\nWhat's already wiped is gone and the rest stays as it was. To use the "
                                    "drive again, make a new partition table.").arg(name)
                      : QObject::tr("Stop wiping %1?\n\nWhat's already wiped is gone and the rest stays as it was. To use it "
                                    "again, format it.").arg(name);
}

QString jobStopButton(const Job &job)
{
    return job.operation == QLatin1String("filesystem-check") ? QObject::tr("Stop Checking") : QObject::tr("Stop Wiping");
}

QString jobStoppedMessage(const Job &job, const QString &name)
{
    if (job.operation == QLatin1String("filesystem-check"))
        return QObject::tr("Stopped checking %1.").arg(name);
    return job.progressValid ? QObject::tr("Stopped wiping %1 at %2%.").arg(name).arg(percent(job))
                             : QObject::tr("Stopped wiping %1.").arg(name);
}

QString diskTitle(const Disk &d)
{
    return QStringLiteral("%1 (%2, %3)").arg(d.model, shortDevice(d.device), formatSize(d.size));
}

void fitHeight(QWidget *dialog)
{
    QLayout *l = dialog->layout();
    if (!l)
        return;
    l->activate();
    int needed = l->hasHeightForWidth() ? l->totalHeightForWidth(dialog->width()) : l->totalSizeHint().height();
    // On a small screen, what can shrink does (a block map).
    if (const QScreen *screen = dialog->screen()) {
        const int frame = dialog->frameGeometry().height() - dialog->height();
        needed = std::min(needed, std::max(l->totalMinimumSize().height(), screen->availableGeometry().height() - frame));
    }
    dialog->setMinimumHeight(needed);
    if (needed > dialog->height())
        dialog->resize(dialog->width(), needed);
    keepOnScreen(dialog);
}

void keepOnScreen(QWidget *window)
{
    const QScreen *screen = window->screen();
    if (!screen)
        return;
    const QRect room = screen->availableGeometry();
    const QRect frame = window->frameGeometry();
    QPoint to = frame.topLeft();
    if (frame.bottom() > room.bottom())
        to.ry() -= frame.bottom() - room.bottom();
    if (frame.right() > room.right())
        to.rx() -= frame.right() - room.right();
    to.setX(std::max(to.x(), room.left()));
    to.setY(std::max(to.y(), room.top()));
    if (to != frame.topLeft())
        window->move(to);
}

PhaseProgress::PhaseProgress(QProgressBar *bar, QLabel *label)
    : m_bar(bar)
    , m_label(label)
{
}

void PhaseProgress::update(const QString &phase, quint64 done, quint64 total)
{
    if (phase != m_phase) {
        m_phase = phase;
        m_timer.start();
    }
    m_bar->setRange(0, 1000);
    m_bar->setValue(total ? int(done * 1000 / total) : 0);
    m_bar->setVisible(true);
    const double seconds = m_timer.nsecsElapsed() / 1e9;
    QString text = phase;
    if (seconds > 2 && done < total) {
        const double rate = done / seconds;
        text += QObject::tr(", %1/s, %2 left").arg(formatSize(quint64(rate)), durationText((total - done) / std::max(rate, 1.0)));
    }
    m_label->setText(text);
}

void openBlockThen(UDisks *udisks, QObject *context, const QString &objectPath, UDisks::OpenMode mode,
                   const std::function<void(int fd)> &then)
{
    auto conn = std::make_shared<QMetaObject::Connection>();
    QPointer<QObject> guard(context);
    *conn = QObject::connect(udisks, &UDisks::deviceOpened, udisks, [conn, guard, objectPath, then](const QString &path, int fd) {
        if (path != objectPath)
            return;
        QObject::disconnect(*conn);
        if (!guard) {
            if (fd >= 0)
                blockio::close(fd);
            return;
        }
        then(fd);
    });
    udisks->openBlock(objectPath, mode);
}
