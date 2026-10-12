// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "certificates.h"

#include "applog.h"
#include "blockio.h"
#include "dialogs.h"
#include "format.h"
#include "jobui.h"
#include "pdfreport.h"
#include "udisks.h"

#include <QApplication>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

// The drive the record is about, when it's connected: by its serial number.
const Disk *driveOf(UDisks *udisks, const erasure::Record &r)
{
    for (const Disk &d : udisks->disks()) {
        if (!r.serial.isEmpty() ? d.serial == r.serial && d.model == r.model : d.device == r.device && d.size == r.size)
            return &d;
    }
    return nullptr;
}

} // namespace

CertificateDialog::CertificateDialog(UDisks *udisks, const erasure::Record &record, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_record(record)
    , m_summary(new QLabel)
    , m_by(new QLineEdit)
    , m_for(new QLineEdit)
    , m_notes(new QPlainTextEdit)
    , m_check(new QPushButton(tr("Check It")))
{
    setWindowTitle(tr("Certificate of Erasure"));
    m_summary->setWordWrap(true);
    m_summary->setObjectName(QStringLiteral("summary"));
    m_by->setObjectName(QStringLiteral("by"));
    m_for->setObjectName(QStringLiteral("for"));
    m_by->setText(QSettings(QStringLiteral("diskanvil"), QStringLiteral("diskanvil")).value(QStringLiteral("certificates/by")).toString());
    m_notes->setMaximumHeight(70);
    m_check->setObjectName(QStringLiteral("check"));
    m_check->setToolTip(tr("Reads it back in a hundred places, the very start and end too, to see they're all zeros"));
    m_check->setVisible(m_record.zeros);
    connect(m_check, &QPushButton::clicked, this, &CertificateDialog::check);
    auto *save = new QPushButton(tr("Save as PDF…"));
    save->setObjectName(QStringLiteral("save"));
    connect(save, &QPushButton::clicked, this, [this] {
        QSettings(QStringLiteral("diskanvil"), QStringLiteral("diskanvil")).setValue(QStringLiteral("certificates/by"), m_by->text().trimmed());
        const QString name = tr("Erasure %1 %2.pdf").arg(m_record.serial.isEmpty() ? m_record.model : m_record.serial, m_record.finished.toString(QStringLiteral("yyyy-MM-dd")));
        if (pdfreport::saveAs(this, html(), name, windowTitle()))
            accept();
    });
    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    auto *form = new QFormLayout;
    form->addRow(tr("Erased by:"), m_by);
    form->addRow(tr("Customer or order:"), m_for);
    form->addRow(tr("Notes:"), m_notes);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_check);
    buttons->addStretch();
    buttons->addWidget(save);
    buttons->addWidget(close);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_summary);
    layout->addLayout(form);
    layout->addLayout(buttons);
    const QLocale l;
    m_summary->setText(tr("<b>%1</b> (serial %2, %3)<br>%4<br>Finished %5. %6")
                           .arg(m_record.model.toHtmlEscaped(), m_record.serial.isEmpty() ? tr("unknown") : m_record.serial.toHtmlEscaped(),
                                formatSize(m_record.size), m_record.method.toHtmlEscaped(), l.toString(m_record.finished, QLocale::ShortFormat),
                                m_record.checked.isEmpty() ? QString() : m_record.checked.toHtmlEscaped()));
    m_check->setEnabled(driveOf(m_udisks, m_record) != nullptr && m_record.checked.isEmpty());
    if (m_record.zeros && !driveOf(m_udisks, m_record))
        m_check->setToolTip(tr("Connect the drive to check it"));
    resize(520, sizeHint().height());
}

QString CertificateDialog::html() const
{
    return erasure::certificateHtml(m_record, m_by->text(), m_for->text(), m_notes->toPlainText());
}

