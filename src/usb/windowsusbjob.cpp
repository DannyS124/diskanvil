// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "windowsusbjob.h"

#include "applog.h"
#include "blockio.h"
#include "filecopy.h"
#include "jobui.h"
#include "udisks.h"
#include "usbprep.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>

#include <atomic>

int WindowsUsbJob::splitMiB = windowsusb::kSplitMiB;

WindowsUsbJob::WindowsUsbJob(UDisks *udisks, const QString &diskPath, const QString &isoRoot, const QString &label,
                             const windowsusb::Info &info, const windowsusb::Options &options, QObject *parent)
    : QObject(parent)
    , m_udisks(udisks)
    , m_disk(diskPath)
    , m_iso(isoRoot)
    , m_label(label)
    , m_info(info)
    , m_options(options)
{
}

WindowsUsbJob::~WindowsUsbJob()
{
    cancel();
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
    if (m_wimlib) {
        m_wimlib->kill();
        m_wimlib->waitForFinished(3000);
    }
}

bool WindowsUsbJob::canCancel() const
{
    return m_copier || m_wimlib || m_stopCheck;
}

void WindowsUsbJob::cancel()
{
    m_cancelled = true;
    if (auto *copier = qobject_cast<filecopy::Copier *>(m_copier))
        copier->cancel();
    if (m_stopCheck)
        *m_stopCheck = true;
    if (m_wimlib)
        m_wimlib->terminate();
}

void WindowsUsbJob::start()
{
    qCInfo(lcOps).noquote() << "Make a Windows USB on" << m_disk << "from" << m_iso << m_info.wim.name << "build" << m_info.wim.build
                            << (m_info.needsSplit() ? "splitting" : "") << m_info.install;
    const Disk *d = m_udisks->diskByPath(m_disk);
    if (!d)
        return fail(tr("The stick isn't there anymore."));
    // One FAT32 partition, marked bootable (some PCs only list a stick in their boot menu then).
    m_prep = new UsbPrep(m_udisks, m_disk, QStringLiteral("dos"), {{QStringLiteral("vfat"), m_label, 0, 0x80, true}}, this);
    connect(m_prep, &UsbPrep::phase, this, &WindowsUsbJob::phase);
    connect(m_prep, &UsbPrep::failed, this, [this](const QString &message, bool shownAlready) { emit finished(false, message, shownAlready); });
    connect(m_prep, &UsbPrep::ready, this, [this](const QStringList &mountPoints) { copy(mountPoints.value(0)); });
    connect(m_prep, &UsbPrep::done, this, [this] {
        if (!m_failure.isEmpty()) {
            qCInfo(lcOps).noquote() << "Make a Windows USB failed:" << m_failure;
            emit finished(false, m_failure, false);
            return;
        }
        qCInfo(lcOps).noquote() << "Make a Windows USB finished";
        emit finished(true, tr("The Windows USB is ready. Start the PC from it (usually F12, F11, F9 or Esc at power-on) "
                               "and pick the UEFI entry for the stick. Secure Boot can stay on."),
                      false);
    });
    m_prep->start();
}

// Ends a failed step: the stick is unmounted either way, then finished() says what went wrong.
void WindowsUsbJob::fail(const QString &message)
{
    m_failure = m_cancelled ? filecopy::stoppedMessage() : message;
    m_prep->finish();
}

void WindowsUsbJob::copy(const QString &stick)
{
    m_stick = stick;
    m_source = filecopy::openFolder(m_iso);
    filecopy::Options options;
    const QString install = m_info.install;
    if (m_info.needsSplit())
        options.skip = [install](const QString &path) { return path.compare(install, Qt::CaseInsensitive) == 0; };
    auto *copier = new filecopy::Copier(m_source.get(), stick, options);
    m_copier = copier;
    connect(copier, &filecopy::Copier::progress, this, &WindowsUsbJob::progress);
    connect(copier, &filecopy::Copier::finished, this, [this](bool ok, const QString &message) {
        m_thread->quit();
        m_thread->wait(); // the copier is done with the source after this
        m_thread = nullptr;
        m_copier = nullptr;
        m_source.reset();
        if (!ok)
            return fail(message);
        if (m_info.needsSplit())
            split();
        else
            writeAnswers();
    });
    m_thread = startOnThread(this, copier);
}

