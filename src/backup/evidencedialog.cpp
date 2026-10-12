// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "evidencedialog.h"

#include "applog.h"
#include "blockio.h"
#include "dialogs.h"
#include "format.h"
#include "jobui.h"
#include "pdfreport.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QThread>
#include <QVBoxLayout>

EvidenceDialog::EvidenceDialog(UDisks *udisks, const Disk &disk, const Volume *volume, QWidget *parent)
    : QDialog(parent)
    , m_udisks(udisks)
    , m_disk(disk)
    , m_case(new QLineEdit)
    , m_evidence(new QLineEdit)
    , m_description(new QLineEdit)
    , m_examiner(new QLineEdit)
    , m_file(new QLineEdit)
    , m_notes(new QPlainTextEdit)
    , m_status(new QLabel)
    , m_bar(new QProgressBar)
    , m_start(new QPushButton(tr("Start")))
{
    setWindowTitle(tr("Make an Evidence Image"));
    m_objectPath = volume ? volume->objectPath : disk.blockPath;
    m_device = volume ? volume->device : disk.device;
    m_size = volume ? volume->size : disk.size;
    m_title = volume ? volumeTitle(*volume) : diskTitle(disk);
    m_description->setText(volume ? tr("%1, partition %2 of %3 (serial %4)").arg(volumeTitle(*volume)).arg(volume->number).arg(disk.model, disk.serial)
                                  : tr("%1 (serial %2)").arg(disk.model, disk.serial));
    QSettings settings(QStringLiteral("diskanvil"), QStringLiteral("diskanvil"));
    m_examiner->setText(settings.value(QStringLiteral("evidence/examiner")).toString());
    m_file->setText(QDir::toNativeSeparators(QDir::home().filePath(QStringLiteral("%1.E01").arg(disk.serial.isEmpty() ? shortDevice(m_device) : disk.serial))));
    for (QLineEdit *e : {m_case, m_evidence, m_description, m_examiner, m_file})
        connect(e, &QLineEdit::textChanged, this, &EvidenceDialog::updateState);
    m_case->setObjectName(QStringLiteral("case"));
    m_evidence->setObjectName(QStringLiteral("evidence"));
    m_examiner->setObjectName(QStringLiteral("examiner"));
    m_file->setObjectName(QStringLiteral("file"));
    m_notes->setMaximumHeight(70);
    m_status->setWordWrap(true);
    m_status->setObjectName(QStringLiteral("status"));
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_bar->setRange(0, 1000);
    m_bar->hide();
    m_start->setObjectName(QStringLiteral("start"));
    auto *browse = new QPushButton(tr("Browse…"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getSaveFileName(this, tr("Evidence File"), m_file->text(), tr("Evidence files (*.E01)"));
        if (!file.isEmpty())
            m_file->setText(QDir::toNativeSeparators(file.endsWith(QLatin1String(".E01"), Qt::CaseInsensitive) ? file : file + QStringLiteral(".E01")));
    });
    connect(m_start, &QPushButton::clicked, this, &EvidenceDialog::start);
    auto *close = new QPushButton(tr("Close"));
    connect(close, &QPushButton::clicked, this, &EvidenceDialog::reject);
    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(m_file, 1);
    fileRow->addWidget(browse);
    auto *form = new QFormLayout;
    form->addRow(tr("Case number:"), m_case);
    form->addRow(tr("Evidence number:"), m_evidence);
    form->addRow(tr("Description:"), m_description);
    form->addRow(tr("Examiner:"), m_examiner);
    form->addRow(tr("Notes:"), m_notes);
    form->addRow(tr("Evidence file:"), fileRow);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(m_start);
    buttons->addWidget(close);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(wrappingLabel(tr("<b>%1</b><br>Every sector, read once and never written to, into an evidence file (.E01, EnCase's "
                                       "format) with the case's details and its MD5 and SHA-1, as forensic tools expect. It's checked "
                                       "once written, and a case report (PDF) goes next to it. Sectors that can't be read are kept as "
                                       "zeros and listed.")
                                        .arg(m_title.toHtmlEscaped())));
    layout->addLayout(form);
    layout->addWidget(m_bar);
    layout->addWidget(m_status);
    layout->addLayout(buttons);
    resize(600, sizeHint().height());
    updateState();
}

