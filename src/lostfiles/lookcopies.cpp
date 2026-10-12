// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lookcopies.h"

#include "blockio.h"
#include "ole.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTemporaryDir>

namespace lost {
namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("lookcopies", text);
}

bool inMemory(const QStorageInfo &where)
{
    const QByteArray fs = where.fileSystemType();
    return fs == "tmpfs" || fs == "ramfs";
}

// Room for it there, leaving some over.
bool fits(const QStorageInfo &where, quint64 size, bool memory)
{
    const qint64 free = where.bytesAvailable();
    // In memory, half of what's free at most: the rest of the computer needs some too.
    return free > 0 && size + (64ull << 20) <= quint64(memory ? free / 2 : free);
}

} // namespace

LookCopies::LookCopies(QString sourceDisk)
    : m_disk(std::move(sourceDisk))
{
}

LookCopies::~LookCopies() = default; // the folders go with everything in them

QString LookCopies::folderFor(quint64 size, QString *why)
{
    const auto usable = [this](const QString &path) { return m_disk.isEmpty() || !blockio::pathIsOnDisk(path, m_disk); };
    // A folder picked for them comes first, once there is one.
    if (m_picked && m_picked->isValid())
        return m_picked->path();
    // Memory: nothing is written to any drive at all.
    const QString temp = QDir::tempPath();
    if (inMemory(QStorageInfo(temp)) && fits(QStorageInfo(temp), size, true)) {
        if (!m_memory)
            m_memory = std::make_unique<QTemporaryDir>(temp + QStringLiteral("/diskforge-look-XXXXXX"));
        if (m_memory->isValid())
            return m_memory->path();
    }
    // The cache folder, and on a computer whose temporary folder is on a drive (Windows), that one.
    for (const QString &place : {QStandardPaths::writableLocation(QStandardPaths::CacheLocation), temp}) {
        QDir().mkpath(place);
        if (!usable(place) || inMemory(QStorageInfo(place)) || !fits(QStorageInfo(place), size, false))
            continue;
        if (!m_cache)
            m_cache = std::make_unique<QTemporaryDir>(place + QStringLiteral("/diskforge-look-XXXXXX"));
        if (m_cache->isValid())
            return m_cache->path();
    }
    if (why)
        *why = tr("There's no room for a copy of it to look at, except on the drive it's being found on, where it could overwrite "
                  "other lost files. Pick a folder on another drive for the copies.");
    return {};
}

QStringList LookCopies::folders()
{
    // Asking for a small one makes the folder in memory (or the cache one where memory won't do),
    // and a big one the other.
    folderFor(0, nullptr);
    if (!m_cache) {
        const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QDir().mkpath(cache);
        if ((m_disk.isEmpty() || !blockio::pathIsOnDisk(cache, m_disk)) && !inMemory(QStorageInfo(cache)))
            m_cache = std::make_unique<QTemporaryDir>(cache + QStringLiteral("/diskforge-look-XXXXXX"));
    }
    QStringList out;
    for (const auto *dir : {m_memory.get(), m_cache.get(), m_picked.get()}) {
        if (dir && dir->isValid())
            out << dir->path();
    }
    return out;
}

bool LookCopies::setFolder(const QString &folder, QString *why)
{
    if (!m_disk.isEmpty() && blockio::pathIsOnDisk(folder, m_disk)) {
        if (why)
            *why = tr("That folder is on the drive the files are being found on. Pick one on another drive.");
        return false;
    }
    auto picked = std::make_unique<QTemporaryDir>(folder + QStringLiteral("/diskforge-look-XXXXXX"));
    if (!picked->isValid()) {
        if (why)
            *why = tr("A folder for the copies couldn't be made there.");
        return false;
    }
    m_picked = std::move(picked);
    return true;
}

