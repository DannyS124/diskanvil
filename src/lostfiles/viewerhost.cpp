// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "viewerhost.h"

#include "applog.h"
#include "blockio.h"
#include "format.h"
#include "hostprocess.h"

#include <QApplication>
#include <QIcon>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLocale>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QSaveFile>
#include <QStyle>
#include <QStandardPaths>
#include <QTimer>
#include <QWidget>

#include <algorithm>

ViewerHost::ViewerHost(QWidget *window)
    : QObject(window)
    , m_window(window)
{
}

ViewerHost::~ViewerHost()
{
    stop();
}

QString ViewerHost::profilesFolder() const
{
    return m_profilesFolder.isEmpty() ? enhance::profilesFolder() : m_profilesFolder;
}

void ViewerHost::show(const viewer::Show &show, const QStringList &readable)
{
    // Somewhere its sandbox can't see (a folder picked since it started): it starts again.
    const bool seen = std::any_of(m_readable.cbegin(), m_readable.cend(),
                                  [&show](const QString &folder) { return show.path.startsWith(folder + QLatin1Char('/')); });
    if (m_process && !seen) {
        stop();
    }
    if (!m_process) {
        m_readable = readable;
        start();
    }
    m_shown = show;
    viewer::Message m;
    m.type = viewer::Message::ShowFile;
    m.show = show;
    send(viewer::encode(m));
}

void ViewerHost::sendFacts(const QStringList &facts)
{
    if (!m_process)
        return;
    m_shown.facts = facts;
    viewer::Message m;
    m.type = viewer::Message::Facts;
    m.show.facts = facts;
    send(viewer::encode(m));
}

void ViewerHost::start()
{
    m_ready = false;
    m_heard.clear();
    m_waiting.clear();
    auto *process = new QProcess(this);
    m_process = process;
    process->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] { heard(process->readAllStandardOutput()); });
    connect(process, &QProcess::finished, this, [this, process] {
        process->deleteLater();
        if (process != m_process)
            return;
        const bool neverStarted = !m_ready;
        m_process = nullptr;
        m_ready = false;
        // The sandbox didn't work here (some systems don't allow one): it runs without.
        if (neverStarted && m_sandboxed && !m_shown.path.isEmpty()) {
            qCInfo(lcOps).noquote() << "Viewer: it couldn't start in its sandbox here; it runs without one";
            m_sandboxed = false;
            show(m_shown, m_readable);
        } else {
            Q_EMIT closed();
        }
    });
    const QStringList command = viewer::command(m_sandboxed, m_readable);
    process->start(command.first(), command.mid(1));
    // It looks like DiskAnvil does.
    viewer::Message look;
    look.type = viewer::Message::Look;
    look.style = QApplication::style()->name();
    look.palette = QApplication::palette();
    look.iconTheme = QIcon::themeName();
    look.fallbackIconTheme = QIcon::fallbackThemeName();
    send(viewer::encode(look));
    sendProfiles(QString());
    // A sandbox that hangs rather than failing is given up on after a while, the same way.
    QTimer::singleShot(15000, process, [this, process] {
        if (process == m_process && !m_ready)
            process->kill();
    });
}

void ViewerHost::stop()
{
    if (!m_process)
        return;
    QProcess *process = m_process;
    m_process = nullptr;
    m_ready = false;
    process->disconnect(this);
    // With nothing more to read, it closes by itself.
    process->closeWriteChannel();
    if (!process->waitForFinished(1500)) {
        process->kill();
        process->waitForFinished(1000);
    }
    process->deleteLater();
}

void ViewerHost::send(const QByteArray &message)
{
    if (m_process && m_ready)
        m_process->write(message);
    else
        m_waiting << message;
}

