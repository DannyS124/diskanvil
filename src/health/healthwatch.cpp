// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "healthwatch.h"

#include "healthhistory.h"
#include "hostprocess.h"
#include "rescuestick.h"
#include "udisks.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTextStream>

#ifdef Q_OS_WIN
#include <QMessageBox>
#include <QPushButton>
#else
#include <QDBusConnection>
#include <QDBusMessage>
#endif

namespace healthwatch {

QString unitFolderForTest;
bool printInsteadForTest = false;

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("healthwatch", text);
}

#ifdef Q_OS_WIN
const QString kTask = QStringLiteral("DiskAnvil health check");

int schtasks(const QStringList &args, QString *error = nullptr)
{
    QProcess p;
    p.start(QStringLiteral("schtasks.exe"), args);
    if (!p.waitForFinished(30000))
        return -1;
    if (error)
        *error = QString::fromLocal8Bit(p.readAllStandardError()).trimmed();
    return p.exitCode();
}
#else
const QString kUnit = QStringLiteral("diskanvil-health");

QString unitFolder()
{
    return !unitFolderForTest.isEmpty() ? unitFolderForTest : QDir::homePath() + QStringLiteral("/.config/systemd/user");
}

// What the timer starts: this same DiskAnvil, however it was installed.
QString command()
{
    if (host::inFlatpak())
        return QStringLiteral("/usr/bin/flatpak run --command=diskanvil io.github.DannyS124.DiskAnvil ") + QLatin1String(kOption);
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    const QString program = !appImage.isEmpty() ? appImage : QCoreApplication::applicationFilePath();
    return QLatin1Char('"') + program + QStringLiteral("\" ") + QLatin1String(kOption);
}

int systemctl(const QStringList &args)
{
    if (!unitFolderForTest.isEmpty())
        return 0; // the tests check the files, not the user's systemd
    return host::run(QStringList{QStringLiteral("systemctl"), QStringLiteral("--user")} + args);
}
#endif

void notify(const QStringList &lines)
{
    const QString title = tr("A drive's health got worse");
    const QString body = lines.join(QLatin1Char('\n')) + QStringLiteral("\n") + tr("Open DiskAnvil, Disk Health, for what it means.");
    if (printInsteadForTest) {
        QTextStream(stdout) << title << "\n" << body << Qt::endl;
        return;
    }
#ifdef Q_OS_WIN
    // A window in front, not a tray message: Windows 11 drops those from a program it has no
    // shortcut ID for. The task runs in the signed-in person's session, so they see it.
    QMessageBox box(QMessageBox::Warning, QStringLiteral("DiskAnvil"), title, QMessageBox::Close);
    box.setInformativeText(body);
    box.setWindowFlag(Qt::WindowStaysOnTopHint);
    QPushButton *open = box.addButton(tr("Open DiskAnvil"), QMessageBox::AcceptRole);
    box.exec();
    if (box.clickedButton() == open)
        QProcess::startDetached(QCoreApplication::applicationFilePath(), {});
#else
    QDBusMessage m = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("/org/freedesktop/Notifications"),
                                                    QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("Notify"));
    m << QStringLiteral("DiskAnvil") << uint(0) << QStringLiteral("io.github.DannyS124.DiskAnvil") << title << body << QStringList()
      << QVariantMap{{QStringLiteral("urgency"), QVariant::fromValue(uchar(2))}} << int(0);
    QDBusConnection::sessionBus().call(m, QDBus::Block, 10000);
#endif
}

} // namespace

QString unavailableReason()
{
    if (rescue::runningInRescue())
        return tr("DiskAnvil Live only runs while it's started from the stick, so a daily check is for DiskAnvil on a PC. "
                  "The history here is kept all the same%1.")
            .arg(rescue::keepsChanges() ? tr(", on the stick (Keep my changes is on)") : tr(" until DiskAnvil Live is turned off"));
    return {};
}

QString historyGoesReason()
{
    if (rescue::runningInRescue() && !rescue::keepsChanges())
        return tr("DiskAnvil Live forgets this when it's turned off. Started with Keep my changes, it keeps it on the stick.");
    return {};
}

bool enabled()
{
#ifdef Q_OS_WIN
    return schtasks({QStringLiteral("/Query"), QStringLiteral("/TN"), kTask}) == 0;
#else
    return QFile::exists(unitFolder() + QLatin1Char('/') + kUnit + QStringLiteral(".timer"));
#endif
}

