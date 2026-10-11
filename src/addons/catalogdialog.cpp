// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "catalogdialog.h"

#include "addonprompt.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QVBoxLayout>

CatalogDialog::CatalogDialog(Addons *addons, QWidget *parent)
    : QDialog(parent)
    , m_addons(addons)
    , m_net(new QNetworkAccessManager(this))
    , m_list(new QTreeWidget)
    , m_details(new QTextBrowser)
    , m_status(new QLabel)
    , m_install(new QPushButton(QIcon::fromTheme(QStringLiteral("download")), tr("Install")))
{
    setWindowTitle(tr("Get Add-ons"));
    m_list->setHeaderLabels({tr("Add-on"), tr("Version"), tr("Author"), tr("")});
    m_list->setRootIsDecorated(false);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_details->setOpenLinks(false);
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_install->setEnabled(false);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_list, 2);
    layout->addWidget(m_details, 2);
    layout->addWidget(m_status);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    box->addButton(m_install, QDialogButtonBox::ActionRole);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_install, &QPushButton::clicked, this, &CatalogDialog::install);
    connect(m_list, &QTreeWidget::currentItemChanged, this, &CatalogDialog::showDetails);
    layout->addWidget(box);
    resize(700, 520);
    load();
}

void CatalogDialog::fetch(const QString &url, const std::function<void(const QByteArray &, const QString &)> &done)
{
    QNetworkRequest request{QUrl(url)};
    // A redirect could lead anywhere; the URLs are exact, so none is expected.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("DiskForge/" APP_VERSION));
    request.setTransferTimeout(20000);
    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64) {
        if (received > Addons::kMaxDownload)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [reply, done] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 404)
            done({}, tr("it isn't online yet"));
        else if (reply->error() == QNetworkReply::OperationCanceledError)
            done({}, tr("The download was bigger than it can be"));
        else if (reply->error() != QNetworkReply::NoError)
            done({}, reply->errorString());
        else if (status != 200)
            done({}, tr("the server answered %1").arg(status));
        else
            done(reply->readAll(), {});
    });
}

void CatalogDialog::load()
{
    m_status->setText(tr("Getting the list from %1…").arg(Addons::catalogUrl()));
    fetch(Addons::catalogUrl(), [this](const QByteArray &data, const QString &error) {
        if (!error.isEmpty()) {
            m_status->setText(tr("Couldn't get the list: %1").arg(error));
            return;
        }
        // The list is only used if it's signed with the maintainer's key.
        fetch(Addons::catalogSignatureUrl(), [this, data](const QByteArray &sig, const QString &error) {
            if (!error.isEmpty()) {
                m_status->setText(tr("Couldn't get the list's signature: %1").arg(error));
                return;
            }
            QString problem;
            m_entries = Addons::parseSignedCatalog(data, sig, Addons::catalogKeys(), &problem);
            m_status->setText(problem);
            fill();
        });
    });
}

void CatalogDialog::fill()
{
    m_list->clear();
    for (const CatalogEntry &e : m_entries) {
        QString state;
        for (const Addon &a : m_addons->all()) {
            if (a.id == e.id)
                state = a.version == e.version ? tr("Installed") : tr("Update");
        }
        new QTreeWidgetItem(m_list, {e.name, e.version, e.author, state});
    }
    for (int c = 1; c < 4; ++c)
        m_list->resizeColumnToContents(c);
    if (m_list->topLevelItemCount() > 0)
        m_list->setCurrentItem(m_list->topLevelItem(0));
    else
        m_details->setHtml(tr("<p>The list is empty.</p>"));
}

const CatalogEntry *CatalogDialog::current() const
{
    const int row = m_list->indexOfTopLevelItem(m_list->currentItem());
    return row >= 0 && row < m_entries.size() ? &m_entries[row] : nullptr;
}

void CatalogDialog::showDetails()
{
    const CatalogEntry *e = current();
    m_install->setEnabled(e != nullptr);
    if (!e)
        return;
    const QString state = m_list->currentItem()->text(3);
    m_install->setText(state == tr("Update") ? tr("Update") : state == tr("Installed") ? tr("Install Again") : tr("Install"));
    m_details->setHtml(QStringLiteral("<h3>%1</h3><p>%2</p><p><small>%3<br>%4</small></p>")
                           .arg(e->name.toHtmlEscaped(), e->description.toHtmlEscaped(),
                                tr("Version %1 by %2").arg(e->version.toHtmlEscaped(), e->author.toHtmlEscaped()),
                                e->url.toHtmlEscaped()));
}

void CatalogDialog::install()
{
    const CatalogEntry *e = current();
    if (!e)
        return;
    const CatalogEntry entry = *e;
    m_install->setEnabled(false);
    m_status->setText(tr("Downloading %1…").arg(entry.name));
    fetch(entry.url, [this, entry](const QByteArray &data, const QString &error) {
        m_install->setEnabled(true);
        if (!error.isEmpty()) {
            m_status->setText(tr("Couldn't download it: %1").arg(error));
            return;
        }
        // Same question as installing from a file: what does it run?
        const Addon a = Addons::parseData(data, QString());
        if (a.error.isEmpty() && !askInstallAddon(this, a)) {
            m_status->clear();
            return;
        }
        QString problem;
        if (!Addons::installVerified(data, entry, &problem)) {
            m_status->setText(tr("Not installed: %1").arg(problem));
            return;
        }
        m_addons->load();
        m_status->setText(tr("Installed %1. Each of its actions asks once before it first runs.").arg(entry.name));
        const int row = m_list->indexOfTopLevelItem(m_list->currentItem());
        fill();
        m_list->setCurrentItem(m_list->topLevelItem(row));
    });
}
