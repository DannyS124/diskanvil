// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "driversdialog.h"

#include "applog.h"
#include "authenticode.h"
#include "dialogs.h"
#include "peinfo.h"
#include "wof.h"

#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

DriversDialog::DriversDialog(const QString &root, QWidget *parent)
    : QDialog(parent)
    , m_root(root)
    , m_list(new QTreeWidget)
    , m_hideWindows(new QCheckBox(tr("Hide Windows' own")))
    , m_status(new QLabel)
    , m_switchOff(new QPushButton(tr("Switch Off…")))
{
    setWindowTitle(root.isEmpty() ? tr("Windows Drivers and Services") : tr("Drivers and Services of the Windows on %1").arg(QDir::toNativeSeparators(root)));
    m_list->setHeaderLabels({tr("Name"), tr("Made by"), tr("Changed"), tr("Kind"), tr("File")});
    m_list->setRootIsDecorated(false);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setObjectName(QStringLiteral("drivers"));
    m_hideWindows->setChecked(true);
    m_status->setWordWrap(true);
    m_switchOff->setEnabled(false);
    m_switchOff->setObjectName(QStringLiteral("switchOff"));
    connect(m_hideWindows, &QCheckBox::toggled, this, &DriversDialog::fill);
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, [this] { m_switchOff->setEnabled(!m_list->selectedItems().isEmpty()); });
    connect(m_switchOff, &QPushButton::clicked, this, &DriversDialog::switchOff);
    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_hideWindows);
    buttons->addStretch();
    buttons->addWidget(m_switchOff);
    buttons->addWidget(close);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("What starts with this Windows, the most recently changed first. If Windows stopped starting "
                                       "(a blue screen, or it restarts over and over) just after something was installed, the newest "
                                       "driver from a maker is the usual cause: switch it off, and if Windows starts again, update or "
                                       "remove that program.")));
    layout->addWidget(m_list, 1);
    layout->addWidget(m_status);
    layout->addLayout(buttons);
    resize(900, 560);
    load();
}

DriversDialog::~DriversDialog()
{
    if (m_thread)
        m_thread->wait();
}

void DriversDialog::load()
{
    m_status->setText(tr("Reading what starts with Windows…"));
    const QString root = m_root;
    m_thread = QThread::create([this, root] {
#ifdef Q_OS_WIN
        startup::Report report = root.isEmpty() ? startup::findRunning() : startup::find(root);
#else
        startup::Report report = startup::find(root);
#endif
        QVector<Row> rows;
        for (int i = 0; i < report.entries.size(); ++i) {
            const startup::Entry &e = report.entries.at(i);
            if (e.kind != startup::Kind::Driver && e.kind != startup::Kind::Service)
                continue;
            Row r;
            r.entry = i;
            QFile file(e.file);
            if (!e.file.isEmpty() && file.size() < 128 * 1024 * 1024 && file.open(QIODevice::ReadOnly)) {
                const QByteArray data = file.readAll();
                r.changed = QFileInfo(e.file).lastModified();
                const QHash<QString, QString> v = peinfo::versionStrings(data);
                r.maker = v.value(QStringLiteral("CompanyName"));
                r.description = v.value(QStringLiteral("FileDescription"));
                const authenticode::Result signature = authenticode::check(data, {});
                if (signature.state == authenticode::Result::State::Signed)
                    r.signer = signature.signer;
                // Windows' own: Microsoft's company name, or signed as Windows itself (drivers a
                // maker had Microsoft test are signed by its "Hardware Compatibility Publisher").
                r.windowsOwn = r.maker.startsWith(QLatin1String("Microsoft"))
                            || (r.signer.startsWith(QLatin1String("Microsoft Windows")) && !r.signer.contains(QLatin1String("Hardware Compatibility")));
                // Kept compressed by Windows (CompactOS), which only does that to its own files:
                // Linux can't read them, but whose they are is known.
                if (data.isEmpty() && file.size() > 0 && wof::compressed(e.file)) {
                    r.maker = tr("Windows (kept compressed)");
                    r.windowsOwn = true;
                }
                if (r.maker.isEmpty())
                    r.maker = r.signer;
            }
            rows << r;
        }
        std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.changed > b.changed; });
        QMetaObject::invokeMethod(this, [this, report, rows] {
            m_thread->wait();
            delete m_thread;
            m_thread = nullptr;
            m_report = report;
            m_rows = rows;
            fill();
        });
    });
    m_thread->setParent(this); // gone with the window, if it's closed first
    m_thread->start();
}