bool setEnabled(bool on, QString *message)
{
    if (const QString why = unavailableReason(); on && !why.isEmpty()) {
        *message = why;
        return false;
    }
#ifdef Q_OS_WIN
    QString error;
    int code = 0;
    if (on) {
        // Through PowerShell rather than schtasks /Create, which can't say what the Linux timer does:
        // a day the PC was off at noon is checked when it's next on, and laptops on battery too.
        QProcess ps;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("DA_EXE"), QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
        env.insert(QStringLiteral("DA_TASK"), kTask);
        ps.setProcessEnvironment(env);
        ps.start(QStringLiteral("powershell.exe"),
                 {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-Command"),
                  QStringLiteral("$ErrorActionPreference = 'Stop'; "
                                 "$a = New-ScheduledTaskAction -Execute $env:DA_EXE -Argument '%1'; "
                                 "$t = New-ScheduledTaskTrigger -Daily -At 12:00; "
                                 "$s = New-ScheduledTaskSettingsSet -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries "
                                 "-ExecutionTimeLimit (New-TimeSpan -Hours 1); "
                                 "$p = New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) "
                                 "-LogonType Interactive -RunLevel Highest; "
                                 "Register-ScheduledTask -TaskName $env:DA_TASK -Action $a -Trigger $t -Settings $s -Principal $p -Force | Out-Null")
                      .arg(QLatin1String(kOption))});
        code = ps.waitForFinished(60000) ? ps.exitCode() : -1;
        error = QString::fromLocal8Bit(ps.readAllStandardError()).trimmed();
    } else {
        code = schtasks({QStringLiteral("/Delete"), QStringLiteral("/F"), QStringLiteral("/TN"), kTask}, &error);
    }
    if (code != 0) {
        *message = tr("Task Scheduler said no: %1").arg(error);
        return false;
    }
    *message = on ? tr("DiskAnvil checks your drives every day now (Task Scheduler, \"%1\").").arg(kTask) : tr("The daily check is off.");
    return true;
#else
    const QString base = unitFolder() + QLatin1Char('/') + kUnit;
    if (!on) {
        systemctl({QStringLiteral("disable"), QStringLiteral("--now"), kUnit + QStringLiteral(".timer")});
        QFile::remove(base + QStringLiteral(".timer"));
        QFile::remove(base + QStringLiteral(".service"));
        systemctl({QStringLiteral("daemon-reload")});
        *message = tr("The daily check is off.");
        return true;
    }
    QDir().mkpath(unitFolder());
    QFile service(base + QStringLiteral(".service")), timer(base + QStringLiteral(".timer"));
    if (!service.open(QIODevice::WriteOnly | QIODevice::Truncate) || !timer.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *message = tr("Couldn't write the timer to %1.").arg(unitFolder());
        return false;
    }
    service.write(QStringLiteral("[Unit]\nDescription=DiskAnvil: check the drives' health\n\n[Service]\nType=oneshot\n"
                                 "Environment=QT_QPA_PLATFORM=offscreen\nNice=19\nExecStart=%1\n").arg(command()).toUtf8());
    timer.write("[Unit]\nDescription=DiskAnvil: check the drives' health every day\n\n[Timer]\nOnCalendar=daily\n"
                "RandomizedDelaySec=1h\nPersistent=true\n\n[Install]\nWantedBy=timers.target\n");
    service.close();
    timer.close();
    systemctl({QStringLiteral("daemon-reload")});
    if (systemctl({QStringLiteral("enable"), QStringLiteral("--now"), kUnit + QStringLiteral(".timer")}) != 0) {
        QFile::remove(base + QStringLiteral(".timer"));
        QFile::remove(base + QStringLiteral(".service"));
        *message = tr("systemd wouldn't start the timer (systemctl --user enable --now %1.timer).").arg(kUnit);
        if (host::inFlatpak())
            *message += QLatin1Char(' ') + tr("The Flatpak needs leave to start it: flatpak override --user --talk-name=org.freedesktop.Flatpak "
                                              "io.github.DannyS124.DiskAnvil");
        return false;
    }
    *message = tr("DiskAnvil checks your drives every day now (%1.timer).").arg(kUnit);
    return true;
#endif
}

QStringList check(UDisks *udisks, const QVector<Disk> &disks)
{
    QStringList out;
    for (const Disk &d : disks) {
        if (d.health.state == Health::State::Unknown || d.health.key.isEmpty() || d.isLoop)
            continue;
        const healthhistory::Point now = healthhistory::pointFor(d, udisks->smartAttributes(d));
        healthhistory::note(d, now);
        healthhistory::History h = healthhistory::load(d.health.key);
        const QStringList lines = healthhistory::worse(h.checked, now);
        if (!lines.isEmpty())
            out << QStringLiteral("%1 (%2): %3").arg(d.model.isEmpty() ? d.device : d.model, d.device, lines.join(QLatin1Char(' ')));
        h.checked = now;
        healthhistory::save(h);
    }
    return out;
}

int run(UDisks *udisks)
{
    const QStringList lines = check(udisks, udisks->disks());
    if (!lines.isEmpty())
        notify(lines);
    return 0;
}

} // namespace healthwatch
