// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "recoverdialog.h"

#include "applog.h"
#include "blockio.h"
#include "dialogs.h"
#include "diskmap.h"
#include "format.h"
#include "jobui.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QTimer>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QSignalBlocker>
#include <QThread>
#include <QApplication>
#include <QVBoxLayout>

#include <memory>
#include <unistd.h>

QString RecoverDialog::refusal(const Disk &disk)
{
    if (disk.isSystem)
        return tr("%1 holds the running system, so its partition table can't be written while it runs.").arg(shortDevice(disk.device));
    for (const Volume &v : disk.volumes) {
#ifdef Q_OS_WIN
        // Windows gives a blank stick a drive letter all the same: that holds no file system.
        if (!v.hasFilesystem)
            continue;
#endif
        if (!v.mounts().isEmpty() || v.swapActive || !v.cleartextPath.isEmpty())
            return tr("%1 is in use. Unmount (and lock) everything on the drive first.").arg(shortDevice(v.device));
    }
    return {};
}

RecoverDialog::RecoverDialog(UDisks *udisks, const Disk &disk, const gpt::Report &report, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_disk(disk)
    , m_blockPath(disk.blockPath)
    , m_device(shortDevice(disk.device))
    , m_driveKey(disk.health.key)
    , m_intro(new QLabel)
    , m_list(new QListWidget)
    , m_before(new DiskMap)
    , m_after(new DiskMap)
    , m_problem(new QLabel)
    , m_confirm(new QLineEdit)
{
    build();
    listSources(report);
}

const Disk *RecoverDialog::disk() const
{
    const Disk *live = m_udisks->diskByPath(m_blockPath);
    return live ? live : &m_disk;
}

RecoverDialog::RecoverDialog(UDisks *udisks, const Disk &disk, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_disk(disk)
    , m_blockPath(disk.blockPath)
    , m_device(shortDevice(disk.device))
    , m_driveKey(disk.health.key)
    , m_intro(new QLabel)
    , m_list(new QListWidget)
    , m_before(new DiskMap)
    , m_after(new DiskMap)
    , m_problem(new QLabel)
    , m_confirm(new QLineEdit)
{
    build();
    const QString why = refusal(disk);
    if (!why.isEmpty()) {
        m_problem->setText(redText(why));
        m_list->setEnabled(false);
        m_confirm->setEnabled(false);
        refresh();
        return;
    }
    // Read-only first, like a benchmark: just to see what's there.
    openBlockThen(udisks, this, disk.blockPath, UDisks::OpenMode::Benchmark, [this](int fd) {
        gpt::Report report;
        if (fd >= 0) {
            report = gpt::inspect(fd);
            blockio::close(fd);
        }
        listSources(report);
        emit ready();
    });
    refresh();
}

RecoverDialog::~RecoverDialog()
{
    if (m_scanThread) {
        m_stopFlag->store(true); // it looks every few dozen places, so this is quick
        m_scanThread->wait();
    }
    if (m_scanFd >= 0)
        blockio::close(m_scanFd);
}

void RecoverDialog::build()
{
    setWindowTitle(tr("Recover Partitions on %1").arg(diskTitle(m_disk)));
    m_intro->setWordWrap(true);
    m_intro->setText(tr("Puts a partition table back: from the copy GPT keeps at the end of the drive, or from a layout "
                        "DiskAnvil saw on this drive before. Only the table is written; what's in the partitions isn't "
                        "touched. The table there now is remembered first, so this can be undone the same way."));
    m_problem->setWordWrap(true);
    m_problem->setTextFormat(Qt::RichText);
    m_list->setObjectName(QStringLiteral("sources"));
    m_before->setDisks({m_disk});
    for (DiskMap *map : {m_before, m_after}) {
        map->setAttribute(Qt::WA_TransparentForMouseEvents); // pictures, not something to click
        map->setFocusPolicy(Qt::NoFocus);
    }
    m_confirm->setPlaceholderText(m_device);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_intro);
    layout->addWidget(new QLabel(tr("Put back:")));
    layout->addWidget(m_list, 1);
    m_scan = new QPushButton(tr("Scan the Drive for File Systems…"));
    m_scan->setToolTip(tr("When there's no table and nothing saved: looks through the drive for file systems where partitions started."));
    m_progress = new QProgressBar;
    m_progress->setRange(0, 1000);
    m_scanStatus = new QLabel;
    m_stopScan = new QPushButton(tr("Stop Scan"));
    auto *scanRow = new QHBoxLayout;
    scanRow->addWidget(m_scan);
    scanRow->addWidget(m_progress, 1);
    scanRow->addWidget(m_scanStatus);
    scanRow->addWidget(m_stopScan);
    scanRow->addStretch();
    layout->addLayout(scanRow);
    for (QWidget *w : std::initializer_list<QWidget *>{m_progress, m_scanStatus, m_stopScan})
        w->hide();
    m_found = new QListWidget;
    m_found->setObjectName(QStringLiteral("found"));
    m_found->setMaximumHeight(130);
    m_found->hide();
    layout->addWidget(m_found);
    connect(m_scan, &QPushButton::clicked, this, &RecoverDialog::startScan);
    connect(m_stopScan, &QPushButton::clicked, this, [this] {
        if (m_stopFlag)
            m_stopFlag->store(true);
        m_stopScan->setEnabled(false);
    });
    connect(m_found, &QListWidget::itemChanged, this, &RecoverDialog::ticked);
    layout->addWidget(new QLabel(tr("Now:")));
    layout->addWidget(m_before);
    layout->addWidget(new QLabel(tr("After:")));
    layout->addWidget(m_after);
    layout->addWidget(m_problem);
    auto *confirmLabel = new QLabel(tr("Type <b>%1</b> to confirm:").arg(m_device.toHtmlEscaped()));
    layout->addWidget(confirmLabel);
    layout->addWidget(m_confirm);
    // Writing happens here (the window stays open to show how it went), so not an accept button.
    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    m_write = box->addButton(tr("Write Partition Table"), QDialogButtonBox::ActionRole);
    m_write->setAutoDefault(false);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(box);
    connect(m_write, &QPushButton::clicked, this, &RecoverDialog::write);
    connect(m_list, &QListWidget::currentRowChanged, this, &RecoverDialog::choose);
    connect(m_confirm, &QLineEdit::textChanged, this, &RecoverDialog::refresh);
    resize(820, 640);
}

int RecoverDialog::sourceCount() const
{
    return int(m_sources.size());
}

void RecoverDialog::listSources(const gpt::Report &report)
{
    const QString now = recover::fromDisk(*disk()).fingerprint();
    if (report.backup.valid() && report.backup.lba == report.lastLba && !report.primary.valid()) {
        Source s;
        s.backup = true;
        s.layout = recover::fromReport(report, true);
        s.title = tr("The backup copy at the end of the drive: %n partition(s)", nullptr, int(s.layout.parts.size()));
        m_sources.push_back(s);
    }
    for (const recover::Layout &l : recover::saved(m_driveKey)) {
        if (l.fingerprint() == now)
            continue; // that's what's there already
        Source s;
        s.layout = l;
        QStringList names;
        for (const recover::Part &p : l.parts) {
            if (!p.container)
                names << (p.label.isEmpty() ? (p.fsType.isEmpty() ? formatSize(p.size) : p.fsType) : p.label);
        }
        s.title = tr("As it was on %1: %2").arg(QLocale().toString(l.saved.toLocalTime(), QLocale::ShortFormat), names.join(QStringLiteral(", ")));
        m_sources.push_back(s);
    }
    for (const Source &s : std::as_const(m_sources))
        m_list->addItem(s.title); // plain text: names and labels come from the drive
    if (m_sources.isEmpty())
        m_problem->setText(tr("There's nothing to put back: the table is intact, and DiskAnvil hasn't seen another layout on this drive."));
    else
        m_list->setCurrentRow(0);
    refresh();
}

void RecoverDialog::choose(int row)
{
    if (row < 0 || row >= m_sources.size())
        return;
    m_found->setVisible(m_sources[row].scan);
    m_after->setDisks({recover::preview(*disk(), m_sources[row].layout)});
    refresh();
}

void RecoverDialog::refresh()
{
    const int row = m_list->currentRow();
    const Disk *d = disk();
    QString why;
    if (row >= 0 && row < m_sources.size() && !m_sources[row].backup) {
        // The sector size isn't known before the drive is open for writing; 512 is the
        // strictest for alignment, and write() checks again with the real one.
        why = recover::problem(m_sources[row].layout, d->size, 512);
        m_problem->setText(why.isEmpty() ? QString() : redText(why));
    }
    m_write->setEnabled(!m_busy && row >= 0 && why.isEmpty() && refusal(*d).isEmpty() && m_confirm->text().trimmed() == m_device);
    if (m_scan)
        m_scan->setEnabled(!m_busy && refusal(*d).isEmpty());
}

void RecoverDialog::startScan()
{
    const Disk *d = disk();
    if (!refusal(*d).isEmpty() || m_busy)
        return;
    // A rough idea of how long: a look at every MiB, quick on an SSD, a seek each on a hard drive.
    const double seconds = double(d->size / (1024 * 1024)) * (d->rotationRate == 0 ? 0.0003 : 0.008);
    QMessageBox ask(QMessageBox::Question, tr("Scan %1").arg(m_device),
                    tr("The scan reads a little at every megabyte of %1 (%2) and looks for file systems where partitions "
                       "started. That can take a while: %3. It only reads, and it can be stopped any time.")
                        .arg(m_device, formatSize(d->size), durationText(seconds)),
                    QMessageBox::NoButton, this);
    ask.setTextFormat(Qt::PlainText);
    QPushButton *go = ask.addButton(tr("Scan"), QMessageBox::AcceptRole);
    ask.addButton(QMessageBox::Cancel);
    ask.exec();
    if (ask.clickedButton() != go)
        return;
    m_busy = true;
    refresh();
    m_scanStatus->setText(tr("Opening the drive…"));
    m_scanStatus->show();
    // Plain reads (libblkid reads where it likes, which a benchmark open doesn't allow);
    // nothing's mounted, so nothing gets unmounted for it.
    openBlockThen(m_udisks, this, m_blockPath, UDisks::OpenMode::Read, [this](int fd) {
        if (fd < 0) {
            m_busy = false;
            m_scanStatus->setText(tr("Couldn't open the drive."));
            refresh();
            return;
        }
        m_scanFd = fd;
        m_scanSize = blockio::deviceSize(fd);
        m_stopFlag = std::make_shared<std::atomic<bool>>(false);
        m_progress->setValue(0);
        m_progress->show();
        m_stopScan->setEnabled(true);
        m_stopScan->show();
        m_scanTimer.start();
        QPointer<RecoverDialog> self(this);
        const auto stop = m_stopFlag;
        const quint64 size = m_scanSize;
        m_scanThread = QThread::create([self, fd, size, stop] {
            const QVector<partscan::Found> found = partscan::scan(fd, size, [self, stop](quint64 done, quint64 total) {
                QMetaObject::invokeMethod(qApp, [self, done, total] {
                    if (self)
                        self->scanProgress(done, total);
                }, Qt::QueuedConnection);
                return !stop->load();
            });
            QMetaObject::invokeMethod(qApp, [self, found] {
                if (self)
                    self->scanFinished(found);
            }, Qt::QueuedConnection);
        });
        m_scanThread->setParent(this);
        m_scanThread->start();
    });
}

void RecoverDialog::scanProgress(quint64 done, quint64 total)
{
    if (!total)
        return;
    m_progress->setValue(int(done * 1000 / total));
    const double elapsed = double(m_scanTimer.elapsed()) / 1000;
    const double left = done > 0 && elapsed > 3 ? elapsed * double(total - done) / double(done) : -1;
    m_scanStatus->setText(left >= 0 ? tr("%1 left").arg(durationText(left)) : tr("Scanning…"));
}

void RecoverDialog::scanFinished(const QVector<partscan::Found> &found)
{
    if (m_scanThread) {
        m_scanThread->wait();
        delete m_scanThread;
        m_scanThread = nullptr;
    }
    const bool stopped = m_stopFlag && m_stopFlag->load();
    if (m_scanFd >= 0)
        blockio::close(m_scanFd);
    m_scanFd = -1;
    m_busy = false;
    m_progress->hide();
    m_stopScan->hide();
    m_foundList = partscan::tidy(found, m_scanSize);
    if (m_foundList.isEmpty()) {
        m_scanStatus->setText(stopped ? tr("Stopped; nothing found before that.") : tr("No file systems found."));
        refresh();
        return;
    }
    m_scanStatus->setText(stopped ? tr("Stopped: %n file system(s) found before that.", nullptr, int(m_foundList.size()))
                                  : tr("Found %n file system(s).", nullptr, int(m_foundList.size())));
    {
        const QSignalBlocker quiet(m_found);
        m_found->clear();
        for (const partscan::Found &f : std::as_const(m_foundList)) {
            QString text = tr("%1 at %2, %3").arg(f.type, formatSize(f.offset), formatSize(f.size));
            if (!f.label.isEmpty())
                text += QStringLiteral(" \"%1\"").arg(f.label);
            if (f.sizeGuessed)
                text += tr(" (its size is a guess: up to the next one)");
            auto *item = new QListWidgetItem(text, m_found); // plain text: labels come from the drive
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Checked);
        }
    }
    Source s;
    s.scan = true;
    s.title = tr("Found by scanning the drive (tick the ones to keep below)");
    m_sources.erase(std::remove_if(m_sources.begin(), m_sources.end(), [](const Source &x) { return x.scan; }), m_sources.end());
    m_sources.push_back(s);
    const QSignalBlocker quiet(m_list);
    m_list->clear();
    for (const Source &x : std::as_const(m_sources))
        m_list->addItem(x.title);
    m_problem->clear();
    ticked();
    m_list->setCurrentRow(int(m_sources.size()) - 1);
    choose(int(m_sources.size()) - 1);
}