void DriversDialog::fill()
{
    m_list->clear();
    int hidden = 0;
    for (const Row &r : std::as_const(m_rows)) {
        if (m_hideWindows->isChecked() && r.windowsOwn) {
            ++hidden;
            continue;
        }
        const startup::Entry &e = m_report.entries.at(r.entry);
        QString name = e.name;
        if (m_done.contains(r.entry))
            name += QStringLiteral(" (") + m_done.value(r.entry) + QLatin1Char(')');
        auto *item = new QTreeWidgetItem(m_list, {name, r.maker.isEmpty() ? tr("Unknown") : r.maker,
                                                  r.changed.isValid() ? QLocale().toString(r.changed, QLocale::ShortFormat) : tr("File missing"),
                                                  e.kind == startup::Kind::Driver ? tr("Driver") : tr("Service"), e.program});
        item->setData(0, Qt::UserRole, r.entry);
        QStringList tip;
        if (!r.description.isEmpty())
            tip << r.description;
        tip << (r.signer.isEmpty() ? tr("No signature in the file itself (Windows' own are often signed in its catalog instead)")
                                   : tr("Signed by %1").arg(r.signer));
        tip << e.command;
        for (int c = 0; c < item->columnCount(); ++c)
            item->setToolTip(c, tip.join(QLatin1Char('\n')));
        if (m_done.contains(r.entry))
            item->setDisabled(true);
    }
    for (int c = 0; c < m_list->columnCount() - 1; ++c)
        m_list->resizeColumnToContents(c);
    if (!m_report.windows)
        m_status->setText(redText(tr("There's no Windows there.")));
    else
        m_status->setText(tr("%n driver(s) and service(s)", nullptr, int(m_rows.size())) + (hidden ? QStringLiteral(", ") + tr("%n of Windows' own hidden", nullptr, hidden) : QString())
                          + QStringLiteral(".") + (m_report.notes.isEmpty() ? QString() : QLatin1Char(' ') + m_report.notes.join(QLatin1Char(' '))));
}

void DriversDialog::switchOff()
{
    QVector<int> picked;
    QStringList names;
    bool windowsOwn = false;
    for (QTreeWidgetItem *item : m_list->selectedItems()) {
        const int entry = item->data(0, Qt::UserRole).toInt();
        if (m_done.contains(entry) || !startup::canSwitchOff(m_report.entries.at(entry)))
            continue;
        picked << entry;
        names << QStringLiteral("• ") + m_report.entries.at(entry).name;
        for (const Row &r : std::as_const(m_rows))
            windowsOwn = windowsOwn || (r.entry == entry && r.windowsOwn);
    }
    if (picked.isEmpty())
        return;
    QString question = tr("Switch these off, so they don't start with Windows?") + QStringLiteral("\n\n") + names.join(QLatin1Char('\n'))
                     + QStringLiteral("\n\n")
                     + tr("Everything changed is copied to a \"DiskAnvil backups\" folder on that drive first, with a note saying how to "
                          "put it back.");
    if (windowsOwn)
        question += QStringLiteral("\n\n") + tr("Some of these are part of Windows: switching them off can stop Windows starting at all.");
    if (QMessageBox::question(this, windowTitle(), question, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    QString drive = m_root;
#ifdef Q_OS_WIN
    if (drive.isEmpty())
        drive = QDir::fromNativeSeparators(qEnvironmentVariable("SystemDrive")) + QLatin1Char('/');
#endif
    if (drive.endsWith(QLatin1Char('/')))
        drive.chop(1);
    const QString backup = drive + QStringLiteral("/DiskAnvil backups/") + QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH.mm.ss"));
    QVector<startup::Entry> entries;
    for (int entry : std::as_const(picked))
        entries << m_report.entries.at(entry);
    const QVector<startup::Change> changes = startup::switchOff(entries, backup);
    int done = 0;
    QStringList failed;
    for (int n = 0; n < changes.size(); ++n) {
        qCInfo(lcOps).noquote() << "Switch off" << entries[n].name << (changes[n].done ? "done:" : "not done:") << changes[n].message;
        if (changes[n].done) {
            ++done;
            m_done.insert(picked[n], tr("switched off"));
        } else {
            failed << entries[n].name + QStringLiteral(": ") + changes[n].message;
        }
    }
    fill();
    const QString where = QDir::toNativeSeparators(backup);
    if (failed.isEmpty())
        QMessageBox::information(this, windowTitle(), tr("Switched off %n. The backup, and how to undo it, is in %1.", nullptr, done).arg(where));
    else
        QMessageBox::warning(this, windowTitle(), tr("Switched off %1 of %2. Not these:\n%3\n\nThe backup is in %4.")
                                                      .arg(done)
                                                      .arg(changes.size())
                                                      .arg(failed.join(QLatin1Char('\n')), where));
}
