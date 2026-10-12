// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "rescuestick.h"

#include "applog.h"
#include "blockio.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace {

constexpr quint64 kMaxSmallFile = 4 * 1024 * 1024; // sha256sum.txt and the info file

// Sticks and images made before the renames (DiskForge Rescue, Bluespark, DiskForge Live) have
// the old names. All of them work.
const QStringList kInfoPaths = {QStringLiteral(".disk/diskanvil-live"), QStringLiteral(".disk/diskforge-live"), QStringLiteral(".disk/bluespark"),
                                QStringLiteral(".disk/diskforge-rescue")};
const QString kSumsPath = QStringLiteral("sha256sum.txt");

QByteArray readEntry(int fd, const isofs::Entry &entry)
{
    if (entry.size > kMaxSmallFile)
        return {};
    QByteArray data(qsizetype(entry.size), '\0');
    return blockio::readAt(fd, data.data(), entry.size, entry.offset) ? data : QByteArray();
}

} // namespace

QString rescue::rescueConfPath = QStringLiteral("/etc/diskanvil-live.conf");

bool rescue::runningInRescue()
{
    return QFileInfo::exists(rescueConfPath);
}

QString rescue::bootOptionsPath = QStringLiteral("/proc/cmdline");
QString rescue::rescanHelperPath = QStringLiteral("/usr/local/bin/diskanvil-live-rescan");

bool rescue::canRescanDrives()
{
    return runningInRescue() && QFileInfo(rescanHelperPath).isExecutable();
}

void rescue::rescanDrives(const QStringList &args, QObject *context, const std::function<void(bool)> &done)
{
    auto *process = new QProcess(context);
    auto finish = [process, done](bool ok) {
        process->deleteLater();
        if (done)
            done(ok);
    };
    QObject::connect(process, &QProcess::finished, context, [finish](int code, QProcess::ExitStatus status) {
        finish(status == QProcess::NormalExit && code == 0);
    });
    QObject::connect(process, &QProcess::errorOccurred, context, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(false);
    });
    qCInfo(lcOps).noquote() << "diskanvil-live-rescan" << args.join(QLatin1Char(' '));
    process->start(QStringLiteral("sudo"), QStringList{QStringLiteral("-n"), rescanHelperPath} + args);
}

bool rescue::runningFromStick()
{
    if (!runningInRescue())
        return false;
    QFile cmdline(bootOptionsPath);
    return !cmdline.open(QIODevice::ReadOnly) || !cmdline.readAll().split(' ').contains("toram");
}

QString rescue::notInRescueReason()
{
    return QObject::tr("DiskAnvil Live starts fresh from the stick every time, so this would only change the copy in memory.");
}

QString rescue::runningBuildId()
{
    QFile conf(rescueConfPath);
    if (!conf.open(QIODevice::ReadOnly))
        return {};
    for (const QByteArray &line : conf.read(4096).split('\n')) {
        if (line.startsWith("BUILD_ID="))
            return QString::fromUtf8(line.mid(9).trimmed());
    }
    return {};
}

rescue::Info rescue::parseInfo(const QByteArray &text)
{
    Info info;
    const QList<QByteArray> lines = text.split('\n');
    const QByteArray header = lines.value(0).trimmed();
    if (header != "DiskAnvil Live" && header != "DiskForge Live" && header != "Bluespark" && header != "DiskForge Rescue")
        return info;
    for (const QByteArray &line : lines) {
        const qsizetype eq = line.indexOf('=');
        if (eq <= 0)
            continue;
        const QByteArray key = line.left(eq).trimmed();
        // Shown in the dialog, so only plain characters, and not too many.
        QString value = QString::fromUtf8(line.mid(eq + 1).trimmed()).left(64);
        value.removeIf([](QChar c) { return !(c.isLetterOrNumber() || c == QLatin1Char('.') || c == QLatin1Char('-')); });
        if (key == "version")
            info.version = value;
        else if (key == "built")
            info.built = value;
        else if (key == "id")
            info.id = value;
    }
    return info;
}

