// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Optimize Drives and Disk Cleanup on Windows, with Windows' own tools. Optimize-Volume does
// what each drive needs (TRIM for SSDs, defragmenting for hard drives), and Windows already
// runs it every week from a task of its own. Cleanup empties the temporary folders, the
// Recycle Bin, old error reports and Delivery Optimization's downloads.

#include "systemtools.h"

#include "cleanup.h"
#include "dialogs.h"
#include "format.h"
#include "jobui.h"
#include "win/winutil.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QGridLayout>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <atomic>

#include <shellapi.h>

namespace {

const QString kTask = QStringLiteral("-TaskPath '\\Microsoft\\Windows\\Defrag\\' -TaskName 'ScheduledDefrag'");

QTreeWidget *plainList(const QStringList &headers)
{
    auto *list = new QTreeWidget;
    list->setHeaderLabels(headers);
    list->setRootIsDecorated(false);
    list->setAlternatingRowColors(true);
    list->setSelectionMode(QAbstractItemView::NoSelection);
    return list;
}

// What Optimize-Volume does to a drive, or why it's left alone.
bool optimizable(const Disk &d)
{
    return !d.isLoop && !d.removable && !d.readOnly && (d.discard || d.rotationRate != 0);
}

QString whatItDoes(const Disk &d)
{
    if (d.removable)
        return QCoreApplication::translate("OptimizeDialog", "USB stick: Windows leaves these alone");
    if (d.readOnly)
        return QCoreApplication::translate("OptimizeDialog", "Read-only");
    if (d.discard)
        return QCoreApplication::translate("OptimizeDialog", "SSD: told which space is free (TRIM)");
    if (d.rotationRate == 0)
        return QCoreApplication::translate("OptimizeDialog", "SSD without TRIM: nothing to do");
    if (d.rotationRate > 0)
        return QCoreApplication::translate("OptimizeDialog", "Hard drive: defragmented when it needs it");
    return QCoreApplication::translate("OptimizeDialog", "Optimized the way Windows picks for it");
}

QString windowsFolder(const char *variable, const QString &fallback, const QString &below)
{
    return QDir::fromNativeSeparators(qEnvironmentVariable(variable, fallback)) + below;
}

QString userTemp()
{
    return QDir::tempPath();
}

QString windowsTemp()
{
    return windowsFolder("SystemRoot", QStringLiteral("C:\\Windows"), QStringLiteral("/Temp"));
}

QStringList errorReports()
{
    return {windowsFolder("ProgramData", QStringLiteral("C:\\ProgramData"), QStringLiteral("/Microsoft/Windows/WER/ReportArchive")),
            windowsFolder("ProgramData", QStringLiteral("C:\\ProgramData"), QStringLiteral("/Microsoft/Windows/WER/ReportQueue"))};
}

QString deliveryCache()
{
    return windowsFolder("SystemRoot", QStringLiteral("C:\\Windows"),
                         QStringLiteral("/ServiceProfiles/NetworkService/AppData/Local/Microsoft/Windows/DeliveryOptimization/Cache"));
}

quint64 recycleBinSize()
{
    SHQUERYRBINFO info = {};
    info.cbSize = sizeof info;
    return SUCCEEDED(SHQueryRecycleBinW(nullptr, &info)) && info.i64Size > 0 ? quint64(info.i64Size) : 0;
}

// Sizes on a worker thread: walking the folders takes a moment.
class SizeWorker : public QObject
{
    Q_OBJECT
public slots:
    void run()
    {
        CleanupDialog::Sizes s;
        s.userTemp = cleanup::folderSize(userTemp());
        s.windowsTemp = cleanup::folderSize(windowsTemp());
        s.recycleBin = recycleBinSize();
        for (const QString &folder : errorReports())
            s.errorReports += cleanup::folderSize(folder);
        s.delivery = cleanup::folderSize(deliveryCache());
        emit done(s);
    }
signals:
    void done(const CleanupDialog::Sizes &sizes);
};

class DeleteWorker : public QObject
{
    Q_OBJECT
public:
    DeleteWorker(const QStringList &folders, bool recycleBin) : m_folders(folders), m_recycleBin(recycleBin) {}
public slots:
    void run()
    {
        cleanup::Result total;
        for (const QString &folder : m_folders) {
            const cleanup::Result r = cleanup::removeContents(folder);
            total.failed += r.failed;
            if (total.firstError.isEmpty())
                total.firstError = r.firstError;
        }
        QString binError;
        if (m_recycleBin) {
            const HRESULT result = SHEmptyRecycleBinW(nullptr, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
            if (FAILED(result) && recycleBinSize() > 0)
                binError = win::errorText(DWORD(HRESULT_CODE(result)));
        }
        emit done(total.failed, total.firstError, binError);
    }
signals:
    void done(int failed, const QString &firstError, const QString &binError);

private:
    QStringList m_folders;
    bool m_recycleBin;
};

} // namespace

// --- Optimize -----------------------------------------------------------------

OptimizeDialog::OptimizeDialog(UDisks *udisks, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_list(plainList({tr("Drive"), tr("Kind"), tr("What it gets")}))
    , m_last(new QLabel)
    , m_weekly(new QCheckBox(tr("Let Windows optimize them every week")))
    , m_now(new QPushButton(tr("Optimize Now")))
    , m_status(new QLabel)
{
    setWindowTitle(tr("Optimize Drives"));
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("<p>Drives stay fast when they're looked after the way each kind needs: SSDs are told which "
                                       "space is free (TRIM), and hard drives are defragmented. Windows does it every week by "
                                       "itself; Optimize Now does it straight away.</p>")));
    layout->addWidget(m_list, 1);
    auto *row = new QHBoxLayout;
    row->addWidget(m_weekly);
    row->addStretch();
    row->addWidget(m_last);
    layout->addLayout(row);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    box->addButton(m_now, QDialogButtonBox::ActionRole);
    connect(box, &QDialogButtonBox::rejected, this, &OptimizeDialog::reject);
    layout->addWidget(box);

