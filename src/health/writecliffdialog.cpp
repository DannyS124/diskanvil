// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "writecliffdialog.h"

#include "applog.h"
#include "dialogs.h"
#include "format.h"
#include "jobui.h"
#include "theme.h"
#include "udisks.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStorageInfo>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

bool WriteCliffDialog::allowLoopDevicesForTest = false;

namespace writecliff {

// The speed against how much was written, and where it fell.
class Graph : public QWidget
{
public:
    Graph()
    {
        setMinimumHeight(200);
        setObjectName(QStringLiteral("graph"));
    }
    void set(const QVector<Point> &points, quint64 total, const Finding &f)
    {
        m_points = points;
        m_total = total;
        m_finding = f;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPalette pal = palette();
        p.fillRect(rect(), pal.color(QPalette::Base));
        const QRectF area = QRectF(rect()).adjusted(56, 24, -16, -30);
        double top = 100;
        for (const Point &pt : std::as_const(m_points))
            top = std::max(top, pt.mbps);
        // A round number above the fastest.
        const double step = std::pow(10, std::floor(std::log10(top)));
        top = std::ceil(top / step) * step;
        p.setPen(QPen(pal.color(QPalette::Mid), 1));
        const QFont small = [this] {
            QFont f = font();
            f.setPointSizeF(f.pointSizeF() * 0.85);
            return f;
        }();
        p.setFont(small);
        for (int i = 0; i <= 4; ++i) {
            const double y = area.bottom() - area.height() * i / 4;
            p.setPen(QPen(pal.color(QPalette::Mid), 1, Qt::DotLine));
            p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
            p.setPen(pal.color(QPalette::Text));
            p.drawText(QRectF(0, y - 8, area.left() - 6, 16), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("%1").arg(qRound(top * i / 4)));
        }
        p.drawText(QRectF(4, 2, 200, 16), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("MB/s"));
        const quint64 total = std::max<quint64>(1, m_total);
        for (int i = 0; i <= 4; ++i) {
            const double x = area.left() + area.width() * i / 4;
            const QString label = i == 0 ? QStringLiteral("0") : formatSize(total * quint64(i) / 4);
            if (i == 4)
                p.drawText(QRectF(x - 120, area.bottom() + 4, 120, 16), Qt::AlignRight, label);
            else
                p.drawText(i == 0 ? QRectF(x, area.bottom() + 4, 120, 16) : QRectF(x - 60, area.bottom() + 4, 120, 16), i == 0 ? Qt::AlignLeft : Qt::AlignHCenter, label);
        }
        if (m_points.isEmpty())
            return;
        auto at = [&](const Point &pt) {
            return QPointF(area.left() + area.width() * double(pt.written) / double(total), area.bottom() - area.height() * pt.mbps / top);
        };
        QPainterPath line;
        line.moveTo(at({0, m_points.first().mbps}));
        for (const Point &pt : std::as_const(m_points))
            line.lineTo(at(pt));
        p.setPen(QPen(pal.color(QPalette::Highlight), 2));
        p.drawPath(line);
        if (m_finding.cliff) {
            const double x = area.left() + area.width() * double(m_finding.cacheBytes) / double(total);
            p.setPen(QPen(QColor(0xff, 0x33, 0x55), 1.5, Qt::DashLine));
            p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
            p.drawText(QRectF(x + 4, area.top(), 200, 16), Qt::AlignLeft, QCoreApplication::translate("writecliff", "cache full"));
        }
    }

private:
    QVector<Point> m_points;
    quint64 m_total = 0;
    Finding m_finding;
};

} // namespace writecliff

