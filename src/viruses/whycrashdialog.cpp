// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "whycrashdialog.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QTextBrowser>
#include <QThreadPool>
#include <QTreeWidget>
#include <QVBoxLayout>

WhyCrashDialog::WhyCrashDialog(const QString &root, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Why Did It Crash?"));
    resize(900, 640);
    auto *layout = new QVBoxLayout(this);
    m_summary = new QLabel(tr("Reading Windows' event log, its crash dumps and its error reports…"));
    m_summary->setObjectName(QStringLiteral("summary"));
    m_summary->setWordWrap(true);
    m_summary->setTextFormat(Qt::RichText);
    layout->addWidget(m_summary);
    auto *split = new QSplitter(Qt::Vertical);
    m_list = new QTreeWidget;
    m_list->setObjectName(QStringLiteral("incidents"));
    m_list->setHeaderLabels({tr("When"), tr("What happened"), tr("Driver")});
    m_list->setRootIsDecorated(false);
    m_list->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_detail = new QTextBrowser;
    m_detail->setObjectName(QStringLiteral("detail"));
    split->addWidget(m_list);
    split->addWidget(m_detail);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    layout->addWidget(split, 1);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    m_save = box->addButton(tr("Save Report…"), QDialogButtonBox::ActionRole);
    m_save->setEnabled(false);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_save, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Save the Report"), QDir::homePath() + QStringLiteral("/Why it crashed.txt"), tr("Text (*.txt)"));
        if (path.isEmpty())
            return;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(whycrash::asText(m_report).toUtf8()) < 0)
            QMessageBox::warning(this, windowTitle(), tr("Couldn't save it: %1").arg(f.errorString()));
    });
    connect(m_list, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (item)
            showIncident(item->data(0, Qt::UserRole).toInt());
    });
    layout->addWidget(box);
    if (root == QLatin1String("-"))
        return; // the tests give it a report
    QPointer<WhyCrashDialog> self(this);
    const auto cancel = m_cancel;
    QThreadPool::globalInstance()->start([self, root, cancel] {
        const whycrash::Report report = whycrash::read(root, cancel.get());
        QMetaObject::invokeMethod(qApp, [self, report] {
            if (self)
                self->showReport(report);
        }, Qt::QueuedConnection);
    });
}

WhyCrashDialog::~WhyCrashDialog()
{
    *m_cancel = true;
}

void WhyCrashDialog::showReport(const whycrash::Report &report)
{
    m_report = report;
    m_list->clear();
    QStringList top;
    for (const QString &s : report.summary)
        top << s.toHtmlEscaped();
    for (const QString &n : report.notes)
        top << QStringLiteral("<i>%1</i>").arg(n.toHtmlEscaped());
    if (report.from.isValid())
        top << QStringLiteral("<i>%1</i>").arg(tr("Windows' event log goes back to %1.").arg(QLocale().toString(report.from.toLocalTime(), QLocale::ShortFormat)).toHtmlEscaped());
    m_summary->setText(top.join(QStringLiteral("<br>")));
    for (int i = 0; i < report.incidents.size(); ++i) {
        const whycrash::Incident &inc = report.incidents[i];
        auto *item = new QTreeWidgetItem({inc.when.isValid() ? QLocale().toString(inc.when.toLocalTime(), QLocale::ShortFormat) : tr("Not known"), inc.title, inc.driver});
        item->setData(0, Qt::UserRole, i);
        m_list->addTopLevelItem(item);
    }
    m_list->resizeColumnToContents(0);
    if (m_list->topLevelItemCount() > 0)
        m_list->setCurrentItem(m_list->topLevelItem(0));
    else
        m_detail->setPlainText(tr("Nothing to show: no crashes or sudden stops were found."));
    m_save->setEnabled(true);
    emit shown();
}

void WhyCrashDialog::showIncident(int index)
{
    if (index < 0 || index >= m_report.incidents.size())
        return;
    const whycrash::Incident &inc = m_report.incidents[index];
    QString html = QStringLiteral("<h3>%1</h3><p>%2</p>").arg(inc.title.toHtmlEscaped(), inc.explanation.toHtmlEscaped());
    if (!inc.blame.isEmpty())
        html += QStringLiteral("<p><b>%1</b></p>").arg(inc.blame.toHtmlEscaped());
    if (!inc.before.isEmpty()) {
        html += QStringLiteral("<p>%1</p><ul>").arg(tr("Logged just before it:").toHtmlEscaped());
        for (const QString &b : inc.before)
            html += QStringLiteral("<li>%1</li>").arg(b.toHtmlEscaped());
        html += QStringLiteral("</ul>");
    }
    html += QStringLiteral("<p><small>%1</small></p>").arg(tr("Read from: %1").arg(inc.sources.join(QStringLiteral("; "))).toHtmlEscaped());
    m_detail->setHtml(html);
}
