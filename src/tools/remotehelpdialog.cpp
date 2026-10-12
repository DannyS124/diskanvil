// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "remotehelpdialog.h"

#include "applog.h"
#include "remotehelp.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

RemoteHelpDialog::RemoteHelpDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Let Someone Help"));
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel;
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    if (!remotehelp::available()) {
#ifdef Q_OS_WIN
        intro->setText(tr("Windows does this itself: open Quick Assist from the Start menu. The person helping you opens it too, and "
                          "gives you a code.\n\nOn a PC that won't start, start it from DiskAnvil Live: its Let Someone Help opens the "
                          "screen to someone on the same network."));
#else
        intro->setText(tr("On a PC your desktop does this itself: KDE's Desktop Sharing (krfb), GNOME's Settings > Sharing, or Remmina. "
                          "DiskAnvil Live has it built in: there, Let Someone Help opens the screen and a terminal to someone on the "
                          "same network."));
#endif
        layout->addWidget(buttons);
        return;
    }
    intro->setText(tr("Someone you trust, on the same network (the same Wi-Fi or router), can see this screen and use it, or work in a "
                      "terminal. You give them an address and a password, made up now; Stop, or closing this window, shuts them out "
                      "again.\n\nThey can do anything you can here, erasing drives included: only let in someone you trust."));
    m_screen = new QCheckBox(tr("This screen (they need a VNC viewer: TigerVNC, RealVNC, Remmina...)"));
    m_screen->setObjectName(QStringLiteral("screen"));
    m_screen->setChecked(true);
    m_terminal = new QCheckBox(tr("A terminal (SSH: built into Windows, Macs and Linux)"));
    m_terminal->setObjectName(QStringLiteral("terminal"));
    layout->addWidget(m_screen);
    layout->addWidget(m_terminal);
    m_details = new QLabel;
    m_details->setObjectName(QStringLiteral("details"));
    m_details->setWordWrap(true);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_details->setTextFormat(Qt::RichText);
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("status"));
    m_status->setWordWrap(true);
    layout->addWidget(m_details);
    layout->addWidget(m_status);
    m_start = buttons->addButton(tr("Let Them In"), QDialogButtonBox::ActionRole);
    m_stop = buttons->addButton(tr("Stop"), QDialogButtonBox::ActionRole);
    m_stop->hide();
    connect(m_start, &QPushButton::clicked, this, &RemoteHelpDialog::start);
    connect(m_stop, &QPushButton::clicked, this, &RemoteHelpDialog::stop);
    for (QCheckBox *box : {m_screen, m_terminal})
        connect(box, &QCheckBox::toggled, this, [this] { m_start->setEnabled(m_screen->isChecked() || m_terminal->isChecked()); });
    layout->addWidget(buttons);
    // A network plugged in later shows up.
    m_addresses = new QTimer(this);
    m_addresses->setInterval(3000);
    connect(m_addresses, &QTimer::timeout, this, [this] {
        if (remotehelp::addresses() != m_shown)
            showDetails();
    });
    resize(560, sizeHint().height());
}

RemoteHelpDialog::~RemoteHelpDialog()
{
    stop();
}

void RemoteHelpDialog::done(int result)
{
    stop();
    QDialog::done(result);
}

void RemoteHelpDialog::start()
{
    m_connected.clear();
    m_status->clear();
    QStringList failed;
    if (m_screen->isChecked()) {
        m_vncPassword = remotehelp::makePassword(8); // VNC's own passwords are 8 characters at most
        // A file only this user can read; x11vnc reads it and removes it.
        const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        const QString file = (runtime.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::TempLocation) : runtime)
                             + QStringLiteral("/diskanvil-vnc-%1").arg(QCoreApplication::applicationPid());
        QFile pass(file);
        if (pass.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            pass.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            pass.write(m_vncPassword.toUtf8() + '\n');
            pass.close();
        }
        m_vnc = new QProcess(this);
        connect(m_vnc, &QProcess::readyReadStandardOutput, this, &RemoteHelpDialog::readVnc);
        connect(m_vnc, &QProcess::readyReadStandardError, this, &RemoteHelpDialog::readVnc);
        connect(m_vnc, &QProcess::finished, this, [this, file] {
            QFile::remove(file);
            if (!m_vnc)
                return;
            m_vnc->deleteLater();
            m_vnc = nullptr;
            m_status->setText(tr("Sharing the screen stopped by itself (x11vnc ended)."));
            showDetails();
        });
        m_vnc->start(remotehelp::x11vnc(), remotehelp::vncArguments(file));
        if (!m_vnc->waitForStarted(5000)) {
            failed << tr("The screen: %1").arg(m_vnc->errorString());
            m_vnc->deleteLater();
            m_vnc = nullptr;
            QFile::remove(file);
        }
    }
    if (m_terminal->isChecked()) {
        m_sshPassword = remotehelp::makePassword(12);
        const QStringList command = remotehelp::sshCommand(QStringLiteral("ssh-on"));
        QProcess on;
        on.setProcessChannelMode(QProcess::MergedChannels);
        on.start(command.first(), command.mid(1));
        on.write(m_sshPassword.toUtf8() + '\n');
        on.closeWriteChannel();
        m_ssh = on.waitForFinished(30000) && on.exitStatus() == QProcess::NormalExit && on.exitCode() == 0;
        if (!m_ssh)
            failed << tr("The terminal: %1").arg(QString::fromLocal8Bit(on.readAll()).trimmed().right(500));
    }
    qCInfo(lcOps).noquote() << "Let Someone Help:" << (m_vnc ? "screen shared" : "") << (m_ssh ? "ssh on" : "") << failed.join(QLatin1Char(' '));
    if (!failed.isEmpty())
        m_status->setText(tr("That didn't work.\n%1").arg(failed.join(QLatin1Char('\n'))));
    else
        m_status->setText(tr("Waiting for them to connect."));
    showDetails();
    if (helping())
        m_addresses->start();
}