WriteCliffDialog::WriteCliffDialog(UDisks *udisks, const QString &preferredDisk, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_targets(new QComboBox)
    , m_inFree(new QRadioButton(tr("Into its free space: a test file, deleted afterwards (nothing on it is touched)")))
    , m_whole(new QRadioButton(tr("Over the whole drive: for a drive with nothing on it (it erases it)")))
    , m_amount(new QSpinBox)
    , m_where(new QLabel)
    , m_confirm(new QLineEdit)
    , m_start(new QPushButton(tr("Start")))
    , m_graph(new writecliff::Graph)
    , m_result(new QLabel)
{
    qRegisterMetaType<writecliff::Point>();
    setWindowTitle(tr("Write Speed Over Time"));
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("Most SSDs, and many USB sticks, write into a fast cache first, and slow down a lot once it's full: "
                                       "that's why a big copy starts fast and then crawls. This writes a lot at once and draws the speed "
                                       "as it goes, so you see how big the cache is and how fast the drive really is after it. Cheap and "
                                       "fake drives show themselves here. Write more than the cache to see the drop (some hold 50 GB "
                                       "or more).")));
    auto *form = new QFormLayout;
    m_targets->setObjectName(QStringLiteral("targets"));
    form->addRow(tr("Drive:"), m_targets);
    auto *modes = new QButtonGroup(this);
    modes->addButton(m_inFree);
    modes->addButton(m_whole);
    m_inFree->setObjectName(QStringLiteral("inFree"));
    m_whole->setObjectName(QStringLiteral("whole"));
    m_inFree->setChecked(true);
    auto *modeBox = new QVBoxLayout;
    modeBox->addWidget(m_inFree);
    modeBox->addWidget(m_whole);
    form->addRow(tr("Write:"), modeBox);
    m_amount->setObjectName(QStringLiteral("amount"));
    m_amount->setSuffix(tr(" GB"));
    m_amount->setRange(1, 1);
    form->addRow(tr("How much:"), m_amount);
    layout->addLayout(form);
    m_where->setWordWrap(true);
    m_where->setTextFormat(Qt::RichText);
    m_where->setObjectName(QStringLiteral("where"));
    layout->addWidget(m_where);
    m_confirm->setObjectName(QStringLiteral("confirm"));
    layout->addWidget(m_confirm);
    layout->addWidget(m_graph, 1);
    m_result->setObjectName(QStringLiteral("result"));
    m_result->setWordWrap(true);
    layout->addWidget(m_result);
    auto *buttons = new QHBoxLayout;
    auto *close = new QPushButton(tr("Close"));
    m_start->setObjectName(QStringLiteral("start"));
    buttons->addStretch();
    buttons->addWidget(m_start);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(close, &QPushButton::clicked, this, &WriteCliffDialog::reject);
    connect(m_start, &QPushButton::clicked, this, &WriteCliffDialog::start);
    connect(m_targets, &QComboBox::currentIndexChanged, this, &WriteCliffDialog::updateState);
    connect(modes, &QButtonGroup::buttonToggled, this, &WriteCliffDialog::updateState);
    connect(m_confirm, &QLineEdit::textChanged, this, &WriteCliffDialog::updateState);
    connect(m_udisks, &UDisks::changed, this, [this] {
        if (m_running)
            return;
        fillTargets(m_targets->currentData().toString());
        updateState();
    });
    fillTargets(preferredDisk);
    updateState();
    resize(720, 620);
}

WriteCliffDialog::~WriteCliffDialog()
{
    if (m_thread) {
        if (m_writer)
            m_writer->cancel();
        m_thread->quit();
        m_thread->wait();
    }
}

void WriteCliffDialog::fillTargets(const QString &preferred)
{
    const QSignalBlocker block(m_targets);
    m_targets->clear();
    const QVector<Disk> &disks = m_udisks->disks();
    for (int i = 0; i < disks.size(); ++i) {
        const Disk &d = disks[i];
        if (d.isRaid || d.readOnly || (d.isLoop && !allowLoopDevicesForTest))
            continue;
        m_targets->addItem(tr("Disk %1: %2").arg(i).arg(diskTitle(d)), d.blockPath);
        if (d.blockPath == preferred)
            m_targets->setCurrentIndex(m_targets->count() - 1);
    }
}

const Disk *WriteCliffDialog::target() const
{
    return m_udisks->diskByPath(m_targets->currentData().toString());
}

QString WriteCliffDialog::freeSpaceFolder(quint64 *free) const
{
    const Disk *d = target();
    QString best;
    *free = 0;
    for (const Volume &v : d ? d->volumes : QVector<Volume>()) {
        for (const QString &m : v.mounts()) {
            const QStorageInfo info(m);
            if (!info.isValid() || info.isReadOnly() || !QFileInfo(m).isWritable())
                continue;
            if (quint64(info.bytesAvailable()) > *free) {
                *free = quint64(info.bytesAvailable());
                best = m;
            }
        }
    }
    return best;
}

