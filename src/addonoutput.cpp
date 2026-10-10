// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "addonoutput.h"

#include "dialogs.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QVBoxLayout>

#include <signal.h>

#ifdef Q_OS_WIN
// Windows has no process groups to signal; Stop ends the program itself.
#define SIGKILL 9
#endif

namespace {

constexpr int kMaxLines = 10000;   // older lines scroll off the top
constexpr int kMaxPending = 5000;  // a command printing faster than we can show it skips some

} // namespace

AddonOutputWindow::AddonOutputWindow(const QString &title, const QStringList &command, QWidget *parent)
    : QDialog(parent)
    , m_title(title)
    , m_text(new QPlainTextEdit)
    , m_status(new QLabel)
    , m_stop(new QPushButton(QIcon::fromTheme(QStringLiteral("process-stop")), tr("Stop")))
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(title);
    m_text->setReadOnly(true);
    m_text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_text->setMaximumBlockCount(kMaxLines);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setText(tr("Running…"));

    auto *copy = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-copy")), tr("Copy All"));
    auto *save = new QPushButton(QIcon::fromTheme(QStringLiteral("document-save")), tr("Save…"));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->addButton(m_stop, QDialogButtonBox::ActionRole);
    buttons->addButton(copy, QDialogButtonBox::ActionRole);
    buttons->addButton(save, QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(m_stop, &QPushButton::clicked, this, &AddonOutputWindow::stop);
    connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(text()); });
    connect(save, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getSaveFileName(this, tr("Save Output"), QDir::homePath() + QStringLiteral("/output.txt"));
        if (file.isEmpty())
            return;
        QSaveFile f(file);
        const QByteArray data = text().toUtf8();
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
            warnPlain(this, windowTitle(), tr("Couldn't save %1").arg(file));
    });

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_text, 1);
    layout->addWidget(m_status);
    layout->addWidget(buttons);
    resize(760, 480);

    // Showing text is batched: a command that prints nonstop can't freeze the window.
    m_flush.setInterval(100);
    connect(&m_flush, &QTimer::timeout, this, &AddonOutputWindow::flush);
    m_kill.setSingleShot(true);
    connect(&m_kill, &QTimer::timeout, this, [this] { signalGroup(SIGKILL); });

#ifndef Q_OS_WIN
    // Its own session, so Stop reaches everything it starts; only its output is inherited.
    QProcess::UnixProcessParameters params;
    params.flags = QProcess::UnixProcessFlag::CreateNewSession | QProcess::UnixProcessFlag::CloseFileDescriptors
        | QProcess::UnixProcessFlag::ResetSignalHandlers;
    m_process.setUnixProcessParameters(params);
#endif
    m_process.setProcessChannelMode(QProcess::MergedChannels);
    m_process.setStandardInputFile(QProcess::nullDevice());
    connect(&m_process, &QProcess::readyRead, this, &AddonOutputWindow::readOutput);
    connect(&m_process, &QProcess::finished, this, &AddonOutputWindow::finished);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_status->setText(tr("Couldn't start it: %1").arg(m_process.errorString()));
            m_stop->setEnabled(false);
        }
    });
    if (!command.isEmpty()) {
        m_process.start(command.first(), command.mid(1));
        m_flush.start();
    }
}

AddonOutputWindow::~AddonOutputWindow()
{
    if (isRunning()) {
        signalGroup(SIGKILL);
        m_process.waitForFinished(2000);
    }
}

void AddonOutputWindow::signalGroup(int signal)
{
#ifdef Q_OS_WIN
    Q_UNUSED(signal)
    m_process.kill();
#else
    const qint64 pid = m_process.processId();
    if (pid > 0)
        ::kill(-pid_t(pid), signal); // the whole session: CreateNewSession made it the group leader
#endif
}

void AddonOutputWindow::stop()
{
    if (!isRunning())
        return;
    m_stopped = true;
    m_status->setText(tr("Stopping…"));
    signalGroup(SIGTERM);
    m_kill.start(3000); // whatever ignores SIGTERM gets SIGKILL
}

QString AddonOutputWindow::text() const
{
    return m_text->toPlainText();
}

void AddonOutputWindow::readOutput()
{
    m_pending += m_filter.feed(m_process.readAll());
    if (m_pending.size() > kMaxPending) {
        m_skipped += int(m_pending.size()) - kMaxPending;
        m_pending.erase(m_pending.begin(), m_pending.end() - kMaxPending);
    }
}

void AddonOutputWindow::flush()
{
    const QString current = m_filter.current();
    if (m_pending.isEmpty() && m_skipped == 0 && current == m_shownCurrent)
        return;
    QScrollBar *bar = m_text->verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum() - 4;
    // The unfinished line (a progress bar, say) is the last block; take it away first.
    if (!m_shownCurrent.isEmpty()) {
        QTextCursor cursor(m_text->document());
        cursor.movePosition(QTextCursor::End);
        cursor.select(QTextCursor::BlockUnderCursor);
        cursor.removeSelectedText();
        if (m_text->document()->blockCount() == 1 && m_text->document()->firstBlock().text().isEmpty())
            m_text->clear();
    }
    if (m_skipped > 0) {
        m_text->appendPlainText(tr("… (%n line(s) skipped)", nullptr, m_skipped));
        m_skipped = 0;
    }
    for (const QString &line : std::as_const(m_pending))
        m_text->appendPlainText(line);
    m_pending.clear();
    if (!current.isEmpty())
        m_text->appendPlainText(current);
    m_shownCurrent = current;
    if (atBottom)
        bar->setValue(bar->maximum());
}

void AddonOutputWindow::finished(int code, QProcess::ExitStatus status)
{
    readOutput();
    flush();
    m_flush.stop();
    m_kill.stop();
    m_stop->setEnabled(false);
    if (m_stopped)
        m_status->setText(tr("Stopped."));
    else if (status == QProcess::CrashExit)
        m_status->setText(tr("It stopped unexpectedly."));
    else if (code == 0)
        m_status->setText(tr("Finished."));
    else
        m_status->setText(tr("Finished with an error (exit code %1).").arg(code));
}

void AddonOutputWindow::closeEvent(QCloseEvent *event)
{
    if (isRunning() && !askPlain(this, m_title, tr("\"%1\" is still running. Stop it and close?").arg(m_title))) {
        event->ignore();
        return;
    }
    stop();
    event->accept();
}

void AddonOutputWindow::reject()
{
    close(); // Esc goes through closeEvent too
}