QHash<QString, QByteArray> rescue::parseSums(const QByteArray &text)
{
    QHash<QString, QByteArray> sums;
    for (const QByteArray &line : text.split('\n')) {
        // "<64 hex digits>  ./path" (two spaces, or a space and a '*' for binary mode)
        if (line.size() < 67 || line.at(64) != ' ' || (line.at(65) != ' ' && line.at(65) != '*'))
            continue;
        const QByteArray hash = line.left(64).toLower();
        if (QByteArray::fromHex(hash).toHex() != hash) // fromHex skips what isn't hex
            continue;
        QString path = QString::fromUtf8(line.mid(66).trimmed());
        if (path.startsWith(QLatin1String("./")))
            path.remove(0, 2);
        if (path.isEmpty() || path.startsWith(QLatin1Char('/')) || path.split(QLatin1Char('/')).contains(QLatin1String("..")))
            continue;
        sums.insert(path, hash);
    }
    return sums;
}

rescue::Image rescue::inspect(const QString &isoPath)
{
    Image image;
    // A running rescue stick's own folder (copying the stick DiskAnvil runs from).
    if (QFileInfo(isoPath).isDir()) {
        image.info = stickInfo(isoPath);
        QFile sums(isoPath + QLatin1Char('/') + kSumsPath);
        if (!image.info.valid() || !sums.open(QIODevice::ReadOnly)) {
            image.error = QObject::tr("This folder doesn't hold DiskAnvil Live.");
            return image;
        }
        const QHash<QString, QByteArray> listed = parseSums(sums.read(kMaxSmallFile));
        for (auto it = listed.cbegin(); it != listed.cend(); ++it)
            image.bytes += quint64(QFileInfo(isoPath + QLatin1Char('/') + it.key()).size());
        return image;
    }
    const int fd = blockio::openFile(isoPath, blockio::FileMode::Read);
    if (fd < 0) {
        image.error = QObject::tr("Couldn't open %1.").arg(QFileInfo(isoPath).fileName());
        return image;
    }
    image.listing = isofs::list(fd);
    if (!image.listing.ok()) {
        image.error = image.listing.error;
        ::close(fd);
        return image;
    }
    const isofs::Entry *info = nullptr;
    for (const QString &path : kInfoPaths)
        info = info ? info : isofs::find(image.listing, path);
    const isofs::Entry *sums = isofs::find(image.listing, kSumsPath);
    if (info && !info->isDir)
        image.info = parseInfo(readEntry(fd, *info));
    ::close(fd);
    if (!image.info.valid() || !sums) {
        image.error = QObject::tr("This isn't a DiskAnvil Live image. For other ISOs, use Write Image to USB.");
        return image;
    }
    for (const isofs::Entry &e : image.listing.entries) {
        if (!e.isDir)
            image.bytes += e.size;
    }
    return image;
}

rescue::Info rescue::stickInfo(const QString &root)
{
    for (const QString &path : kInfoPaths) {
        QFile file(root + QLatin1Char('/') + path);
        if (file.open(QIODevice::ReadOnly))
            return parseInfo(file.read(4096));
    }
    return {};
}