void WindowsUsbJob::runWimlib(const QStringList &args, const QString &phaseText, const std::function<void()> &next)
{
    // wimlib says how far along it is as "... (52%) ...", over and over on one line; Windows'
    // DISM as "[====   52.0%   ]".
    m_wimlib = new QProcess(this);
    m_wimlib->setProcessChannelMode(QProcess::MergedChannels);
    auto *log = new QString;
    connect(m_wimlib, &QProcess::readyRead, this, [this, log, phaseText] {
        const QString text = QString::fromLocal8Bit(m_wimlib->readAll());
        log->append(text);
        if (log->size() > 64 * 1024)
            log->remove(0, log->size() - 32 * 1024);
        static const QRegularExpression percent(QStringLiteral("(\\d+)(?:\\.\\d+)?%"));
        QRegularExpressionMatchIterator it = percent.globalMatch(text);
        int last = -1;
        while (it.hasNext())
            last = it.next().captured(1).toInt();
        if (last >= 0)
            emit progress(phaseText, quint64(last), 100);
    });
    connect(m_wimlib, &QProcess::finished, this, [this, log, next](int code, QProcess::ExitStatus status) {
        const QString output = log->trimmed().section(QLatin1Char('\n'), -3);
        delete log;
        m_wimlib->deleteLater();
        m_wimlib = nullptr;
        if (status != QProcess::NormalExit || code != 0) {
#ifdef Q_OS_WIN
            return fail(tr("Windows couldn't split install.wim: %1").arg(output.isEmpty() ? tr("it stopped") : output));
#else
            return fail(tr("wimlib couldn't finish: %1").arg(output.isEmpty() ? tr("it stopped") : output));
#endif
        }
        next();
    });
    connect(m_wimlib, &QProcess::errorOccurred, this, [this, log](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        delete log;
        m_wimlib->deleteLater();
        m_wimlib = nullptr;
        fail(tr("wimlib isn't installed (it's called wimlib, or wimtools on Debian and Ubuntu)."));
    });
#ifdef Q_OS_WIN
    const QString program = QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"))).filePath(QStringLiteral("System32/dism.exe"));
#else
    const QString program = QStringLiteral("wimlib-imagex");
#endif
    qCInfo(lcOps).noquote() << program << args.join(QLatin1Char(' '));
    emit progress(phaseText, 0, 100);
    m_wimlib->start(program, args);
}

void WindowsUsbJob::split()
{
    // Straight onto the stick: install.swm, install2.swm... Windows Setup reads them as they are.
    // (Joined with QDir: on Windows the stick's root is "F:/", with its slash.)
    const QString target = QDir(m_stick).filePath(QStringLiteral("sources/install.swm"));
    QDir(m_stick).mkpath(QStringLiteral("sources"));
#ifdef Q_OS_WIN
    // With Windows' own DISM, which is always there.
    runWimlib({QStringLiteral("/Split-Image"), QStringLiteral("/ImageFile:") + QDir::toNativeSeparators(QDir(m_iso).filePath(m_info.install)),
               QStringLiteral("/SWMFile:") + QDir::toNativeSeparators(target), QStringLiteral("/FileSize:%1").arg(splitMiB)},
              tr("Splitting install.wim onto the stick"), [this] { verify(); });
#else
    runWimlib({QStringLiteral("split"), QDir(m_iso).filePath(m_info.install), target, QString::number(splitMiB)},
              tr("Splitting install.wim onto the stick"), [this] { verify(); });
#endif
}

void WindowsUsbJob::verify()
{
#ifdef Q_OS_WIN
    // DISM has no check of its own: the parts are compared with install.wim, read back from the
    // stick itself.
    QStringList parts;
    const QDir sources(QDir(m_stick).filePath(QStringLiteral("sources")));
    for (const QString &part : sources.entryList({QStringLiteral("install*.swm")}, QDir::Files))
        parts << sources.filePath(part);
    const QString original = QDir(m_iso).filePath(m_info.install);
    const QString phaseText = tr("Checking install.wim's parts on the stick");
    auto stop = std::make_shared<std::atomic_bool>(false);
    m_stopCheck = stop;
    qCInfo(lcOps).noquote() << "Checking" << parts.size() << "parts against" << original;
    m_thread = QThread::create([this, original, parts, phaseText, stop] {
        QElapsedTimer since;
        since.start();
        const QString error = windowsusb::checkParts(original, parts, [this, phaseText, stop, &since](quint64 done, quint64 total) {
            if (since.elapsed() > 200 || done == total) {
                since.restart();
                QMetaObject::invokeMethod(this, [this, phaseText, done, total] { emit progress(phaseText, done, total); }, Qt::QueuedConnection);
            }
            return !stop->load();
        });
        QMetaObject::invokeMethod(this, [this, error] {
            m_thread->wait();
            m_thread->deleteLater();
            m_thread = nullptr;
            m_stopCheck.reset();
            if (!error.isEmpty())
                return fail(error);
            qCInfo(lcOps).noquote() << "The parts match install.wim";
            writeAnswers();
        }, Qt::QueuedConnection);
    });
    m_thread->setParent(this);
    m_thread->start();
#else
    // Flushed and out of the page cache first, so the check reads the stick itself.
    const QStringList parts = QDir(m_stick + QStringLiteral("/sources")).entryList({QStringLiteral("install*.swm")}, QDir::Files);
    for (const QString &part : parts) {
        const int fd = blockio::openFile(m_stick + QStringLiteral("/sources/") + part, blockio::FileMode::Read);
        if (fd < 0)
            continue;
        blockio::flush(fd);
        blockio::dropCache(fd);
        blockio::close(fd);
    }
    runWimlib({QStringLiteral("verify"), m_stick + QStringLiteral("/sources/install.swm"),
               QStringLiteral("--ref=%1/sources/install*.swm").arg(m_stick)},
              tr("Checking install.wim's parts on the stick"), [this] { writeAnswers(); });
#endif
}

void WindowsUsbJob::writeAnswers()
{
    const QByteArray xml = windowsusb::unattendXml(m_options, m_info.wim.arch);
    if (!xml.isEmpty()) {
        QFile f(QDir(m_stick).filePath(QStringLiteral("autounattend.xml")));
        if (!f.open(QIODevice::WriteOnly) || f.write(xml) != xml.size())
            return fail(tr("Couldn't write autounattend.xml: %1").arg(f.errorString()));
        f.close();
    }
    blockio::syncFileSystem(m_stick);
    m_prep->finish();
}
