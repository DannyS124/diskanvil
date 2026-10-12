// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "about.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QSysInfo>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {

QLabel *richLabel(const QString &html)
{
    auto *label = new QLabel(html);
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    label->setOpenExternalLinks(true);
    label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    label->setContentsMargins(12, 12, 12, 12);
    return label;
}

QString link(const QString &url, const QString &text = {})
{
    return QStringLiteral("<a href=\"%1\">%2</a>").arg(url, text.isEmpty() ? url : text);
}

QString resourceText(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

} // namespace

AboutDialog::AboutDialog(const QString &udisksVersion, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("About DiskAnvil"));
    const QString homepage = QStringLiteral(APP_HOMEPAGE);

    auto *icon = new QLabel;
    icon->setPixmap(QApplication::windowIcon().pixmap(64, 64));
    auto *title = new QLabel(tr("<span style=\"font-size:18pt; font-weight:bold\">DiskAnvil</span><br>"
                                "Version %1<br>Disk manager and partition editor")
                                 .arg(QApplication::applicationVersion()));
    auto *header = new QHBoxLayout;
    header->addWidget(icon);
    header->addSpacing(8);
    header->addWidget(title, 1);

    auto *tabs = new QTabWidget;
    tabs->addTab(richLabel(tr("<p>A disk manager for Linux and Windows that works like Windows Disk Management, and a rescue "
                              "system on a USB stick, DiskAnvil Live.</p>"
                              "<p>© 2026 %1<br>Released under the GNU General Public License, version 3 or later. "
                              "This program comes with absolutely no warranty.</p>"
                              "<p>%2<br>%3</p>"
                              "<p>DiskAnvil is free. If it helped you and you'd like to say thanks: %4</p>")
                               .arg(QStringLiteral(APP_AUTHOR), link(homepage, tr("Project website")),
                                    link(homepage + QStringLiteral("/issues"), tr("Report a bug")),
                                    link(QStringLiteral(APP_SUPPORT_URL), tr("buy me a coffee on Ko-fi")))),
                 tr("&About"));
    tabs->addTab(richLabel(tr("<p><b>%1</b><br>%2</p>")
                               .arg(QStringLiteral(APP_AUTHOR),
                                    link(QStringLiteral(APP_AUTHOR_URL)))),
                 tr("A&uthor"));
    tabs->addTab(richLabel(tr("<p>Drive failure rates in Disk Health come from %1: Backblaze's published records of the drives in "
                              "its data centres, free to use when they're named as the source.</p>"
                              "<p>DiskAnvil runs these as separate programs when they're there: ClamAV, smartmontools, "
                              "efibootmgr, QEMU, dnsmasq, x11vnc, OpenSSH, sqlite3, wimlib, cryptsetup, btrfs-progs, mdadm, "
                              "LVM and ldmtool. DiskAnvil Live is built from Debian.</p>")
                               .arg(link(QStringLiteral("https://www.backblaze.com/cloud-storage/resources/hard-drive-test-data"),
                                         tr("Backblaze Drive Stats")))),
                 tr("&Thanks"));

    auto *license = new QTextBrowser;
    license->setPlainText(resourceText(QStringLiteral(":/LICENSE")));
    tabs->addTab(license, tr("&License"));

    tabs->addTab(richLabel(tr("<p>Qt %1<br>UDisks2 %2<br>%3</p>")
                               .arg(QString::fromLatin1(qVersion()),
                                    udisksVersion.isEmpty() ? tr("(not running)") : udisksVersion,
                                    QSysInfo::prettyProductName().toHtmlEscaped())),
                 tr("&Components"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(header);
    layout->addWidget(tabs, 1);
    layout->addWidget(buttons);
    resize(520, 420);
}

HelpWindow::HelpWindow(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("DiskAnvil Help"));
    auto *browser = new QTextBrowser;
    browser->setOpenExternalLinks(true);
    browser->setHtml(resourceText(QStringLiteral(":/data/help.html")));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(browser, 1);
    layout->addWidget(buttons);
    resize(760, 680);
}
