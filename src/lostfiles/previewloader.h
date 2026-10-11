// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lostscan.h"
#include "preview.h"

#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QStringList>
#include <QWaitCondition>

#include <memory>

class QProcess;

// Gets found files' previews from the preview helper (preview.h), newest wish first: what's on
// screen now matters more than what was scrolled past. It runs on a thread of its own, since
// reading a file from a drive and waiting for the helper both take time. A helper that doesn't
// answer within kTimeout is stopped, and the next wish starts a new one.
class PreviewLoader : public QObject
{
    Q_OBJECT
public:
    explicit PreviewLoader(std::shared_ptr<lost::Source> source);
    ~PreviewLoader() override;

    void want(int index, const lost::Found &file, int pixels);
    void stop();

    static constexpr int kTimeout = 15000;
    // How the helper is started: this program with --preview-helper, inside bubblewrap on Linux
    // when `sandbox` and bwrap is there (not in the Flatpak, which is a sandbox already).
    static QStringList helperCommand(bool sandbox);

public Q_SLOTS:
    void run();

Q_SIGNALS:
    // `facts`: what the file says about itself. `failed`: the helper crashed or hung on it.
    void ready(int index, int pixels, const QImage &image, const QString &text, const QStringList &facts, bool failed);
    // A wish let go of unanswered (too many piled up): ask again when it's wanted again.
    void dropped(int index, int pixels);

private:
    struct Wish {
        int index = -1;
        lost::Found file;
        int pixels = 0;
    };
    preview::Result ask(const preview::Request &request, bool *failed);
    bool startHelper();

    std::shared_ptr<lost::Source> m_source;
    QMutex m_lock;
    QWaitCondition m_wake;
    QList<Wish> m_queue;
    bool m_stop = false;
    std::unique_ptr<QProcess> m_helper;
    bool m_sandboxed = false;
    bool m_sandboxWorks = false; // answered once from inside the sandbox
    bool m_noSandbox = false;    // bwrap can't make one here (some systems don't allow it)
};
