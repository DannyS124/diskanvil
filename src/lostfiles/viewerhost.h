// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "enhance.h"
#include "viewer.h"

#include <QObject>
#include <QPointer>
#include <QStringList>

class QProcess;
class QWidget;

// Running the viewer (viewer.h), a program of its own in its sandbox, for a window that shows
// files in it: Find Lost Files, and Look Closer for pictures and films on the PC. It starts the
// viewer for the first file (again when a file is somewhere its sandbox can't read), and does
// what the viewer can't from its sandbox: saves the pictures and films it makes where the person
// says, and keeps the enhance profiles. What's asked about the file itself (the next one, saving
// it, repairing it) goes to the window.
class ViewerHost : public QObject
{
    Q_OBJECT
public:
    explicit ViewerHost(QWidget *window);
    ~ViewerHost() override;

    // Shows it; `readable` are the folders the sandbox may read (where the file is).
    void show(const viewer::Show &show, const QStringList &readable);
    // New facts about the one shown.
    void sendFacts(const QStringList &facts);
    void stop();
    bool running() const { return m_process != nullptr; }
    bool ready() const { return m_ready; }

    // Nothing it saves goes on this drive (the one files are being got back from).
    void setGuardedDisk(const QString &device) { m_guarded = device; }
    // Where saving starts.
    void setSaveFolder(const QString &folder) { m_saveFolder = folder; }

    // The tests: where a picture, film or profile file is saved (no dialog), where profiles are
    // kept, the files "Add Profiles" picks, and what the viewer says, as if it said it.
    void setSaveTarget(const QString &path) { m_saveTarget = path; }
    void setProfilesFolder(const QString &folder) { m_profilesFolder = folder; }
    void setProfileFilesToAdd(const QStringList &files) { m_profileFiles = files; }
    void hear(const QByteArray &said) { heard(said); }
    QString profilesFolder() const;

Q_SIGNALS:
    // What the viewer asks the window for (Ready is handled here, and said too).
    void asked(viewer::Ask ask);
    // Its changed copy of the bytes of the one shown.
    void edited(const QMap<quint64, quint8> &edits);
    // A line for the window's status.
    void status(const QString &text);
    // It closed (not a restart).
    void closed();

private:
    void start();
    void send(const QByteArray &message);
    void heard(const QByteArray &said);
    void savePicture(const QString &name, const QByteArray &png);
    void exportFilm(double speed, const enhance::Settings &settings, qint64 durationMs);
    void sendProfiles(const QString &note);
    void profileAsked(viewer::ProfileDo what, const QString &name, const QByteArray &file, const QStringList &dropped);
    // A save somewhere it mustn't go: said, and false.
    bool allowed(const QString &target);

    QPointer<QWidget> m_window;
    QProcess *m_process = nullptr;
    QByteArray m_heard;            // what it said that isn't read yet
    QList<QByteArray> m_waiting;   // for it, until it's ready
    QStringList m_readable;        // the folders its sandbox may read
    bool m_ready = false;
    bool m_sandboxed = true;       // false once the sandbox turned out not to work here
    viewer::Show m_shown;          // the last one sent
    QString m_guarded, m_saveFolder, m_saveTarget, m_profilesFolder;
    QStringList m_profileFiles;
};