void ViewerHost::heard(const QByteArray &said)
{
    m_heard += said;
    viewer::Message m;
    bool bad = false;
    while (viewer::decode(m_heard, m, &bad)) {
        switch (m.type) {
        case viewer::Message::Edited:
            Q_EMIT edited(m.edits);
            break;
        case viewer::Message::SavePicture:
            savePicture(m.pictureName, m.picture);
            break;
        case viewer::Message::ExportFilm:
            exportFilm(m.speed, m.settings, m.durationMs);
            break;
        case viewer::Message::Profile:
            profileAsked(m.profileDo, m.profileName, m.profile, m.dropped);
            break;
        case viewer::Message::AskFor:
            if (m.ask == viewer::Ask::Ready) {
                m_ready = true;
                if (m_process) {
                    for (const QByteArray &waiting : std::as_const(m_waiting))
                        m_process->write(waiting);
                }
                m_waiting.clear();
            }
            Q_EMIT asked(m.ask);
            break;
        default:
            break;
        }
    }
    if (bad)
        stop();
}

bool ViewerHost::allowed(const QString &target)
{
    if (!m_guarded.isEmpty() && blockio::pathIsOnDisk(QFileInfo(target).absolutePath(), m_guarded)) {
        QMessageBox::warning(m_window, m_window ? m_window->windowTitle() : QString(),
                             tr("That's on the drive being searched: saving there could overwrite the lost files. Pick a folder on another drive."));
        return false;
    }
    return true;
}

void ViewerHost::savePicture(const QString &name, const QByteArray &png)
{
    // Made in the viewer (enhanced, or a film's frame): its bytes are written where the person
    // says, never read here.
    if (png.isEmpty() || !png.startsWith("\x89PNG") || !m_window)
        return;
    m_window->raise();
    m_window->activateWindow();
    const QString folder = m_saveFolder.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) : m_saveFolder;
    const QString target = m_saveTarget.isEmpty()
                               ? QFileDialog::getSaveFileName(m_window, tr("Save the Picture"), folder + QLatin1Char('/') + cleanName(name),
                                                              tr("PNG picture (*.png)"))
                               : m_saveTarget;
    if (target.isEmpty() || !allowed(target))
        return;
    QSaveFile out(target);
    if (out.open(QIODevice::WriteOnly) && out.write(png) == png.size() && out.commit()) {
        m_saveFolder = QFileInfo(target).absolutePath();
        Q_EMIT status(tr("Saved the picture as %1.").arg(QDir::toNativeSeparators(target)));
    } else {
        QMessageBox::warning(m_window, m_window->windowTitle(), tr("The picture couldn't be saved: %1").arg(out.errorString()));
    }
}