void RemoteHelpDialog::stop()
{
    if (m_addresses)
        m_addresses->stop();
    if (QProcess *vnc = m_vnc) {
        m_vnc = nullptr;
        vnc->terminate();
        if (!vnc->waitForFinished(3000))
            vnc->kill();
        vnc->waitForFinished(1000);
        vnc->deleteLater();
    }
    if (m_ssh) {
        m_ssh = false;
        const QStringList command = remotehelp::sshCommand(QStringLiteral("ssh-off"));
        QProcess off;
        off.start(command.first(), command.mid(1));
        off.waitForFinished(30000);
        qCInfo(lcOps).noquote() << "Let Someone Help: ssh off" << off.exitCode();
    }
    m_vncPassword.clear();
    m_sshPassword.clear();
    m_connected.clear();
    if (m_status && !m_status->text().isEmpty() && !m_status->text().startsWith(tr("That didn't work.")))
        m_status->setText(tr("Stopped: the passwords don't work any more."));
    if (m_details)
        showDetails();
}

void RemoteHelpDialog::readVnc()
{
    if (!m_vnc)
        return;
    const QString text = QString::fromLocal8Bit(m_vnc->readAllStandardOutput() + m_vnc->readAllStandardError());
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("PORT="))) {
            m_port = line.mid(5).trimmed().toInt();
            showDetails();
        } else if (const QString from = remotehelp::connectedFrom(line); !from.isEmpty()) {
            m_connected << from;
            m_status->setText(tr("Connected: %1 can see this screen.").arg(m_connected.join(QStringLiteral(", "))));
        } else if (remotehelp::clientsLeft(line) == 0 && !m_connected.isEmpty()) {
            m_connected.clear();
            m_status->setText(tr("They've gone. The way in stays open until you press Stop."));
        }
    }
}

void RemoteHelpDialog::showDetails()
{
    m_shown = remotehelp::addresses();
    m_start->setVisible(!helping());
    m_stop->setVisible(helping());
    m_screen->setEnabled(!helping());
    m_terminal->setEnabled(!helping());
    if (!helping()) {
        m_details->clear();
        return;
    }
    const QString address = m_shown.value(0);
    QStringList lines;
    if (address.isEmpty()) {
        lines << tr("<b>This PC isn't on a network yet.</b> Connect it (the network icon at the bottom right), and its address shows "
                    "up here.");
    } else {
        lines << tr("This PC's address: <b>%1</b>").arg(address.toHtmlEscaped());
        if (m_shown.size() > 1)
            lines << tr("(also %1)").arg(m_shown.mid(1).join(QStringLiteral(", ")).toHtmlEscaped());
    }
    const QString host = address.contains(QLatin1Char(':')) ? QStringLiteral("[%1]").arg(address) : address;
    if (m_vnc) {
        const int port = m_port ? m_port : 5900;
        lines << tr("The screen: in their VNC viewer, <b>%1:%2</b>, password <b>%3</b>")
                     .arg(host.isEmpty() ? tr("(the address)") : host.toHtmlEscaped())
                     .arg(port)
                     .arg(m_vncPassword.toHtmlEscaped());
    }
    if (m_ssh)
        lines << tr("The terminal: <b>ssh rescue@%1</b>, password <b>%2</b>")
                     .arg(host.isEmpty() ? tr("(the address)") : host.toHtmlEscaped(), m_sshPassword.toHtmlEscaped());
    m_details->setText(lines.join(QStringLiteral("<br>")));
}
