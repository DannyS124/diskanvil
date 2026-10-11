// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "checksums.h"
#include "filecopy.h"
#include "isomode.h"

#include <QObject>

#include <memory>

class QThread;
class UDisks;
class UsbPrep;

// Write Image to USB's "copy the files" way: the stick gets an MBR with a FAT32 partition
// holding the ISO's files (with the boot menus pointed at the stick's label) and, with
// persistence, an ext4 partition after it. A checksum is checked before anything is erased.
class IsoCopy : public QObject
{
    Q_OBJECT
public:
    IsoCopy(UDisks *udisks, const QString &diskPath, const QString &isoPath, const isomode::Analysis &analysis,
            quint64 persistenceBytes, const checksums::Expected &expected, QObject *parent = nullptr);
    ~IsoCopy() override;

    void start();
    // Stops the checksum check or the copy. The UDisks steps can't be stopped halfway.
    void cancel();
    bool canCancel() const { return m_worker != nullptr; }

signals:
    void phase(const QString &text);
    void progress(const QString &phase, quint64 done, quint64 total);
    // `shownAlready`: a UDisks error the main window shows itself.
    void finished(bool ok, const QString &message, bool shownAlready);

private:
    void checkDownload();
    void prepare();
    void copy(const QStringList &mountPoints);
    void done(bool ok, const QString &message, bool shownAlready = false);

    UDisks *m_udisks;
    QString m_disk;
    QString m_iso;
    isomode::Analysis m_analysis;
    quint64 m_persistence;
    checksums::Expected m_expected;
    UsbPrep *m_prep = nullptr;
    std::unique_ptr<filecopy::Source> m_source; // read by the copier on its thread
    QThread *m_thread = nullptr;
    QObject *m_worker = nullptr;
    QString m_failure;
};