    connect(m_now, &QPushButton::clicked, this, [this] {
        QStringList letters;
        for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
            const QString letter = m_list->topLevelItem(i)->data(0, Qt::UserRole).toString();
            if (!letter.isEmpty())
                letters << letter;
        }
        m_running = true;
        m_now->setEnabled(false);
        m_weekly->setEnabled(false);
        m_status->clear();
        optimizeNext(letters);
    });
    connect(m_weekly, &QCheckBox::clicked, this, [this](bool on) {
        m_weekly->setEnabled(false);
        win::powerShell(this, (on ? QStringLiteral("Enable-ScheduledTask ") : QStringLiteral("Disable-ScheduledTask ")) + kTask + QStringLiteral(" | Out-Null"), {},
                        [this](bool ok, const QString &said) {
                            if (!ok)
                                m_status->setText(tr("Couldn't change that: %1").arg(said));
                            askAboutTask();
                        });
    });
    refresh();
    resize(680, 420);
}

void OptimizeDialog::reject()
{
    // Optimize-Volume goes on in Windows anyway; the list just stops following it.
    if (!m_running || QMessageBox::question(this, windowTitle(), tr("Close? Windows finishes optimizing the drive it's on by itself.")) == QMessageBox::Yes)
        QDialog::reject();
}

