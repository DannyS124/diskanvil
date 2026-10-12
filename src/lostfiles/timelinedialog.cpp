// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timelinedialog.h"

#include "dialogs.h"
#include "format.h"
#include "pdfreport.h"

#include <QAbstractTableModel>
#include <QCheckBox>
#include <QDateEdit>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

class TimelineModel : public QAbstractTableModel
{
public:
    TimelineModel(const QVector<lost::Found> &files, QObject *parent)
        : QAbstractTableModel(parent)
        , m_files(files)
    {
    }

    void set(QVector<timeline::Event> events)
    {
        beginResetModel();
        m_events = std::move(events);
        endResetModel();
    }
    const QVector<timeline::Event> &events() const { return m_events; }

    int rowCount(const QModelIndex &parent) const override { return parent.isValid() ? 0 : int(m_events.size()); }
    int columnCount(const QModelIndex &parent) const override { return parent.isValid() ? 0 : 5; }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (role != Qt::DisplayRole || index.row() >= m_events.size())
            return {};
        const timeline::Event &e = m_events.at(index.row());
        const lost::Found &f = m_files.at(e.file);
        switch (index.column()) {
        case 0: {
            const QDateTime when = QDateTime::fromMSecsSinceEpoch(e.when);
            return e.dayOnly ? QLocale().toString(when.date(), QLocale::ShortFormat) : QLocale().toString(when, QLocale::ShortFormat);
        }
        case 1:
            return timeline::kindName(e.kind);
        case 2:
            return f.deleted ? QObject::tr("%1 (deleted)").arg(f.name) : f.name;
        case 3:
            return f.folder;
        default:
            return formatSize(f.size);
        }
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return {};
        const QStringList names = {QObject::tr("When"), QObject::tr("What happened"), QObject::tr("File"), QObject::tr("Folder"), QObject::tr("Size")};
        return names.value(section);
    }

private:
    const QVector<lost::Found> &m_files;
    QVector<timeline::Event> m_events;
};

