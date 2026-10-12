// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "netbootdialog.h"

#include "applog.h"
#include "dialogs.h"
#include "format.h"
#include "netboot.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

NetBootDialog::NetBootDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Start Other PCs from This One"));
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel;
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    if (!netboot::available()) {
        intro->setText(tr("DiskAnvil Live does this: it hands itself to other PCs on the same network, with nothing installed on them. "
                          "Start a PC from a DiskAnvil Live stick, then Tools > Start Other PCs from This One there."));
        layout->addWidget(buttons);
        return;
    }
    const QString medium = netboot::medium();
    const qint64 size = medium.isEmpty() ? 0 : QFileInfo(medium + QStringLiteral("/live/filesystem.squashfs")).size();
    intro->setText(tr("PCs on the same network can start DiskAnvil Live from this one, with nothing installed on them: turn one on, open "
                      "its boot menu (F12, F11, F9 or Esc) and pick the network (it says PXE, IPv4 or the network card's name). They need "
                      "a cable to the same router; Secure Boot can stay on. Each one downloads about %1 and runs it from memory.\n\n"
                      "This PC answers only network starts: the router still gives out the addresses.")
                       .arg(formatSize(quint64(size))));
    m_cards = new QComboBox;
    m_cards->setObjectName(QStringLiteral("cards"));
    for (const netboot::Card &card : netboot::cards())
        m_cards->addItem(card.wireless ? tr("%1 (Wi-Fi), %2").arg(card.name, card.address) : tr("%1, %2").arg(card.name, card.address), card.name);
    auto *form = new QFormLayout;
    form->addRow(tr("Network:"), m_cards);
    layout->addLayout(form);
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("status"));
    m_status->setWordWrap(true);
    m_pcs = new QLabel;
    m_pcs->setObjectName(QStringLiteral("pcs"));
    m_pcs->setWordWrap(true);
    m_pcs->setTextFormat(Qt::RichText);
    layout->addWidget(m_status);
    layout->addWidget(m_pcs);
    m_start = buttons->addButton(tr("Start"), QDialogButtonBox::ActionRole);
    m_stop = buttons->addButton(tr("Stop"), QDialogButtonBox::ActionRole);
    m_stop->hide();
    connect(m_start, &QPushButton::clicked, this, &NetBootDialog::start);
    connect(m_stop, &QPushButton::clicked, this, &NetBootDialog::stop);
    layout->addWidget(buttons);
    if (medium.isEmpty()) {
        m_status->setText(tr("DiskAnvil Live's own files aren't here to hand over: it has to have started from a stick or a CD (not over the "
                             "network, and not copied to memory)."));
        m_start->setEnabled(false);
    } else if (m_cards->count() == 0) {
        m_status->setText(tr("This PC isn't connected to a network: plug in a cable (or join a Wi-Fi network), then open this again."));
        m_start->setEnabled(false);
    }
    m_refresh = new QTimer(this);
    m_refresh->setInterval(500);
    connect(m_refresh, &QTimer::timeout, this, &NetBootDialog::showPcs);
    resize(600, sizeHint().height());
}

NetBootDialog::~NetBootDialog()
{
    stop();
}

void NetBootDialog::done(int result)
{
    stop();
    QDialog::done(result);
}

void NetBootDialog::start()
{
    m_order.clear();
    m_state.clear();
    m_said.clear();
    m_server = new netboot::FileServer(netboot::medium(), this);
    if (!m_server->listen()) {
        delete m_server;
        m_server = nullptr;
        m_status->setText(tr("No network port was free for it (8099 to 8199)."));
        return;
    }
    connect(m_server, &netboot::FileServer::progress, this, [this](const QString &pc, quint64 done, quint64 total) {
        if (!m_order.contains(pc))
            m_order << pc;
        m_state[pc] = tr("loading DiskAnvil Live: %1 of %2").arg(formatSize(done), formatSize(total));
    });
    connect(m_server, &netboot::FileServer::finished, this, [this](const QString &pc, bool whole) {
        m_state[pc] = whole ? tr("has DiskAnvil Live, and is starting it") : tr("stopped loading it (turned off, or the cable came out?)");
        showPcs();
    });
    const QString card = m_cards->currentData().toString();
    const QStringList command = netboot::command(card, m_server->port());
    m_dnsmasq = new QProcess(this);
    m_dnsmasq->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_dnsmasq, &QProcess::readyRead, this, &NetBootDialog::readDnsmasq);
    connect(m_dnsmasq, &QProcess::finished, this, [this](int code) {
        if (!m_dnsmasq)
            return;
        readDnsmasq();
        qCInfo(lcOps).noquote() << "Start Other PCs: dnsmasq ended" << code << m_said;
        m_dnsmasq->deleteLater();
        m_dnsmasq = nullptr;
        stop();
        m_status->setText(redText(tr("It stopped: %1").arg(m_said.isEmpty() ? tr("no reason given") : m_said.right(400))));
        m_status->setTextFormat(Qt::RichText);
    });
    qCInfo(lcOps).noquote() << "Start Other PCs from This One:" << card << "http port" << m_server->port();
    m_dnsmasq->start(command.first(), command.mid(1));
    if (!m_dnsmasq->waitForStarted(5000)) {
        m_status->setText(tr("dnsmasq didn't start: %1").arg(m_dnsmasq->errorString()));
        m_dnsmasq->deleteLater();
        m_dnsmasq = nullptr;
        return stop();
    }
    m_status->setText(tr("Waiting for PCs: start them from the network now."));
    m_cards->setEnabled(false);
    m_start->hide();
    m_stop->show();
    m_refresh->start();
}

void NetBootDialog::stop()
{
    if (m_refresh)
        m_refresh->stop();
    if (QProcess *p = m_dnsmasq) {
        m_dnsmasq = nullptr;
        p->terminate(); // sudo hands it on to dnsmasq
        if (!p->waitForFinished(5000))
            p->kill();
        p->waitForFinished(1000);
        p->deleteLater();
        qCInfo(lcOps).noquote() << "Start Other PCs from This One: stopped";
    }
    delete m_server;
    m_server = nullptr;
    if (m_start) {
        m_cards->setEnabled(true);
        m_start->show();
        m_stop->hide();
        if (m_status->text().startsWith(tr("Waiting for PCs")))
            m_status->setText(tr("Stopped. PCs that have DiskAnvil Live already keep running it."));
    }
}

void NetBootDialog::readDnsmasq()
{
    if (!m_dnsmasq)
        return;
    for (const QString &line : QString::fromLocal8Bit(m_dnsmasq->readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        m_said = line.trimmed();
        QString file;
        if (const QString mac = netboot::askedBy(line); !mac.isEmpty()) {
            if (!m_order.contains(mac))
                m_order << mac;
            m_state[mac] = tr("asked to start from the network");
        } else if (const QString pc = netboot::sentTo(line, &file); !pc.isEmpty()) {
            if (!m_order.contains(pc))
                m_order << pc;
            m_state[pc] = file == QLatin1String("initrd.img") ? tr("has the start files, and is asking for the rest")
                                                              : tr("starting (%1 sent)").arg(file);
        }
    }
    showPcs();
}

void NetBootDialog::showPcs()
{
    if (!m_pcs)
        return;
    QStringList lines;
    for (const QString &pc : std::as_const(m_order))
        lines << QStringLiteral("<b>%1</b>: %2").arg(pc.toHtmlEscaped(), m_state.value(pc).toHtmlEscaped());
    m_pcs->setText(lines.join(QStringLiteral("<br>")));
}
