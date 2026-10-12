// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "activitydialog.h"

#include "dialogs.h"
#include "pdfreport.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

ActivityDialog::ActivityDialog(const QString &root, QWidget *parent)
    : QDialog(parent)
    , m_root(root)
    , m_overview(new QLabel)
    , m_kind(new QComboBox)
    , m_words(new QLineEdit)
    , m_list(new QTreeWidget)
    , m_status(new QLabel)
    , m_save(new QPushButton(tr("Save a Report…")))
{
    const QString where = root.isEmpty() ? tr("this PC") : QDir::toNativeSeparators(root);
    setWindowTitle(tr("What This PC Did — %1").arg(where));
    m_overview->setTextFormat(Qt::RichText);
    m_overview->setWordWrap(false); // a table: wrapping makes Qt measure it a line tall
    m_overview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_kind->setObjectName(QStringLiteral("kind"));
    m_kind->addItem(tr("Everything"), -1);
    for (const activity::Kind k : {activity::Kind::UsbDevice, activity::Kind::ProgramRun, activity::Kind::FileOpened, activity::Kind::FolderOpened,
                                   activity::Kind::Search, activity::Kind::Typed, activity::Kind::Website, activity::Kind::Download,
                                   activity::Kind::Network, activity::Kind::Login, activity::Kind::Software, activity::Kind::Account})
        m_kind->addItem(activity::kindName(k), int(k));
    m_words->setObjectName(QStringLiteral("words"));
    m_words->setPlaceholderText(tr("Words to look for"));
    m_words->setClearButtonEnabled(true);
    m_list->setObjectName(QStringLiteral("events"));
    m_list->setHeaderLabels({tr("When"), tr("What"), tr("More"), tr("Whose"), tr("Read from")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_status->setWordWrap(true);
    m_save->setObjectName(QStringLiteral("save"));
    m_save->setEnabled(false);
    connect(m_kind, &QComboBox::currentIndexChanged, this, &ActivityDialog::fill);
    connect(m_words, &QLineEdit::textChanged, this, &ActivityDialog::fill);
    connect(m_save, &QPushButton::clicked, this, &ActivityDialog::saveReport);

    auto *filters = new QHBoxLayout;
    filters->addWidget(new QLabel(tr("Show:")));
    filters->addWidget(m_kind);
    filters->addWidget(m_words, 1);
    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_status, 1);
    buttons->addWidget(m_save);
    buttons->addWidget(close);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("What system this is, and what it was used for, read from its own records without starting it: on "
                                       "Windows, USB drives plugged in, programs run, files and folders opened, searches, websites and "
                                       "downloads, networks joined and its accounts; on Linux, its accounts, sign-ins, programs installed, "
                                       "recent files and websites. Nothing on the drive is changed.")));
    layout->addWidget(m_overview);
    layout->addLayout(filters);
    layout->addWidget(m_list, 1);
    layout->addLayout(buttons);
    resize(1080, 640);

    m_status->setText(tr("Reading Windows' records…"));
    m_thread = QThread::create([this, root] {
        const activity::Report report = activity::read(root, &m_stop);
        QMetaObject::invokeMethod(this, [this, report] {
            m_report = report;
            fill();
        }, Qt::QueuedConnection);
    });
    m_thread->start();
}

ActivityDialog::~ActivityDialog()
{
    m_stop = true;
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
}

void ActivityDialog::waitUntilRead()
{
    while (m_thread && !m_thread->isFinished())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QCoreApplication::processEvents();
}

int ActivityDialog::shown() const
{
    return m_list->topLevelItemCount();
}

QVector<activity::Event> ActivityDialog::visible() const
{
    QVector<activity::Event> out;
    const int kind = m_kind->currentData().toInt();
    const QStringList words = m_words->text().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const activity::Event &e : m_report.events) {
        if (kind >= 0 && int(e.kind) != kind)
            continue;
        const QString all = e.what + QLatin1Char(' ') + e.detail + QLatin1Char(' ') + e.user;
        if (std::all_of(words.cbegin(), words.cend(), [&all](const QString &w) { return all.contains(w, Qt::CaseInsensitive); }))
            out << e;
    }
    return out;
}