TimelineDialog::TimelineDialog(const QVector<lost::Found> &files, const QString &title, QWidget *parent)
    : QDialog(parent)
    , m_files(files)
    , m_title(title)
    , m_view(new QTableView)
    , m_from(new QDateEdit)
    , m_to(new QDateEdit)
    , m_words(new QLineEdit)
    , m_deletedOnly(new QCheckBox(tr("Deleted files only")))
    , m_count(new QLabel)
{
    setWindowTitle(tr("Timeline of %1").arg(title));
    m_all = timeline::events(m_files, QDateTime::currentMSecsSinceEpoch());
    m_model = new TimelineModel(m_files, this);
    m_view->setModel(m_model);
    m_view->setObjectName(QStringLiteral("timeline"));
    m_view->verticalHeader()->hide();
    m_view->horizontalHeader()->setStretchLastSection(false);
    m_view->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setAlternatingRowColors(true);
    const QDate first = m_all.isEmpty() ? QDate::currentDate() : QDateTime::fromMSecsSinceEpoch(m_all.first().when).date();
    const QDate last = m_all.isEmpty() ? QDate::currentDate() : QDateTime::fromMSecsSinceEpoch(m_all.last().when).date();
    for (QDateEdit *d : {m_from, m_to}) {
        d->setCalendarPopup(true);
        d->setDateRange(first, last);
        connect(d, &QDateEdit::dateChanged, this, &TimelineDialog::filter);
    }
    m_from->setDate(first);
    m_to->setDate(last);
    m_from->setObjectName(QStringLiteral("from"));
    m_to->setObjectName(QStringLiteral("to"));
    m_words->setPlaceholderText(tr("Name or folder has…"));
    m_words->setClearButtonEnabled(true);
    m_words->setObjectName(QStringLiteral("words"));
    m_deletedOnly->setObjectName(QStringLiteral("deletedOnly"));
    m_count->setObjectName(QStringLiteral("count"));
    connect(m_words, &QLineEdit::textChanged, this, &TimelineDialog::filter);
    connect(m_deletedOnly, &QCheckBox::toggled, this, &TimelineDialog::filter);

    auto *csv = new QPushButton(tr("Save as CSV…"));
    connect(csv, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Save the Timeline"), QDir::home().filePath(tr("Timeline of %1.csv").arg(m_title)),
                                                          tr("CSV files (*.csv)"));
        QFile out(path);
        if (!path.isEmpty() && (!out.open(QIODevice::WriteOnly) || out.write(timeline::csv(visible(), m_files)) < 0))
            QMessageBox::warning(this, windowTitle(), out.errorString());
    });
    auto *pdf = new QPushButton(tr("Save as PDF…"));
    connect(pdf, &QPushButton::clicked, this, [this] {
        // A day's heading, then its events; the first 3,000 (a CSV holds them all).
        const QVector<timeline::Event> events = visible();
        QString html = QStringLiteral("<html><body style=\"font-family:sans-serif; font-size:9pt\"><h1 style=\"font-size:17pt\">%1</h1>")
                           .arg(tr("Timeline of %1").arg(m_title).toHtmlEscaped());
        QDate day;
        bool open = false;
        for (qsizetype i = 0; i < events.size() && i < 3000; ++i) {
            const timeline::Event &e = events.at(i);
            const QDateTime when = QDateTime::fromMSecsSinceEpoch(e.when);
            if (when.date() != day) {
                day = when.date();
                html += (open ? QStringLiteral("</table>") : QString())
                      + QStringLiteral("<h2 style=\"font-size:11pt\">%1</h2><table>").arg(QLocale().toString(day, QLocale::LongFormat).toHtmlEscaped());
                open = true;
            }
            const lost::Found &f = m_files.at(e.file);
            html += QStringLiteral("<tr><td style=\"padding:1px 10px 1px 0\">%1</td><td style=\"padding:1px 10px 1px 0\">%2</td><td>%3</td></tr>")
                        .arg(e.dayOnly ? QString() : when.time().toString(QStringLiteral("HH:mm:ss")), timeline::kindName(e.kind).toHtmlEscaped(),
                             (f.folder.isEmpty() ? f.name : f.folder + QLatin1Char('/') + f.name).toHtmlEscaped() + (f.deleted ? tr(" (deleted)") : QString()));
        }
        html += (open ? QStringLiteral("</table>") : QString());
        if (events.size() > 3000)
            html += QStringLiteral("<p>%1</p>").arg(tr("…and %1 more: the CSV has them all.").arg(events.size() - 3000).toHtmlEscaped());
        html += QStringLiteral("</body></html>");
        pdfreport::saveAs(this, html, tr("Timeline of %1.pdf").arg(m_title), windowTitle());
    });
    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    auto *filters = new QHBoxLayout;
    filters->addWidget(new QLabel(tr("From")));
    filters->addWidget(m_from);
    filters->addWidget(new QLabel(tr("to")));
    filters->addWidget(m_to);
    filters->addWidget(m_words, 1);
    filters->addWidget(m_deletedOnly);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_count);
    buttons->addStretch();
    buttons->addWidget(csv);
    buttons->addWidget(pdf);
    buttons->addWidget(close);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("Everything that happened to the files found, as far as the drive kept it, oldest first: when each "
                                       "was made, written, opened, renamed and deleted, and the dates photos and emails carry.")));
    layout->addLayout(filters);
    layout->addWidget(m_view, 1);
    layout->addLayout(buttons);
    resize(980, 640);
    filter();
}

QVector<timeline::Event> TimelineDialog::visible() const
{
    const qint64 from = m_from->date().startOfDay().toMSecsSinceEpoch(), to = m_to->date().addDays(1).startOfDay().toMSecsSinceEpoch();
    const QString words = m_words->text().trimmed();
    QVector<timeline::Event> out;
    for (const timeline::Event &e : m_all) {
        if (e.when < from || e.when >= to)
            continue;
        const lost::Found &f = m_files.at(e.file);
        if (m_deletedOnly->isChecked() && !f.deleted)
            continue;
        if (!words.isEmpty() && !f.name.contains(words, Qt::CaseInsensitive) && !f.folder.contains(words, Qt::CaseInsensitive))
            continue;
        out << e;
    }
    return out;
}

void TimelineDialog::filter()
{
    m_model->set(visible());
    m_view->resizeColumnsToContents();
    m_count->setText(tr("%n event(s)", nullptr, shown()));
}

int TimelineDialog::shown() const
{
    return int(m_model->events().size());
}