QString LookCopies::make(const std::shared_ptr<Source> &source, const Found &f, int index, QString *why,
                         const std::function<bool(quint64, quint64)> &progress)
{
    if (const QString done = m_made.value(index); !done.isEmpty() && QFileInfo::exists(done))
        return done;
    if (f.nameOnly) {
        if (why)
            *why = tr("Only its name is left: there's nothing of it to look at.");
        return {};
    }
    const QString folder = folderFor(f.size, why);
    if (folder.isEmpty())
        return {};
    // Each in a folder of its own, under its own name, so two files called the same don't meet.
    const QString path = QStringLiteral("%1/%2/%3").arg(folder).arg(index).arg(cleanName(f.name.isEmpty() ? QStringLiteral("file") : f.name));
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        if (why)
            *why = tr("A copy couldn't be made: %1").arg(out.errorString());
        return {};
    }
    FileReader reader(source, f);
    QByteArray buffer(1 << 20, Qt::Uninitialized);
    for (quint64 at = 0; at < f.size;) {
        const qint64 n = reader.read(at, buffer.data(), qint64(std::min<quint64>(buffer.size(), f.size - at)));
        if (n <= 0 || out.write(buffer.constData(), n) != n) {
            out.remove();
            if (why)
                *why = tr("A copy couldn't be made: the file couldn't be read, or there was no more room.");
            return {};
        }
        at += quint64(n);
        if (progress && !progress(at, f.size)) {
            out.remove();
            if (why)
                why->clear(); // stopped: nothing to say
            return {};
        }
    }
    out.close();
    // Read-only: it's a copy to look at, and a program opening it shouldn't save over it.
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadUser);
    m_made.insert(index, path);
    return path;
}

bool LookCopies::safeToOpen(const QString &name)
{
    // Kinds whose programs show them rather than run them. Not here: programs and scripts of
    // every kind, web pages and SVG (they can carry scripts), Office files with macros (.docm),
    // shortcuts, and anything without a known kind.
    static const QSet<QString> safe = {
        QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"), QStringLiteral("gif"), QStringLiteral("bmp"),
        QStringLiteral("webp"), QStringLiteral("heic"), QStringLiteral("heif"), QStringLiteral("avif"), QStringLiteral("tif"),
        QStringLiteral("tiff"), QStringLiteral("cr2"), QStringLiteral("cr3"), QStringLiteral("nef"), QStringLiteral("arw"),
        QStringLiteral("dng"), QStringLiteral("orf"), QStringLiteral("rw2"), QStringLiteral("raf"), QStringLiteral("psd"),
        QStringLiteral("pdf"), QStringLiteral("doc"), QStringLiteral("docx"), QStringLiteral("xls"), QStringLiteral("xlsx"),
        QStringLiteral("ppt"), QStringLiteral("pptx"), QStringLiteral("odt"), QStringLiteral("ods"), QStringLiteral("odp"),
        QStringLiteral("rtf"), QStringLiteral("txt"), QStringLiteral("md"), QStringLiteral("csv"), QStringLiteral("epub"),
        QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("m4v"), QStringLiteral("3gp"), QStringLiteral("avi"),
        QStringLiteral("mkv"), QStringLiteral("webm"), QStringLiteral("wmv"), QStringLiteral("mpg"), QStringLiteral("mpeg"),
        QStringLiteral("ogv"), QStringLiteral("mp3"), QStringLiteral("wav"), QStringLiteral("m4a"), QStringLiteral("aac"),
        QStringLiteral("flac"), QStringLiteral("ogg"), QStringLiteral("opus"), QStringLiteral("wma"), QStringLiteral("zip"),
        QStringLiteral("7z"), QStringLiteral("rar"), QStringLiteral("tar"), QStringLiteral("gz"), QStringLiteral("tgz"),
        QStringLiteral("bz2"), QStringLiteral("xz")};
    // The last extension is what decides: "invoice.pdf.exe" is a program.
    return name.contains(QLatin1Char('.')) && safe.contains(name.section(QLatin1Char('.'), -1).toLower());
}

bool LookCopies::hasMacros(const QString &copy)
{
    QFile file(copy);
    if (!file.open(QIODevice::ReadOnly) || !file.peek(8).startsWith("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1"))
        return false;
    if (file.size() > 512ll * 1024 * 1024)
        return true; // too big to look through: treated as if it had them
    return ole::hasMacros(ole::File(file.readAll()));
}

} // namespace lost