void ActivityDialog::fill()
{
    if (m_report.system.isEmpty()) {
        if (m_thread && m_thread->isFinished())
            m_status->setText(redText(tr("No Windows, Linux or Mac system was found there.")));
        return;
    }
    const activity::Overview &o = m_report.overview;
    const auto when = [](const QDateTime &t) { return t.isValid() ? QLocale().toString(t.toLocalTime(), QLocale::ShortFormat) : QString(); };
    QStringList rows;
    const auto row = [&rows](const QString &label, const QString &value) {
        if (!value.trimmed().isEmpty())
            rows << QStringLiteral("<tr><td style=\"padding-right:14px\"><b>%1</b></td><td>%2</td></tr>").arg(label.toHtmlEscaped(), value.toHtmlEscaped());
    };
    row(tr("System"), QStringList{o.product, o.product.contains(o.release) ? QString() : o.release,
                                  o.build.isEmpty() ? QString() : QStringLiteral("(%1)").arg(o.build.contains(QLatin1Char(' ')) ? o.build : tr("build %1").arg(o.build))}
                          .join(QLatin1Char(' '))
                          .simplified());
    row(tr("PC's name"), o.computerName);
    QStringList owner{o.owner.trimmed(), o.organisation.trimmed()};
    owner.removeAll(QString());
    row(tr("Registered to"), owner.join(QStringLiteral(", ")));
    row(tr("Installed"), when(o.installed));
    row(tr("Last shut down"), when(o.lastShutdown));
    row(tr("Time zone"), o.timeZone);
    m_overview->setText(QStringLiteral("<table>") + rows.join(QString()) + QStringLiteral("</table>"));

    const QVector<activity::Event> events = visible();
    m_list->setUpdatesEnabled(false);
    m_list->clear();
    QList<QTreeWidgetItem *> items;
    items.reserve(events.size());
    for (const activity::Event &e : events) {
        auto *item = new QTreeWidgetItem({when(e.when), e.what, e.detail, e.user, e.source});
        item->setToolTip(1, activity::kindName(e.kind));
        item->setToolTip(2, e.detail);
        items << item;
    }
    m_list->addTopLevelItems(items);
    for (int c = 0; c < 4; ++c)
        m_list->resizeColumnToContents(c);
    m_list->setColumnWidth(1, std::min(m_list->columnWidth(1), 380));
    m_list->setColumnWidth(2, std::min(m_list->columnWidth(2), 360));
    m_list->setUpdatesEnabled(true);
    m_save->setEnabled(!events.isEmpty());
    QString status = tr("%L1 of %L2 shown.").arg(events.size()).arg(m_report.events.size());
    if (!m_report.notes.isEmpty())
        status += QLatin1Char(' ') + m_report.notes.join(QLatin1Char(' '));
    m_status->setText(status);
}

void ActivityDialog::saveReport()
{
    QString filter;
    const QString suggested = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QLatin1Char('/')
                            + tr("What this PC did %1").arg(QDate::currentDate().toString(Qt::ISODate));
    QString path = QFileDialog::getSaveFileName(this, tr("Save the Report"), suggested + QStringLiteral(".html"),
                                                tr("Web page (*.html);;Spreadsheet (*.csv);;PDF (*.pdf)"), &filter);
    if (path.isEmpty())
        return;
    const QString kind = filter.contains(QLatin1String("*.csv")) ? QStringLiteral("csv") : filter.contains(QLatin1String("*.pdf")) ? QStringLiteral("pdf")
                                                                                                                                       : QStringLiteral("html");
    if (QFileInfo(path).suffix().isEmpty())
        path += QLatin1Char('.') + kind;
    const QString where = m_report.overview.computerName.isEmpty() ? (m_root.isEmpty() ? tr("this PC") : m_root) : m_report.overview.computerName;
    QString error;
    bool ok = false;
    if (kind == QLatin1String("pdf")) {
        ok = pdfreport::save(activity::html(m_report, visible(), where), path, tr("What this PC did"), &error);
    } else {
        QSaveFile out(path);
        ok = out.open(QIODevice::WriteOnly)
          && out.write(kind == QLatin1String("csv") ? activity::csv(visible()) : activity::html(m_report, visible(), where).toUtf8()) > 0 && out.commit();
        error = out.errorString();
    }
    if (ok)
        m_status->setText(tr("The report is saved: %1").arg(QDir::toNativeSeparators(path)));
    else
        QMessageBox::warning(this, windowTitle(), tr("The report couldn't be saved: %1").arg(error));
}