void RecoverDialog::ticked()
{
    QVector<partscan::Found> keep;
    for (int i = 0; i < m_found->count() && i < m_foundList.size(); ++i) {
        if (m_found->item(i)->checkState() == Qt::Checked)
            keep << m_foundList[i];
    }
    // The table it had, if it said; GPT otherwise.
    const QString table = disk()->tableType == QLatin1String("dos") ? QStringLiteral("dos") : QStringLiteral("gpt");
    for (Source &s : m_sources) {
        if (s.scan)
            s.layout = partscan::toLayout(keep, table, m_scanSize);
    }
    const int row = m_list->currentRow();
    if (row >= 0 && row < m_sources.size() && m_sources[row].scan)
        choose(row);
    refresh();
}

void RecoverDialog::write()
{
    const Disk *disk = m_udisks->diskByPath(m_blockPath);
    const int row = m_list->currentRow();
    if (!disk || row < 0 || row >= m_sources.size() || !refusal(*disk).isEmpty())
        return;
    // What's there now goes on the list first, so this can be undone the same way.
    recover::remember(m_driveKey, recover::fromDisk(*disk));
    const Source source = m_sources[row];
    m_busy = true;
    refresh();
    m_problem->setText(tr("Writing the partition table…"));
    openBlockThen(m_udisks, this, m_blockPath, UDisks::OpenMode::ReadWrite, [this, source](int fd) {
        if (fd < 0) {
            m_busy = false;
            m_problem->setText(redText(tr("Couldn't open the drive for writing.")));
            refresh();
            return;
        }
        const gpt::Result result = source.backup ? gpt::restoreFromBackup(fd) : recover::write(fd, source.layout);
        blockio::close(fd);
        qCInfo(lcOps).noquote() << "Recover Partitions on" << m_blockPath << (source.backup ? "from the backup GPT" : "from a layout")
                                << (result.ok ? QStringLiteral("done") : QStringLiteral("failed: ") + result.error);
        if (!result.ok) {
            m_busy = false;
            m_problem->setText(redText(result.error));
            refresh();
            emit done(false, result.error);
            return;
        }
        m_udisks->rescan(m_blockPath);
        waitForPartitions(int(source.layout.parts.size()));
    });
}