void ViewerHost::exportFilm(double speed, const enhance::Settings &s, qint64 durationMs)
{
    const QString copy = m_shown.path;
    if (copy.isEmpty() || !m_window)
        return;
    m_window->raise();
    m_window->activateWindow();
#ifdef Q_OS_WIN
    QString ffmpeg = host::windowsProgram(QStringLiteral("ffmpeg"));
    if (ffmpeg == QLatin1String("ffmpeg"))
        ffmpeg.clear();
#else
    QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
#endif
    // The Flatpak has no ffmpeg of its own: the PC's, when it's allowed to run it (as add-ons are).
    const bool onHost = ffmpeg.isEmpty() && host::inFlatpak() && host::programExists(QStringLiteral("ffmpeg"));
    if (onHost)
        ffmpeg = QStringLiteral("ffmpeg");
    if (ffmpeg.isEmpty()) {
        QMessageBox::information(m_window, m_window->windowTitle(),
                                 host::inFlatpak()
                                     ? tr("Saving a film at another speed needs ffmpeg. The Flatpak uses this PC's, when ffmpeg is installed and the "
                                          "Flatpak may run it:\nflatpak override --user --talk-name=org.freedesktop.Flatpak io.github.DannyS124.DiskAnvil\n"
                                          "A frame can still be saved, and the film played at any speed.")
                                     : tr("Saving a film at another speed needs ffmpeg, which isn't installed here. A frame can still be saved, "
                                          "and the film played at any speed."));
        return;
    }
    const QString folder = m_saveFolder.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) : m_saveFolder;
    QStringList how;
    if (speed != 1)
        how << tr("x%1").arg(QLocale().toString(speed));
    if (!s.isDefault())
        how << tr("enhanced");
    const QString suggested = folder + QLatin1Char('/') + cleanName(QFileInfo(m_shown.name).completeBaseName())
                            + (how.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(how.join(QStringLiteral(", ")))) + QStringLiteral(".mp4");
    const QString target = QFileDialog::getSaveFileName(m_window, tr("Save the Film"), suggested, tr("MP4 film (*.mp4)"));
    if (target.isEmpty() || !allowed(target))
        return;
    if (QFileInfo(target).absoluteFilePath() == QFileInfo(copy).absoluteFilePath()) {
        QMessageBox::warning(m_window, m_window->windowTitle(), tr("That's the film itself: save the copy under another name."));
        return;
    }
    // The PC's ffmpeg sees DiskAnvil's own cache folder (it's the same folder outside the Flatpak), not
    // always where the film is going: it's made there, then moved.
    const QString made = onHost ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/film-%1.mp4").arg(QCoreApplication::applicationPid())
                                : target;
    QDir().mkpath(QFileInfo(made).absolutePath());
    const QStringList command = host::command(QStringList{ffmpeg} + enhance::ffmpegArguments(copy, made, speed, s));

    auto *process = new QProcess(this);
    auto *wait = new QProgressDialog(tr("Making the film…"), tr("Stop"), 0, 1000, m_window);
    wait->setWindowModality(Qt::WindowModal);
    wait->setMinimumDuration(0);
    const qint64 total = durationMs > 0 ? qint64(durationMs / speed) * 1000 : 0; // microseconds, at the new speed
    connect(process, &QProcess::readyReadStandardOutput, wait, [process, wait, total] {
        for (const QByteArray &line : process->readAllStandardOutput().split('\n')) {
            if (line.startsWith("out_time_us=") && total > 0)
                wait->setValue(int(std::clamp<qint64>(line.mid(12).toLongLong() * 1000 / total, 0, 999)));
        }
    });
    connect(wait, &QProgressDialog::canceled, process, [process] { process->kill(); });
    connect(process, &QProcess::finished, this, [this, process, wait, target, made](int code, QProcess::ExitStatus exit) {
        const bool cancelled = wait->wasCanceled();
        wait->deleteLater();
        process->deleteLater();
        if (exit == QProcess::NormalExit && code == 0) {
            if (made != target) {
                QFile::remove(target);
                if (!QFile::rename(made, target) && !(QFile::copy(made, target) && QFile::remove(made))) {
                    QMessageBox::warning(m_window, m_window ? m_window->windowTitle() : QString(),
                                         tr("The film was made, but couldn't be moved to %1. It's in %2.").arg(target, made));
                    return;
                }
            }
            m_saveFolder = QFileInfo(target).absolutePath();
            Q_EMIT status(tr("Saved the film as %1.").arg(QDir::toNativeSeparators(target)));
            return;
        }
        QFile::remove(made);
        if (!cancelled)
            QMessageBox::warning(m_window, m_window ? m_window->windowTitle() : QString(),
                                 tr("ffmpeg couldn't make the film: %1")
                                     .arg(QString::fromLocal8Bit(process->readAllStandardError()).trimmed().section(QLatin1Char('\n'), -2)));
    });
    process->start(command.first(), command.mid(1));
}

void ViewerHost::sendProfiles(const QString &note)
{
    viewer::Message m;
    m.type = viewer::Message::Profiles;
    for (const enhance::Profile &p : enhance::savedProfiles(profilesFolder()))
        m.profiles << enhance::profileToFile(p);
    m.note = note;
    send(viewer::encode(m));
}