void OptimizeDialog::refresh()
{
    m_list->clear();
    for (const Disk &d : m_udisks->disks()) {
        for (const Volume &v : d.volumes) {
            const QStringList mounts = v.mounts();
            if (mounts.isEmpty() || d.isLoop)
                continue;
            // The title has the drive letter in it already.
            auto *item = new QTreeWidgetItem(m_list, {volumeTitle(v), diskKind(d), whatItDoes(d)});
            item->setToolTip(0, Qt::convertFromPlainText(d.model, Qt::WhiteSpaceNormal));
            if (optimizable(d) && mounts.first().size() >= 2 && mounts.first().at(1) == QLatin1Char(':')) {
                item->setData(0, Qt::UserRole, mounts.first().left(1));
                item->setData(0, Qt::UserRole + 1, !d.discard && d.rotationRate > 0); // defragmenting takes long
            }
        }
    }
    for (int c = 0; c < 3; ++c)
        m_list->resizeColumnToContents(c);
    m_now->setEnabled(!m_running);
    askAboutTask();
}

void OptimizeDialog::askAboutTask()
{
    // Windows' own weekly task: whether it's on, and when it last ran (PowerShell takes a few
    // seconds to load what asks about tasks).
    m_weekly->setEnabled(false);
    m_last->setText(tr("Last done by Windows: …"));
    win::powerShell(this,
                    QStringLiteral("$t = Get-ScheduledTask %1; $i = $t | Get-ScheduledTaskInfo; "
                                   "Write-Output ('{0}|{1}' -f $t.State, $i.LastRunTime.ToString('o'))")
                        .arg(kTask),
                    {}, [this](bool ok, const QString &said) {
                        const QString state = said.section(QLatin1Char('|'), 0, 0);
                        const QDateTime last = QDateTime::fromString(said.section(QLatin1Char('|'), 1), Qt::ISODateWithMs);
                        {
                            const QSignalBlocker block(m_weekly);
                            m_weekly->setChecked(ok && state != QLatin1String("Disabled"));
                        }
                        m_weekly->setEnabled(ok && !m_running);
                        m_last->setText(tr("Last done by Windows: %1")
                                            .arg(ok && last.isValid() && last.date().year() > 2000 ? QLocale().toString(last, QLocale::ShortFormat)
                                                                                                     : tr("never")));
                    });
}

void OptimizeDialog::optimizeNext(QStringList letters)
{
    auto rowOf = [this](const QString &letter) -> QTreeWidgetItem * {
        for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
            if (m_list->topLevelItem(i)->data(0, Qt::UserRole).toString() == letter)
                return m_list->topLevelItem(i);
        }
        return nullptr;
    };
    if (letters.isEmpty()) {
        // The list keeps what happened to each drive.
        m_running = false;
        m_status->setText(tr("Done."));
        m_now->setEnabled(true);
        askAboutTask();
        return;
    }
    const QString letter = letters.takeFirst();
    QTreeWidgetItem *row = rowOf(letter);
    if (row)
        row->setText(2, tr("Optimizing…"));
    m_status->setText(row && row->data(0, Qt::UserRole + 1).toBool() ? tr("Defragmenting %1: (a hard drive can take a while)…").arg(letter)
                                                                       : tr("Optimizing %1:…").arg(letter));
    // Without -ReTrim or -Defrag, Windows picks what the drive needs.
    win::powerShell(this, QStringLiteral("Optimize-Volume -DriveLetter $env:DF_LETTER"), {{QStringLiteral("DF_LETTER"), letter}},
                    [this, letters, letter, rowOf](bool ok, const QString &said) {
                        if (QTreeWidgetItem *item = rowOf(letter))
                            item->setText(2, ok ? tr("Optimized") : tr("Couldn't: %1").arg(said));
                        optimizeNext(letters);
                    });
}

// --- Cleanup ------------------------------------------------------------------