void WriteCliffDialog::updateState()
{
    const Disk *d = target();
    bool ok = d && !m_running;
    QString where;
    quint64 room = 0;
    if (d && m_inFree->isChecked()) {
        quint64 free = 0;
        const QString folder = freeSpaceFolder(&free);
        if (folder.isEmpty()) {
            where = tr("None of it is open where it can be written to: open (mount) one of its partitions first.").toHtmlEscaped();
            ok = false;
        } else {
            room = free / 10 * 9; // a tenth left free, so nothing else runs out of room meanwhile
            where = tr("The test file goes in %1 (%2 free). It's deleted at the end.").arg(QDir::toNativeSeparators(folder), formatSize(free)).toHtmlEscaped();
        }
    } else if (d) {
        bool used = false;
        for (const Volume &v : d->volumes)
            used |= v.hasFilesystem || v.encrypted || !v.mounts().isEmpty();
        const QString name = shortDevice(d->device);
        room = d->size;
        if (d->isSystem) {
            where = tr("Not the drive the system runs from: use its free space instead.").toHtmlEscaped();
            ok = false;
        } else {
            where = QStringLiteral("<b>%1</b>").arg(tr("Everything on %1 will be erased.").arg(name).toHtmlEscaped());
            if (used)
                where += QStringLiteral(" ") + tr("It has partitions with something on them: make sure there's nothing you need.").toHtmlEscaped();
            where += QStringLiteral("<br>") + tr("Type <b>%1</b> to confirm:").arg(name.toHtmlEscaped());
            m_confirm->setPlaceholderText(name);
            ok = ok && m_confirm->text().trimmed() == name;
        }
    }
    m_confirm->setVisible(d && m_whole->isChecked() && !d->isSystem && !m_running);
    const int gb = int(std::max<quint64>(1, room / 1000000000));
    const QSignalBlocker block(m_amount);
    const bool fresh = m_amount->maximum() != gb;
    m_amount->setRange(1, gb);
    if (fresh)
        m_amount->setValue(std::min(gb, 64)); // enough to fill most caches; more to be sure
    ok = ok && room >= 64ull * 1024 * 1024;
    m_where->setText(where);
    m_start->setEnabled(ok);
    m_targets->setEnabled(!m_running);
    m_inFree->setEnabled(!m_running);
    m_whole->setEnabled(!m_running);
    m_amount->setEnabled(!m_running);
}

void WriteCliffDialog::reject()
{
    if (m_running) {
        if (m_writer)
            m_writer->cancel();
        return;
    }
    QDialog::reject();
}

void WriteCliffDialog::start()
{
    const Disk *d = target();
    if (!d)
        return;
    // Gigabytes as drives count them; never more than there's room for.
    quint64 total = quint64(m_amount->value()) * 1000000000ull;
    m_points.clear();
    m_result->clear();
    m_graph->set({}, total, {});
    m_running = true;
    m_start->setText(tr("Stop"));
    disconnect(m_start, &QPushButton::clicked, this, nullptr);
    connect(m_start, &QPushButton::clicked, this, [this] {
        if (m_writer)
            m_writer->cancel();
    });
    m_start->setEnabled(true);
    m_confirm->hide();
    updateState();
    m_start->setEnabled(true);
    auto begin = [this, total](const QString &file, int fd) {
        auto *writer = new writecliff::Writer(file, fd, total);
        m_writer = writer;
        connect(writer, &writecliff::Writer::point, this, [this, total](const writecliff::Point &p) {
            m_points << p;
            m_graph->set(m_points, total, writecliff::analyse(m_points));
            m_result->setText(tr("%1 written, %2 MB/s now…").arg(formatSize(p.written)).arg(qRound(p.mbps)));
        });
        connect(writer, &writecliff::Writer::finished, this, &WriteCliffDialog::done);
        m_thread = startOnThread(this, writer);
    };
    qCInfo(lcOps).noquote() << "Write speed over time on" << d->device << (m_inFree->isChecked() ? "free space" : "whole drive") << formatSize(total);
    if (m_inFree->isChecked()) {
        quint64 free = 0;
        const QString folder = freeSpaceFolder(&free);
        begin(QDir(folder).filePath(QStringLiteral(".diskanvil-write-speed-test")), -1);
        return;
    }
    openBlockThen(m_udisks, this, d->blockPath, UDisks::OpenMode::ReadWriteDirect, [this, begin](int fd) {
        if (fd < 0)
            return done(false, tr("The drive couldn't be opened for writing."));
        begin(QString(), fd);
    });
}

void WriteCliffDialog::done(bool completed, const QString &error)
{
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
        m_thread = nullptr;
    }
    m_writer = nullptr;
    m_running = false;
    m_start->setText(tr("Start"));
    disconnect(m_start, &QPushButton::clicked, this, nullptr);
    connect(m_start, &QPushButton::clicked, this, &WriteCliffDialog::start);
    QString text;
    if (!m_points.isEmpty())
        text = writecliff::describe(writecliff::analyse(m_points));
    if (!error.isEmpty())
        text = error + (text.isEmpty() ? QString() : QStringLiteral(" ") + tr("So far: %1").arg(text));
    if (m_whole->isChecked() && !m_points.isEmpty())
        text += QStringLiteral(" ") + tr("The drive was written over: make a partition table on it before use.");
    m_result->setText(text);
    qCInfo(lcOps).noquote() << "Write speed over time:" << text;
    m_udisks->refresh();
    updateState();
    emit ended(completed);
}
