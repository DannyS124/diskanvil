// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "previewloader.h"

#include "applog.h"
#include "filetypes.h"
#include "hostprocess.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

PreviewLoader::PreviewLoader(std::shared_ptr<lost::Source> source)
    : m_source(std::move(source))
{
}

PreviewLoader::~PreviewLoader() = default;

QStringList PreviewLoader::helperCommand(bool sandbox)
{
    const QString app = QCoreApplication::applicationFilePath();
#ifdef Q_OS_LINUX
    const QString bwrap = QStandardPaths::findExecutable(QStringLiteral("bwrap"));
    if (sandbox && !bwrap.isEmpty() && !host::inFlatpak()) {
        QStringList command = {bwrap,
                               QStringLiteral("--ro-bind"), QStringLiteral("/"), QStringLiteral("/"),
                               QStringLiteral("--dev"), QStringLiteral("/dev"),
                               QStringLiteral("--proc"), QStringLiteral("/proc"),
                               QStringLiteral("--tmpfs"), QStringLiteral("/tmp"),
                               QStringLiteral("--tmpfs"), QStringLiteral("/run"),
                               QStringLiteral("--tmpfs"), QStringLiteral("/home"),
                               QStringLiteral("--tmpfs"), QStringLiteral("/root"),
                               QStringLiteral("--unshare-all"),
                               QStringLiteral("--die-with-parent"),
                               QStringLiteral("--new-session"),
                               QStringLiteral("--cap-drop"), QStringLiteral("ALL")};
        // The program itself can be in one of those (a build in the home folder, an AppImage
        // mounted in /tmp): that one folder comes back, read-only.
        const QString folder = qEnvironmentVariable("APPDIR", QFileInfo(app).absolutePath());
        command << QStringLiteral("--ro-bind") << folder << folder;
        command << QStringLiteral("--") << app << QStringLiteral("--preview-helper");
        return command;
    }
#else
    Q_UNUSED(sandbox)
#endif
    return {app, QStringLiteral("--preview-helper")};
}

void PreviewLoader::want(int index, const lost::Found &file, int pixels)
{
    QList<Wish> gone;
    {
        QMutexLocker locker(&m_lock);
        // The same wish again goes to the front instead of being in the queue twice.
        m_queue.removeIf([&](const Wish &w) { return w.index == index && w.pixels == pixels; });
        m_queue.prepend({index, file, pixels});
        // Long scrolls pile up wishes: the oldest ones aren't on screen anymore.
        while (m_queue.size() > 300)
            gone << m_queue.takeLast();
        m_wake.wakeOne();
    }
    for (const Wish &w : std::as_const(gone))
        Q_EMIT dropped(w.index, w.pixels);
}

void PreviewLoader::stop()
{
    QMutexLocker locker(&m_lock);
    m_stop = true;
    m_wake.wakeOne();
}

void PreviewLoader::run()
{
    for (;;) {
        Wish wish;
        {
            QMutexLocker locker(&m_lock);
            while (m_queue.isEmpty() && !m_stop)
                m_wake.wait(&m_lock);
            if (m_stop)
                break;
            wish = m_queue.takeFirst();
        }
        const lost::Found &f = wish.file;
        // A thumbnail of a song or a film only needs its start (the tags, the first frame).
        const filetypes::Category category = f.type >= 0 ? filetypes::types()[f.type].category : filetypes::Category::Other;
        const bool media = category == filetypes::Category::Music || category == filetypes::Category::Video;
        const bool pdf = f.type >= 0 && filetypes::types()[f.type].id == QLatin1String("pdf");
        const qint64 most = wish.pixels <= 160 && media ? 16 * 1024 * 1024 : pdf ? preview::kMostPdfData : preview::kMostData;
        preview::Request request;
        request.type = f.type;
        request.name = f.name;
        request.size = f.size;
        request.pixels = wish.pixels;
        request.data = lost::FileReader(m_source, f).head(most);
        bool failed = false;
        const preview::Result result = ask(request, &failed);
        Q_EMIT ready(wish.index, wish.pixels, result.image, result.text, failed);
    }
    m_helper.reset();
}

bool PreviewLoader::startHelper()
{
    for (;;) {
        m_sandboxed = !m_noSandbox;
        const QStringList command = helperCommand(m_sandboxed);
        m_sandboxed = m_sandboxed && command.first() != QCoreApplication::applicationFilePath();
        m_helper = std::make_unique<QProcess>();
        m_helper->setProcessChannelMode(QProcess::SeparateChannels);
        m_helper->setStandardErrorFile(QProcess::nullDevice());
        m_helper->start(command.first(), command.mid(1));
        bool ok = m_helper->waitForStarted(5000);
        // A sandbox that's never been seen to work gets an empty question first: some systems
        // don't let bwrap make one, and the helper then runs without it.
        if (ok && m_sandboxed && !m_sandboxWorks) {
            preview::write(*m_helper, preview::Request());
            preview::Result answer;
            ok = m_helper->waitForBytesWritten(5000) && preview::read(*m_helper, answer, 5000);
            m_sandboxWorks = ok;
            if (!ok) {
                m_noSandbox = true;
                qCInfo(lcOps).noquote() << "Find Lost Files: bwrap couldn't make a sandbox here; previews are made without one";
                m_helper->kill();
                m_helper->waitForFinished(2000);
                m_helper.reset();
                continue;
            }
        }
        if (!ok)
            m_helper.reset();
        return ok;
    }
}

preview::Result PreviewLoader::ask(const preview::Request &request, bool *failed)
{
    if (!m_helper && !startHelper()) {
        *failed = true;
        return {};
    }
    preview::write(*m_helper, request);
    QDeadlineTimer deadline(kTimeout);
    while (m_helper->bytesToWrite() > 0 && m_helper->state() == QProcess::Running && !deadline.hasExpired())
        m_helper->waitForBytesWritten(int(std::min<qint64>(deadline.remainingTime(), 1000)));
    preview::Result result;
    if (preview::read(*m_helper, result, int(std::max<qint64>(1, deadline.remainingTime()))))
        return result;
    // It crashed or hung on this file: that file gets no preview, the next one a new helper.
    m_helper->kill();
    m_helper->waitForFinished(2000);
    m_helper.reset();
    *failed = true;
    return {};
}
