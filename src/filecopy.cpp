// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filecopy.h"

#include "blockio.h"
#include "isofs.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace {

constexpr qsizetype kChunk = 4 * 1024 * 1024;
constexpr quint64 kSmallFile = 1024 * 1024; // what a transform may change

QString tr(const char *text)
{
    return QCoreApplication::translate("filecopy", text);
}

QString lastError()
{
    return QString::fromLocal8Bit(strerror(errno));
}

bool writeAll(int fd, const char *data, qsizetype size)
{
    while (size > 0) {
        const ssize_t n = ::write(fd, data, size_t(size));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        data += n;
        size -= n;
    }
    return true;
}

class IsoSource : public filecopy::Source
{
public:
    explicit IsoSource(const QString &path)
    {
        m_fd = blockio::openFile(path, blockio::FileMode::Read);
        if (m_fd < 0) {
            m_error = tr("Couldn't open %1.").arg(QFileInfo(path).fileName());
            return;
        }
        const isofs::Listing listing = isofs::list(m_fd);
        if (!listing.ok()) {
            m_error = listing.error;
            return;
        }
        m_label = listing.volumeId;
        for (const isofs::Entry &e : listing.entries) {
            m_entries.push_back({e.path, e.isDir, e.size});
            m_offsets.insert(e.path, e.offset);
        }
    }
    ~IsoSource() override
    {
        if (m_fd >= 0)
            ::close(m_fd);
    }
    QString error() const override { return m_error; }
    const QVector<filecopy::Entry> &entries() const override { return m_entries; }
    QString label() const override { return m_label; }
    bool read(const filecopy::Entry &entry, quint64 offset, char *buffer, qsizetype length) override
    {
        const auto found = m_offsets.constFind(entry.path);
        if (found == m_offsets.cend() || offset + quint64(length) > entry.size)
            return false;
        return blockio::readAt(m_fd, buffer, quint64(length), *found + offset);
    }

private:
    int m_fd = -1;
    QString m_error;
    QString m_label;
    QVector<filecopy::Entry> m_entries;
    QHash<QString, quint64> m_offsets;
};

class FolderSource : public filecopy::Source
{
public:
    explicit FolderSource(const QString &root)
        : m_root(QDir(root).absolutePath())
    {
        if (!QFileInfo(m_root).isDir()) {
            m_error = tr("%1 isn't a folder.").arg(root);
            return;
        }
        m_label = QStorageInfo(m_root).name();
        // QDirIterator's order is the folder's own; sort so folders come before what's in them.
        QDirIterator it(m_root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo info = it.fileInfo();
            if (info.isSymLink())
                continue;
            const QString path = QDir(m_root).relativeFilePath(info.filePath());
            m_entries.push_back({path, info.isDir(), info.isDir() ? 0 : quint64(info.size())});
        }
        std::sort(m_entries.begin(), m_entries.end(), [](const filecopy::Entry &a, const filecopy::Entry &b) {
            return a.path < b.path; // "a" sorts before "a/b"
        });
    }
    QString error() const override { return m_error; }
    const QVector<filecopy::Entry> &entries() const override { return m_entries; }
    QString label() const override { return m_label; }
    bool read(const filecopy::Entry &entry, quint64 offset, char *buffer, qsizetype length) override
    {
        if (m_open.fileName() != m_root + QLatin1Char('/') + entry.path) {
            m_open.close();
            m_open.setFileName(m_root + QLatin1Char('/') + entry.path);
            if (!m_open.open(QIODevice::ReadOnly))
                return false;
        }
        return m_open.seek(qint64(offset)) && m_open.read(buffer, length) == length;
    }

private:
    QString m_root;
    QString m_error;
    QString m_label;
    QVector<filecopy::Entry> m_entries;
    QFile m_open;
};

} // namespace

std::unique_ptr<filecopy::Source> filecopy::openIso(const QString &isoPath)
{
    return std::make_unique<IsoSource>(isoPath);
}

std::unique_ptr<filecopy::Source> filecopy::openFolder(const QString &root)
{
    return std::make_unique<FolderSource>(root);
}

QString filecopy::stoppedMessage()
{
    return tr("Stopped. The stick is only half done; make it again before using it.");
}

filecopy::Copier::Copier(Source *source, const QString &target, Options options)
    : m_source(source)
    , m_root(target)
    , m_options(std::move(options))
{
}

void filecopy::Copier::finish(bool ok, const QString &message)
{
    emit finished(ok, message);
}