EvidenceDialog::~EvidenceDialog()
{
    if (m_job)
        m_job->cancel();
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
}

void EvidenceDialog::reject()
{
    if (m_job) {
        m_job->cancel();
        return;
    }
    QDialog::reject();
}

void EvidenceDialog::updateState()
{
    const QString file = QDir::fromNativeSeparators(m_file->text().trimmed());
    QString problem;
    if (file.isEmpty())
        problem = tr("Where should it go?");
    else if (!QFileInfo(QFileInfo(file).absolutePath()).isDir())
        problem = tr("That folder isn't there.");
    else if (blockio::pathIsOnDisk(QFileInfo(file).absolutePath(), m_disk.device))
        problem = tr("It can't go on the drive it's made from.");
    // Only what this put there is cleared: not the outcome of a run.
    if (!problem.isEmpty())
        m_status->setText(redText(problem));
    else if (m_problemShown)
        m_status->clear();
    m_problemShown = !problem.isEmpty();
    m_start->setEnabled(problem.isEmpty() && !m_job && !m_case->text().trimmed().isEmpty());
}

QString EvidenceDialog::reportHtml(const ewf::Case &c, const ewf::Result &r, const QString &source, const QString &file, const QString &checked)
{
    const QLocale l;
    const auto row = [](const QString &name, const QString &value) {
        return QStringLiteral("<tr><td style=\"padding:3px 14px 3px 0; color:#555\">%1</td><td style=\"padding:3px 0\"><b>%2</b></td></tr>")
            .arg(name.toHtmlEscaped(), value.toHtmlEscaped());
    };
    QString html = QStringLiteral("<html><body style=\"font-family:sans-serif; font-size:10pt\"><h1 style=\"font-size:19pt\">%1</h1>")
                       .arg(tr("Evidence Acquisition Report").toHtmlEscaped());
    html += QStringLiteral("<h2 style=\"font-size:12pt\">%1</h2><table>").arg(tr("The case").toHtmlEscaped());
    html += row(tr("Case number"), c.caseNumber) + row(tr("Evidence number"), c.evidenceNumber) + row(tr("Description"), c.description)
          + row(tr("Examiner"), c.examiner) + row(tr("Notes"), c.notes);
    html += QStringLiteral("</table><h2 style=\"font-size:12pt\">%1</h2><table>").arg(tr("The source").toHtmlEscaped());
    html += row(tr("Drive"), c.model) + row(tr("Serial number"), c.serial) + row(tr("Read from"), source)
          + row(tr("Size"), QStringLiteral("%1 (%2 bytes)").arg(formatSize(r.bytes)).arg(r.bytes))
          + row(tr("How"), tr("Opened read-only: nothing was written to it"));
    html += QStringLiteral("</table><h2 style=\"font-size:12pt\">%1</h2><table>").arg(tr("The evidence file").toHtmlEscaped());
    html += row(tr("File"), file) + row(tr("Its size"), formatSize(r.fileSize)) + row(tr("Format"), tr("Expert Witness (E01), EnCase 6's layout"))
          + row(tr("Started"), l.toString(r.started, QLocale::LongFormat)) + row(tr("Finished"), l.toString(r.finished, QLocale::LongFormat))
          + row(QStringLiteral("MD5"), QString::fromLatin1(r.md5.toHex())) + row(QStringLiteral("SHA-1"), QString::fromLatin1(r.sha1.toHex()))
          + row(QStringLiteral("SHA-256"), QString::fromLatin1(r.sha256.toHex())) + row(tr("Checked"), checked);
    QStringList bad;
    for (const auto &[sector, count] : r.unreadable)
        bad << (count == 1 ? QString::number(sector) : QStringLiteral("%1-%2").arg(sector).arg(sector + count - 1));
    html += row(tr("Unreadable sectors"), bad.isEmpty() ? tr("None") : tr("%1 (kept as zeros)").arg(bad.join(QStringLiteral(", "))));
    html += row(tr("Made with"), QCoreApplication::applicationName() + QLatin1Char(' ') + QCoreApplication::applicationVersion());
    html += QStringLiteral("</table><p style=\"margin-top:40px\">%1 ____________________________ &nbsp;&nbsp; %2 ______________</p></body></html>")
                .arg(tr("Examiner's signature").toHtmlEscaped(), tr("Date").toHtmlEscaped());
    return html;
}

