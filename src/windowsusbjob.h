// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "windowsusb.h"

#include <QObject>

#include <atomic>
#include <functional>
#include <memory>

class QProcess;
class QThread;
class UDisks;
class UsbPrep;
namespace filecopy {
class Source;
}

// Makes a Windows install stick from a mounted Windows ISO: an MBR with one FAT32 partition,
// the ISO's files copied and checked, install.wim split onto it with wimlib when it's too big
// for FAT32 (then checked by wimlib, reading it back from the stick), and autounattend.xml for
// the options. The ISO's mounting is up to the caller (WindowsUsbDialog keeps it mounted).
class WindowsUsbJob : public QObject
{
    Q_OBJECT
public:
    WindowsUsbJob(UDisks *udisks, const QString &diskPath, const QString &isoRoot, const QString &label,
                  const windowsusb::Info &info, const windowsusb::Options &options, QObject *parent = nullptr);
    ~WindowsUsbJob() override;

    void start();
    // Stops the copy or the split. Setting the stick up can't be stopped halfway.
    void cancel();
    bool canCancel() const;

    // For the tests: the part size install.wim is split into, in MiB.
    static int splitMiB;

signals:
    void phase(const QString &text);
    void progress(const QString &phase, quint64 done, quint64 total);
    // `shownAlready`: a UDisks error the main window shows itself.
    void finished(bool ok, const QString &message, bool shownAlready);

private:
    void copy(const QString &stick);
    void split();
    void verify();
    void writeAnswers();
    void runWimlib(const QStringList &args, const QString &phaseText, const std::function<void()> &next);
    void fail(const QString &message);

    UDisks *m_udisks;
    QString m_disk;
    QString m_iso;
    QString m_label;
    windowsusb::Info m_info;
    windowsusb::Options m_options;
    QString m_stick;
    QString m_failure;
    UsbPrep *m_prep = nullptr;
    std::unique_ptr<filecopy::Source> m_source;
    QThread *m_thread = nullptr;
    QObject *m_copier = nullptr;
    QProcess *m_wimlib = nullptr; // DISM on Windows
    std::shared_ptr<std::atomic_bool> m_stopCheck; // while the parts are checked (Windows)
    bool m_cancelled = false;
};