void ViewerHost::profileAsked(viewer::ProfileDo what, const QString &name, const QByteArray &file, const QStringList &dropped)
{
    // Kept here, not in the viewer: it can't reach the home folder. What comes from it, or from a
    // file, is checked the same way before it's kept.
    const QString folder = profilesFolder();
    QString why;
    switch (what) {
    case viewer::ProfileDo::Save: {
        enhance::Profile p;
        if (!enhance::profileFromFile(file, &p, &why))
            return sendProfiles(why);
        if (enhance::saveProfile(p, &why, folder).isEmpty())
            return sendProfiles(tr("The profile couldn't be saved: %1").arg(why));
        return sendProfiles(tr("Saved the profile \"%1\".").arg(p.name));
    }
    case viewer::ProfileDo::Remove:
        return sendProfiles(enhance::removeProfile(name, folder) ? tr("Removed the profile \"%1\".").arg(name)
                                                                  : tr("The profile \"%1\" couldn't be removed.").arg(name));
    case viewer::ProfileDo::Import:
    case viewer::ProfileDo::Dropped: {
        QStringList files = dropped;
        if (what == viewer::ProfileDo::Import) {
            if (m_window) {
                m_window->raise();
                m_window->activateWindow();
            }
            files = m_profileFiles.isEmpty()
                        ? QFileDialog::getOpenFileNames(m_window, tr("Add Enhance Profiles"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
                                                        tr("Enhance profiles (*.%1 *.json)").arg(QLatin1String(enhance::kProfileSuffix)))
                        : m_profileFiles;
            if (files.isEmpty())
                return;
        }
        QStringList added, refused;
        for (const QString &path : std::as_const(files).mid(0, 50)) {
            QFile f(path);
            enhance::Profile p;
            if (!f.open(QIODevice::ReadOnly) || !enhance::profileFromFile(f.read(64 * 1024 + 1), &p, &why)) {
                refused << QStringLiteral("%1 (%2)").arg(QFileInfo(path).fileName(), f.isOpen() ? why : f.errorString());
                continue;
            }
            if (enhance::saveProfile(p, &why, folder).isEmpty())
                refused << QStringLiteral("%1 (%2)").arg(QFileInfo(path).fileName(), why);
            else
                added << p.name;
        }
        QString note = added.isEmpty() ? QString() : tr("Added %n profile(s): %1.", nullptr, int(added.size())).arg(added.join(QStringLiteral(", ")));
        if (!refused.isEmpty())
            note += (note.isEmpty() ? QString() : QStringLiteral(" ")) + tr("Not added: %1").arg(refused.join(QStringLiteral("; ")));
        return sendProfiles(note);
    }
    case viewer::ProfileDo::Export: {
        enhance::Profile p;
        if (!enhance::profileFromFile(file, &p, &why))
            return sendProfiles(why);
        if (m_window) {
            m_window->raise();
            m_window->activateWindow();
        }
        const QString suggested = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QLatin1Char('/') + cleanName(p.name)
                                + QLatin1Char('.') + QLatin1String(enhance::kProfileSuffix);
        const QString target = m_saveTarget.isEmpty()
                                   ? QFileDialog::getSaveFileName(m_window, tr("Save the Profile as a File"), suggested,
                                                                  tr("Enhance profile (*.%1)").arg(QLatin1String(enhance::kProfileSuffix)))
                                   : m_saveTarget;
        if (target.isEmpty())
            return;
        QSaveFile out(target);
        const QByteArray bytes = enhance::profileToFile(p);
        if (out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size() && out.commit())
            return sendProfiles(tr("Saved \"%1\" as %2: give it to anyone with DiskAnvil, who drops it on the viewer to add it.")
                                    .arg(p.name, QDir::toNativeSeparators(target)));
        return sendProfiles(tr("The profile couldn't be saved: %1").arg(out.errorString()));
    }
    }
}