bool filecopy::Copier::copy(const Entry &entry, quint64 &done, quint64 total)
{
    const QString target = m_root + QLatin1Char('/') + entry.path;
    QCryptographicHash original(QCryptographicHash::Sha256);
    QCryptographicHash written(QCryptographicHash::Sha256);

    // Small files a transform wants to change are read whole, changed, then written.
    QByteArray replaced;
    if (m_options.transform && entry.size < kSmallFile) {
        QByteArray data(qsizetype(entry.size), Qt::Uninitialized);
        if (!m_source->read(entry, 0, data.data(), data.size())) {
            m_error = tr("Couldn't read %1 from the image.").arg(entry.path);
            return false;
        }
        original.addData(data);
        replaced = m_options.transform(entry.path, data);
    }

    const int out = blockio::openFile(target, blockio::FileMode::Replace);
    if (out < 0) {
        m_error = tr("Couldn't write %1 to the stick: %2").arg(entry.path, lastError());
        return false;
    }
    bool ok = true;
    if (!replaced.isNull()) {
        ok = writeAll(out, replaced.constData(), replaced.size());
        written.addData(replaced);
        done += entry.size;
        emit progress(tr("Copying"), done, total);
    } else {
        QByteArray buffer(kChunk, Qt::Uninitialized);
        quint64 copied = 0;
        while (ok && copied < entry.size) {
            if (m_cancel) {
                ok = false;
                break;
            }
            const qsizetype want = qsizetype(qMin<quint64>(quint64(kChunk), entry.size - copied));
            if (!m_source->read(entry, copied, buffer.data(), want)) {
                m_error = tr("Couldn't read %1 from the image.").arg(entry.path);
                ok = false;
                break;
            }
            const QByteArrayView chunk(buffer.constData(), want);
            original.addData(chunk);
            written.addData(chunk);
            if (!writeAll(out, buffer.constData(), want)) {
                m_error = errno == EFBIG ? tr("%1 is too big for a FAT32 stick (4 GB at most).").arg(entry.path)
                                         : tr("Couldn't write %1 to the stick: %2").arg(entry.path, lastError());
                ok = false;
                break;
            }
            copied += quint64(want);
            done += quint64(want);
            emit progress(tr("Copying"), done, total);
        }
    }
    if (!ok && m_error.isEmpty() && !m_cancel)
        m_error = tr("Couldn't write %1 to the stick: %2").arg(entry.path, lastError());
    // On the stick for real and out of the page cache, so the check reads the stick itself.
    if (ok && !blockio::flush(out)) {
        m_error = tr("Couldn't write %1 to the stick: %2").arg(entry.path, lastError());
        ok = false;
    }
    blockio::dropCache(out);
    ::close(out);
    const QByteArray expected = m_options.expected.value(entry.path);
    if (ok && !expected.isEmpty() && original.result().toHex() != expected) {
        m_error = tr("The image is damaged: %1 doesn't match its checksum. Download it again.").arg(entry.path);
        ok = false;
    }
    if (ok) {
        m_written.insert(entry.path, written.result());
        ++m_files;
        m_bytes += entry.size;
    }
    return ok;
}

bool filecopy::Copier::check(const QString &path, quint64 &done, quint64 total)
{
    const int in = blockio::openFile(m_root + QLatin1Char('/') + path, blockio::FileMode::Read);
    if (in < 0) {
        m_error = tr("%1 isn't on the stick after writing it. The stick may be failing; try another one.").arg(path);
        return false;
    }
    blockio::dropCache(in);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(kChunk, Qt::Uninitialized);
    bool ok = true;
    for (;;) {
        if (m_cancel) {
            ok = false;
            break;
        }
        const ssize_t n = ::read(in, buffer.data(), size_t(buffer.size()));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            m_error = tr("Couldn't read %1 back from the stick. It may be failing; try another one.").arg(path);
            ok = false;
            break;
        }
        if (n == 0)
            break;
        hash.addData(QByteArrayView(buffer.constData(), n));
        done += quint64(n);
        emit progress(tr("Checking"), done, total);
    }
    ::close(in);
    if (ok && hash.result() != m_written.value(path)) {
        m_error = tr("The stick gave back something different from what was written to %1. "
                     "It may be failing; try another one.").arg(path);
        ok = false;
    }
    return ok;
}

void filecopy::Copier::run()
{
    if (!m_source || !m_source->error().isEmpty())
        return finish(false, m_source ? m_source->error() : tr("Nothing to copy."));

    QVector<const Entry *> files;
    QVector<const Entry *> folders;
    quint64 total = 0;
    for (const Entry &e : m_source->entries()) {
        if (m_options.skip && m_options.skip(e.path))
            continue;
        if (e.isDir) {
            folders.push_back(&e);
        } else {
            files.push_back(&e);
            total += e.size;
        }
    }

    const QStorageInfo storage(m_root);
    if (storage.isValid() && quint64(storage.bytesAvailable()) < total + m_options.reserve)
        return finish(false, tr("The stick is too small: this needs %1 MB.").arg((total + m_options.reserve) / 1000000));

    QDir root(m_root);
    for (const Entry *e : std::as_const(folders)) {
        if (!root.mkpath(e->path))
            return finish(false, tr("Couldn't make the folder %1 on the stick.").arg(e->path));
    }
    for (const Entry *e : std::as_const(files)) {
        const QString folder = QFileInfo(e->path).path();
        if (folder != QLatin1String(".") && !root.mkpath(folder))
            return finish(false, tr("Couldn't make the folder %1 on the stick.").arg(folder));
    }

    quint64 done = 0;
    for (const Entry *e : std::as_const(files)) {
        if (!copy(*e, done, total))
            return finish(false, m_cancel ? stoppedMessage() : m_error);
    }
    done = 0;
    for (const Entry *e : std::as_const(files)) {
        if (!check(e->path, done, total))
            return finish(false, m_cancel ? stoppedMessage() : m_error);
    }
    blockio::syncFileSystem(m_root);
    finish(true, tr("Copied and checked %1 files.").arg(m_files));
}
