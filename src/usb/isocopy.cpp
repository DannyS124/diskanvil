// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "isocopy.h"

#include "applog.h"
#include "blockio.h"
#include "filecopy.h"
#include "jobui.h"
#include "udisks.h"
#include "usbprep.h"

#include <QFile>
#include <QThread>


namespace {

constexpr quint64 kMiB = 1024 * 1024;

} // namespace

IsoCopy::IsoCopy(UDisks *udisks, const QString &diskPath, const QString &isoPath, const isomode::Analysis &analysis,
                 quint64 persistenceBytes, const checksums::Expected &expected, QObject *parent)
    : QObject(parent)
    , m_udisks(udisks)
    , m_disk(diskPath)
    , m_iso(isoPath)
    , m_analysis(analysis)
    , m_persistence(analysis.persistence == isomode::Persistence::None ? 0 : persistenceBytes)
    , m_expected(expected)
{
}

IsoCopy::~IsoCopy()
{
    cancel();
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
}

void IsoCopy::cancel()
{
    if (auto *hasher = qobject_cast<checksums::Hasher *>(m_worker))
        hasher->cancel();
    else if (auto *copier = qobject_cast<filecopy::Copier *>(m_worker))
        copier->cancel();
}

void IsoCopy::start()
{
    qCInfo(lcOps).noquote() << "Copying" << m_iso << "onto" << m_disk << "label" << m_analysis.fatLabel << "persistence" << m_persistence;
    if (m_expected.isSet())
        checkDownload();
    else
        prepare();
}

void IsoCopy::done(bool ok, const QString &message, bool shownAlready)
{
    qCInfo(lcOps).noquote() << "Copying the ISO" << (ok ? "finished:" : "failed:") << message;
    emit finished(ok, message, shownAlready);
}

void IsoCopy::checkDownload()
{
    // Before anything on the stick is touched, so a broken download costs nothing.
    auto *hasher = new checksums::Hasher(m_iso, {m_expected.algorithm});
    m_worker = hasher;
    connect(hasher, &checksums::Hasher::progress, this, [this](quint64 done, quint64 total) {
        emit progress(tr("Checking the download"), done, total);
    });
    connect(hasher, &checksums::Hasher::finished, this, [this](bool ok, const QStringList &hex, const QString &error) {
        m_thread->quit();
        m_thread->wait();
        m_thread = nullptr;
        m_worker = nullptr;
        if (!ok)
            return done(false, error);
        if (hex.value(0).toLatin1() != m_expected.hex)
            return done(false, tr("The ISO's %1 doesn't match the one you gave. The download may be broken. Nothing was written.")
                                   .arg(checksums::name(m_expected.algorithm)));
        prepare();
    });
    m_thread = startOnThread(this, hasher);
}

void IsoCopy::prepare()
{
    const Disk *d = m_udisks->diskByPath(m_disk);
    if (!d)
        return done(false, tr("The stick isn't there anymore."));
    // FAT32 first (bootable: some PCs only list a USB stick then), persistence in the rest.
    QVector<UsbPrep::Partition> parts;
    parts.append({QStringLiteral("vfat"), m_analysis.fatLabel, m_persistence ? d->size - m_persistence - 2 * kMiB : 0, 0x80, true});
    if (m_persistence) {
        parts.append({QStringLiteral("ext4"), isomode::persistenceLabel(m_analysis.persistence), 0, 0,
                      m_analysis.persistence == isomode::Persistence::LiveBoot});
    }
    m_prep = new UsbPrep(m_udisks, m_disk, QStringLiteral("dos"), parts, this);
    connect(m_prep, &UsbPrep::phase, this, &IsoCopy::phase);
    connect(m_prep, &UsbPrep::failed, this, [this](const QString &message, bool shownAlready) { done(false, message, shownAlready); });
    connect(m_prep, &UsbPrep::ready, this, &IsoCopy::copy);
    connect(m_prep, &UsbPrep::done, this, [this] {
        if (!m_failure.isEmpty())
            return done(false, m_failure);
        QString message = tr("The USB stick is ready. It starts UEFI PCs (old BIOS-only ones need it written as it is).");
        if (m_persistence)
            message += QLatin1Char(' ') + tr("Changes you make while it runs are kept on its second partition.");
        message += QLatin1Char(' ') + tr("The free space on it can hold your files too.");
        done(true, message);
    });
    m_prep->start();
}

void IsoCopy::copy(const QStringList &mountPoints)
{
    m_source = filecopy::openIso(m_iso);
    filecopy::Options options;
    const isomode::Analysis a = m_analysis;
    options.transform = [a](const QString &path, const QByteArray &data) {
        if (!isomode::isBootConfig(path))
            return QByteArray();
        const QByteArray patched = isomode::patchConfig(data, a.isoLabel, a.fatLabel, a.persistence);
        return patched == data ? QByteArray() : patched;
    };
    auto *copier = new filecopy::Copier(m_source.get(), mountPoints.value(0), options);
    m_worker = copier;
    connect(copier, &filecopy::Copier::progress, this, &IsoCopy::progress);
    connect(copier, &filecopy::Copier::finished, this, [this, mountPoints](bool ok, const QString &message) {
        m_thread->quit();
        m_thread->wait(); // the copier is done with the source after this
        m_thread = nullptr;
        m_worker = nullptr;
        m_source.reset();
        m_failure = ok ? QString() : message;
        if (ok && m_analysis.persistence == isomode::Persistence::LiveBoot && m_persistence) {
            // Debian live only keeps changes on a partition that says what to keep.
            QFile conf(mountPoints.value(1) + QStringLiteral("/persistence.conf"));
            if (!conf.open(QIODevice::WriteOnly) || conf.write(isomode::persistenceConf()) < 0)
                m_failure = tr("Couldn't write persistence.conf: %1").arg(conf.errorString());
            conf.close();
            blockio::syncFileSystem(mountPoints.value(1));
        }
        // Unmounted either way, so it isn't left mounted after a failed copy.
        m_prep->finish();
    });
    m_thread = startOnThread(this, copier);
}