CleanupDialog::CleanupDialog(UDisks *udisks, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_userTemp(new QCheckBox(tr("Your temporary files")))
    , m_windowsTemp(new QCheckBox(tr("Windows' temporary files")))
    , m_recycle(new QCheckBox(tr("Recycle Bin")))
    , m_reports(new QCheckBox(tr("Error reports")))
    , m_delivery(new QCheckBox(tr("Update files shared with other PCs")))
    , m_userTempSize(new QLabel)
    , m_windowsTempSize(new QLabel)
    , m_recycleSize(new QLabel)
    , m_reportsSize(new QLabel)
    , m_deliverySize(new QLabel)
    , m_status(new QLabel)
    , m_clean(new QPushButton(tr("Clean Up")))
{
    qRegisterMetaType<CleanupDialog::Sizes>();
    setWindowTitle(tr("Disk Cleanup"));
    for (QCheckBox *c : {m_userTemp, m_windowsTemp, m_reports, m_delivery})
        c->setChecked(true);

    auto *grid = new QGridLayout;
    int row = 0;
    auto add = [&](QCheckBox *box, QLabel *size, const QString &explain) {
        size->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(box, row, 0);
        grid->addWidget(size, row++, 1);
        auto *label = wrappingLabel(QStringLiteral("<small>%1</small>").arg(explain));
        label->setContentsMargins(24, 0, 0, 6);
        grid->addWidget(label, row++, 0, 1, 2);
    };
    add(m_userTemp, m_userTempSize, tr("What programs left in %1. Files a running program still uses stay.")
                                        .arg(QDir::toNativeSeparators(userTemp()).toHtmlEscaped()));
    add(m_windowsTemp, m_windowsTempSize, tr("The same for Windows itself, in %1.").arg(QDir::toNativeSeparators(windowsTemp()).toHtmlEscaped()));
    add(m_recycle, m_recycleSize, tr("Files you deleted, on every drive. They're gone for good afterwards."));
    add(m_reports, m_reportsSize, tr("Reports Windows kept about programs that crashed, already sent or never to be."));
    add(m_delivery, m_deliverySize, tr("Windows updates kept to pass on to other PCs (Delivery Optimization). Windows downloads "
                                       "them again if it needs them."));
    grid->setColumnStretch(0, 1);

    m_status->setWordWrap(true);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("<p>Frees space by removing things that are safe to remove.</p>")));
    layout->addLayout(grid);
    layout->addStretch();
    layout->addWidget(m_status);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    box->addButton(m_clean, QDialogButtonBox::ActionRole);
    connect(m_clean, &QPushButton::clicked, this, &CleanupDialog::clean);
    connect(box, &QDialogButtonBox::rejected, this, &CleanupDialog::reject);
    layout->addWidget(box);

    for (QCheckBox *c : {m_userTemp, m_windowsTemp, m_recycle, m_reports, m_delivery})
        connect(c, &QCheckBox::toggled, this, [this] { showSizes(m_sizes); });
    resize(600, sizeHint().height());
    measure();
}

CleanupDialog::~CleanupDialog()
{
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
}

void CleanupDialog::reject()
{
    if (!m_busy)
        QDialog::reject();
}

void CleanupDialog::setBusy(bool busy, const QString &text)
{
    m_busy = busy;
    for (QWidget *w : std::initializer_list<QWidget *>{m_userTemp, m_windowsTemp, m_recycle, m_reports, m_delivery, m_clean})
        w->setEnabled(!busy);
    if (!text.isEmpty())
        m_status->setText(text);
    if (!busy)
        showSizes(m_sizes);
}

// Sizes worked out again; `done` (after cleaning) is said with what was freed.
void CleanupDialog::measure(const QString &done)
{
    setBusy(true, done.isEmpty() ? tr("Working out how much can go…") : tr("Cleaning up…"));
    if (done.isEmpty()) {
        for (QLabel *l : {m_userTempSize, m_windowsTempSize, m_recycleSize, m_reportsSize, m_deliverySize})
            l->setText(QStringLiteral("…"));
    }
    auto *worker = new SizeWorker;
    connect(worker, &SizeWorker::done, this, [this, done](const Sizes &sizes) {
        m_thread->quit();
        m_thread->wait();
        m_thread = nullptr;
        m_sizes = sizes;
        if (done.isEmpty()) {
            setBusy(false);
            m_status->clear();
            return;
        }
        const quint64 after = sizes.userTemp + sizes.windowsTemp + sizes.recycleBin + sizes.errorReports + sizes.delivery;
        setBusy(false, tr("Freed about %1.").arg(formatSize(m_before > after ? m_before - after : 0))
                           + (m_problems.isEmpty() ? QString() : QLatin1Char(' ') + m_problems.join(QLatin1Char(' '))));
    });
    m_thread = startOnThread(this, worker);
}