int rescue::logFolders(const QString &root)
{
    return int(QDir(root + QStringLiteral("/logs")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size());
}

rescue::StickWriter::StickWriter(const QString &source, const QString &stickRoot)
    : m_source(source)
    , m_root(stickRoot)
{
}

void rescue::StickWriter::cancel()
{
    m_cancel = true;
    if (filecopy::Copier *copier = m_copier)
        copier->cancel();
}

void rescue::StickWriter::finish(bool ok, const QString &message)
{
    qCInfo(lcOps).noquote() << "Make a DiskAnvil Live USB" << (ok ? "finished:" : "failed:") << message;
    emit finished(ok, message);
}

void rescue::StickWriter::run()
{
    const std::unique_ptr<filecopy::Source> source =
        QFileInfo(m_source).isDir() ? filecopy::openFolder(m_source) : filecopy::openIso(m_source);
    if (!source->error().isEmpty())
        return finish(false, source->error());
    auto find = [&source](const QString &path) -> const filecopy::Entry * {
        for (const filecopy::Entry &e : source->entries()) {
            if (e.path == path && !e.isDir)
                return &e;
        }
        return nullptr;
    };
    auto readSmall = [&source](const filecopy::Entry *e) {
        QByteArray data;
        if (e && e->size <= kMaxSmallFile) {
            data.resize(qsizetype(e->size));
            if (!source->read(*e, 0, data.data(), data.size()))
                data.clear();
        }
        return data;
    };
    const QHash<QString, QByteArray> sums = parseSums(readSmall(find(kSumsPath)));
    if (sums.isEmpty() || (!find(kInfoPaths[0]) && !find(kInfoPaths[1])))
        return finish(false, tr("This isn't a DiskAnvil Live image."));
    for (auto it = sums.cbegin(); it != sums.cend(); ++it) {
        if (!find(it.key()))
            return finish(false, tr("The image is damaged: %1 is missing. Download it again.").arg(it.key()));
    }
    // The boot code for old BIOS PCs goes on after the copy; images from before it was there
    // just don't have it.
    const QString bootPath = QStringLiteral("boot/grub/i386-pc/stick-boot.img"), corePath = QStringLiteral("boot/grub/i386-pc/stick-core.img");
    m_biosBoot = readSmall(find(bootPath));
    m_biosCore = readSmall(find(corePath));
    auto checked = [&sums](const QString &path, const QByteArray &data) {
        return !data.isEmpty() && QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex() == sums.value(path);
    };
    if (!checked(bootPath, m_biosBoot) || !checked(corePath, m_biosCore)) {
        m_biosBoot.clear();
        m_biosCore.clear();
    }

    // What goes onto the stick: everything sha256sum.txt lists, plus that file itself. The
    // boot catalog xorriso adds is only for CDs, so it's left out.
    filecopy::Options options;
    options.expected = sums;
    options.skip = [&sums](const QString &path) { return path != kSumsPath && !sums.contains(path); };
    filecopy::Copier copier(source.get(), m_root, options);
    connect(&copier, &filecopy::Copier::progress, this, &StickWriter::progress);
    bool ok = false;
    QString message;
    connect(&copier, &filecopy::Copier::finished, this, [&](bool copied, const QString &text) {
        ok = copied;
        message = text;
    });
    qCInfo(lcOps).noquote() << "Make a DiskAnvil Live USB: copying from" << m_source << "to" << m_root;
    m_copier = &copier;
    if (m_cancel)
        copier.cancel();
    copier.run();
    m_copier = nullptr;
    if (!ok)
        return finish(false, message);

    // Where the rescue system leaves its notes.
    QDir(m_root).mkpath(QStringLiteral("logs"));
    QFile note(m_root + QStringLiteral("/logs/README.txt"));
    if (note.open(QIODevice::WriteOnly | QIODevice::Text)) {
        note.write("DiskAnvil Live keeps its notes here: one folder for each time this stick started a PC,\n"
                   "named by the date and the PC's model. summary.txt in each one is the place to start.\n");
        note.close();
    }
    blockio::syncFileSystem(m_root);
    finish(true, message);
}

bool rescue::writeBiosBoot(int fd, const QByteArray &bootImg, const QByteArray &coreImg, QString *error)
{
    auto fail = [error](const QString &text) {
        if (error)
            *error = text;
        return false;
    };
    if (bootImg.size() != 512 || coreImg.isEmpty())
        return fail(QObject::tr("the boot code in the image isn't usable"));
    // GRUB's boot sector reads its core from sector 1, counted in 512 bytes.
    if (blockio::logicalSize(fd) != 512)
        return fail(QObject::tr("the stick's sectors aren't 512 bytes"));
    QByteArray sector(512, '\0');
    if (!blockio::readAt(fd, sector.data(), 512, 0))
        return fail(QObject::tr("the stick couldn't be read"));
    if (uchar(sector[510]) != 0x55 || uchar(sector[511]) != 0xAA)
        return fail(QObject::tr("the stick has no partition table"));
    // Where the first partition starts, from the table itself. On a GPT stick sector 1 is the
    // GPT, so that's left alone.
    quint64 firstPartition = 0;
    for (int i = 0; i < 4; ++i) {
        const uchar *entry = reinterpret_cast<const uchar *>(sector.constData()) + 446 + 16 * i;
        const quint32 start = entry[8] | entry[9] << 8 | entry[10] << 16 | quint32(entry[11]) << 24;
        if (entry[4] == 0xee)
            return fail(QObject::tr("the stick has a GPT partition table"));
        if (entry[4] != 0 && start > 0 && (firstPartition == 0 || start < firstPartition))
            firstPartition = start;
    }
    if (firstPartition == 0)
        return fail(QObject::tr("the stick has no partition"));
    if (512 + quint64(coreImg.size()) > firstPartition * 512)
        return fail(QObject::tr("there's no room for it before the partition"));
    memcpy(sector.data(), bootImg.constData(), 440);
    // The core first: a stick with the new boot sector but no core behind it would hang.
    if (!blockio::writeAt(fd, coreImg.constData(), quint64(coreImg.size()), 512) || !blockio::flush(fd)
        || !blockio::writeAt(fd, sector.constData(), 512, 0) || !blockio::flush(fd))
        return fail(QString::fromLocal8Bit(strerror(errno)));
    return true;
}