void EvidenceDialog::start()
{
    const QString file = QDir::fromNativeSeparators(m_file->text().trimmed());
    QSettings(QStringLiteral("diskanvil"), QStringLiteral("diskanvil")).setValue(QStringLiteral("evidence/examiner"), m_examiner->text().trimmed());
    const ewf::Case details{m_case->text().trimmed(), m_evidence->text().trimmed(), m_description->text().trimmed(), m_examiner->text().trimmed(),
                            m_notes->toPlainText().trimmed(), m_disk.model, m_disk.serial};
    m_start->setEnabled(false);
    m_status->setText(tr("Waiting for permission…"));
    // Read-only, kept still while it's read: unmounted first (never the running system's).
    openBlockThen(m_udisks, this, m_objectPath, UDisks::OpenMode::Read, [this, file, details](int fd) {
        if (fd < 0) {
            m_status->setText(redText(tr("The drive couldn't be opened to read it.")));
            updateState();
            return;
        }
        qCInfo(lcOps).noquote() << "Evidence image of" << m_device << "to" << file;
        m_bar->show();
        m_bar->setValue(0);
        m_status->setText(tr("Reading…"));
        m_job = new ewf::Acquire(fd, m_size, std::max(m_disk.sectorSize, 512), file, details);
        connect(m_job, &ewf::Acquire::progress, this, [this](quint64 done, quint64 total) { m_bar->setValue(total ? int(done * 1000 / total) : 0); });
        connect(m_job, &ewf::Acquire::finished, this, [this, file, details](const ewf::Result &r) {
            m_thread->quit();
            m_thread->wait();
            m_thread = nullptr;
            m_job = nullptr;
            if (!r.ok) {
                m_status->setText(redText(r.error.toHtmlEscaped()));
                updateState();
                return;
            }
            // Read back from the file, against its own MD5, then the report next to it.
            m_status->setText(tr("Checking the evidence file…"));
            QApplication::setOverrideCursor(Qt::WaitCursor);
            const QString problem = ewf::verify(file);
            QApplication::restoreOverrideCursor();
            const QString checked = problem.isEmpty() ? tr("Read back from the file: its MD5 matches") : problem;
            const QString report = file.left(file.size() - 4) + QStringLiteral(" report.pdf");
            QString error;
            pdfreport::save(reportHtml(details, r, m_device, QDir::toNativeSeparators(file), checked), report, tr("Evidence Acquisition Report"), &error);
            qCInfo(lcOps).noquote() << "Evidence image done: md5" << r.md5.toHex() << "sha1" << r.sha1.toHex() << checked;
            m_bar->setValue(1000);
            QString text = tr("Done. MD5 %1, SHA-1 %2.").arg(QString::fromLatin1(r.md5.toHex()), QString::fromLatin1(r.sha1.toHex())) + QLatin1Char(' ') + checked
                         + QStringLiteral(". ") + tr("The report: %1").arg(QDir::toNativeSeparators(report));
            if (!r.unreadable.isEmpty())
                text += QLatin1Char(' ') + tr("%n stretch(es) of sectors couldn't be read: kept as zeros, and listed in the report.", nullptr,
                                              int(r.unreadable.size()));
            m_status->setText(problem.isEmpty() ? text.toHtmlEscaped() : redText(text.toHtmlEscaped()));
            updateState();
        });
        m_thread = startOnThread(this, m_job);
    });
}