void CleanupDialog::showSizes(const Sizes &s)
{
    auto amount = [](quint64 bytes) { return bytes > 0 ? formatSize(bytes) : tr("nothing to remove"); };
    m_userTempSize->setText(amount(s.userTemp));
    m_windowsTempSize->setText(amount(s.windowsTemp));
    m_recycleSize->setText(amount(s.recycleBin));
    m_reportsSize->setText(amount(s.errorReports));
    m_deliverySize->setText(amount(s.delivery));
    if (m_busy)
        return;
    m_userTemp->setEnabled(s.userTemp > 0);
    m_windowsTemp->setEnabled(s.windowsTemp > 0);
    m_recycle->setEnabled(s.recycleBin > 0);
    m_reports->setEnabled(s.errorReports > 0);
    m_delivery->setEnabled(s.delivery > 0);
    quint64 total = 0;
    total += m_userTemp->isEnabled() && m_userTemp->isChecked() ? s.userTemp : 0;
    total += m_windowsTemp->isEnabled() && m_windowsTemp->isChecked() ? s.windowsTemp : 0;
    total += m_recycle->isEnabled() && m_recycle->isChecked() ? s.recycleBin : 0;
    total += m_reports->isEnabled() && m_reports->isChecked() ? s.errorReports : 0;
    total += m_delivery->isEnabled() && m_delivery->isChecked() ? s.delivery : 0;
    m_clean->setEnabled(total > 0);
    m_clean->setText(total > 0 ? tr("Free %1").arg(formatSize(total)) : tr("Clean Up"));
}

void CleanupDialog::clean()
{
    auto chosen = [](QCheckBox *c) { return c->isEnabled() && c->isChecked(); };
    const bool recycle = chosen(m_recycle);
    if (recycle && QMessageBox::question(this, windowTitle(), tr("Empty the Recycle Bin? What's in it can't be brought back afterwards."))
                       != QMessageBox::Yes)
        return;
    QStringList folders;
    if (chosen(m_userTemp))
        folders << userTemp();
    if (chosen(m_windowsTemp))
        folders << windowsTemp();
    if (chosen(m_reports))
        folders << errorReports();
    const bool delivery = chosen(m_delivery);
    const Sizes &s = m_sizes;
    m_before = s.userTemp + s.windowsTemp + s.recycleBin + s.errorReports + s.delivery;
    m_problems.clear();
    setBusy(true, tr("Cleaning up…"));

    auto *worker = new DeleteWorker(folders, recycle);
    connect(worker, &DeleteWorker::done, this, [this, delivery](int failed, const QString &firstError, const QString &binError) {
        m_thread->quit();
        m_thread->wait();
        m_thread = nullptr;
        if (failed > 0)
            m_problems << tr("%n file(s) stayed, in use by programs that are running or by Windows.", nullptr, failed);
        Q_UNUSED(firstError)
        if (!binError.isEmpty())
            m_problems << tr("The Recycle Bin couldn't be emptied: %1.").arg(binError);
        if (!delivery)
            return measure(QStringLiteral("done"));
        // Delivery Optimization's cache is the service's own: it empties it itself.
        win::powerShell(this, QStringLiteral("Delete-DeliveryOptimizationCache -Force"), {}, [this](bool ok, const QString &said) {
            if (!ok)
                m_problems << tr("The update files couldn't be removed: %1.").arg(said);
            measure(QStringLiteral("done"));
        });
    });
    m_thread = startOnThread(this, worker);
}

#include "systemtools_win.moc"