void CertificateDialog::check()
{
    const Disk *d = driveOf(m_udisks, m_record);
    if (!d)
        return;
    m_check->setEnabled(false);
    m_summary->setText(m_summary->text() + QStringLiteral("<br>") + tr("Checking…"));
    const quint64 size = d->size;
    openBlockThen(m_udisks, this, d->blockPath, UDisks::OpenMode::Peek, [this, size](int fd) {
        if (fd < 0) {
            m_summary->setText(m_summary->text() + QStringLiteral("<br>") + redText(tr("It couldn't be opened to read it.")));
            return;
        }
        // The first and last MiB, and a hundred places between, at random.
        QApplication::setOverrideCursor(Qt::WaitCursor);
        QVector<quint64> at{0, size > 1024 * 1024 ? size - 1024 * 1024 : 0};
        for (int i = 0; i < 100 && size > 1024 * 1024; ++i)
            at << QRandomGenerator::system()->bounded(size / 4096) * 4096;
        QByteArray buf;
        int zero = 0, read = 0;
        for (const quint64 offset : std::as_const(at)) {
            const quint64 len = std::min<quint64>(offset < 2 || offset + 1024 * 1024 >= size ? 1024 * 1024 : 4096, size - offset);
            buf.resize(qsizetype(len));
            if (!blockio::readAt(fd, buf.data(), len, offset))
                continue;
            ++read;
            zero += buf.count('\0') == buf.size() ? 1 : 0;
        }
        blockio::close(fd);
        QApplication::restoreOverrideCursor();
        const QString when = QLocale().toString(QDateTime::currentDateTime(), QLocale::ShortFormat);
        m_record.checked = zero == read && read > 0 ? tr("Read back in %1 places, the start and end included: all zeros (%2).").arg(read).arg(when)
                                                    : tr("Read back in %1 places: %2 weren't zeros (%3).").arg(read).arg(read - zero).arg(when);
        erasure::update(m_record);
        qCInfo(lcOps).noquote() << "Erasure check of" << m_record.device << ":" << m_record.checked;
        m_summary->setText(m_summary->text().replace(tr("Checking…"), m_record.checked.toHtmlEscaped()));
    });
}

CertificatesDialog::CertificatesDialog(UDisks *udisks, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_list(new QTreeWidget)
{
    setWindowTitle(tr("Erasure Certificates"));
    m_list->setHeaderLabels({tr("Finished"), tr("Drive"), tr("Serial"), tr("How")});
    m_list->setRootIsDecorated(false);
    m_list->setObjectName(QStringLiteral("erasures"));
    auto *open = new QPushButton(tr("Certificate…"));
    connect(open, &QPushButton::clicked, this, [this] {
        if (QTreeWidgetItem *item = m_list->currentItem()) {
            const QVector<erasure::Record> all = erasure::all();
            const int index = item->data(0, Qt::UserRole).toInt();
            if (index >= 0 && index < all.size())
                CertificateDialog(m_udisks, all.at(index), this).exec();
            fill();
        }
    });
    connect(m_list, &QTreeWidget::itemDoubleClicked, open, &QPushButton::click);
    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(open);
    buttons->addWidget(close);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("Every Wipe Disk and Secure Erase that finished here, newest first: a certificate of erasure can "
                                       "be saved for each, for a customer or the drive's next owner.")));
    layout->addWidget(m_list, 1);
    layout->addLayout(buttons);
    resize(720, 400);
    fill();
}

void CertificatesDialog::fill()
{
    m_list->clear();
    const QVector<erasure::Record> all = erasure::all();
    for (int i = 0; i < all.size(); ++i) {
        const erasure::Record &r = all.at(i);
        auto *item = new QTreeWidgetItem(m_list, {QLocale().toString(r.finished, QLocale::ShortFormat), QStringLiteral("%1 (%2)").arg(r.model, formatSize(r.size)),
                                                  r.serial, r.method});
        item->setData(0, Qt::UserRole, i);
    }
    for (int c = 0; c < m_list->columnCount(); ++c)
        m_list->resizeColumnToContents(c);
}