void RecoverDialog::waitForPartitions(int expected)
{
    // udev reads the new table when the drive is closed, and partitions can come and go for
    // a moment. Done once the count is right and nothing changed for 2 s (or after 20 s).
    m_problem->setText(tr("Waiting for the partitions to show up…"));
    auto *settled = new QTimer(this);
    settled->setSingleShot(true);
    settled->setInterval(2000);
    auto *timeout = new QTimer(this);
    timeout->setSingleShot(true);
    auto finish = [this, settled, timeout, expected](bool inTime) {
        settled->stop();
        timeout->stop();
        disconnect(m_udisks, &UDisks::changed, this, nullptr);
        const Disk *d = m_udisks->diskByPath(m_blockPath);
        const int found = d ? int(d->volumes.size()) : 0;
        m_busy = false;
        const QString message = found >= expected
            ? tr("The partition table of %1 is back: %n partition(s).", nullptr, found).arg(m_device)
            : tr("The table was written, but only %1 of %2 partitions showed up%3.").arg(found).arg(expected).arg(inTime ? QString() : tr(" in time"));
        // The window stays open to show how it went: the drive as it is now, and the message
        // (after refresh(), which would clear it).
        if (d)
            m_before->setDisks({*d});
        refresh();
        m_problem->setText(message.toHtmlEscaped());
        emit done(found >= expected, message);
    };
    connect(m_udisks, &UDisks::changed, this, [this, settled, expected] {
        const Disk *d = m_udisks->diskByPath(m_blockPath);
        if (d && int(d->volumes.size()) >= expected)
            settled->start();
        else
            settled->stop();
    });
    connect(settled, &QTimer::timeout, this, [finish] { finish(true); });
    connect(timeout, &QTimer::timeout, this, [finish] { finish(false); });
    timeout->start(20000);
    m_udisks->refresh();
}
