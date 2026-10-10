// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fswalk.h"

#include <QElapsedTimer>
#include <QFile>
#include <QSet>

#include <algorithm>
#include <cstring>
#ifdef Q_OS_WIN
#include "win/winutil.h"

#include <QDir>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

constexpr int kMaxDepth = 400; // each level holds one open folder

// A folder's files go in biggest first, the ones past kFilesPerFolder folded into one entry.
void addFiles(UsageNode *node, QVector<UsageNode> fileNodes)
{
    std::sort(fileNodes.begin(), fileNodes.end(), [](const UsageNode &a, const UsageNode &b) { return a.size > b.size; });
    if (fileNodes.size() > UsageScan::kFilesPerFolder) {
        UsageNode rest;
        for (qsizetype i = UsageScan::kFilesPerFolder; i < fileNodes.size(); ++i) {
            rest.size += fileNodes[i].size;
            rest.files += 1;
        }
        rest.name = QObject::tr("%n smaller file(s)", nullptr, int(rest.files));
        fileNodes.resize(UsageScan::kFilesPerFolder);
        fileNodes.append(std::move(rest));
    }
    node->children.append(fileNodes);
    std::sort(node->children.begin(), node->children.end(), [](const UsageNode &a, const UsageNode &b) { return a.size > b.size; });
}

#ifndef Q_OS_WIN

struct Walker {
    dev_t device = 0;
    QSet<QPair<dev_t, ino_t>> seenLinks;
    QSet<QString> mountPoints;
    std::atomic<bool> *cancel = nullptr;
    int unreadable = 0;
    quint64 bytes = 0, files = 0;
    QElapsedTimer sinceReport;
    UsageScan *scan = nullptr;

    void walk(int dirFd, const QString &path, UsageNode *node, int depth)
    {
        DIR *dir = ::fdopendir(::fcntl(dirFd, F_DUPFD_CLOEXEC, 0));
        if (!dir) {
            node->skipped = true;
            ++unreadable;
            return;
        }
        struct Entry {
            QByteArray name;
            struct stat st;
        };
        QVector<Entry> entries;
        while (dirent *e = ::readdir(dir)) {
            if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0)
                continue;
            Entry entry{QByteArray(e->d_name), {}};
            if (::fstatat(dirFd, e->d_name, &entry.st, AT_SYMLINK_NOFOLLOW) == 0)
                entries.append(entry);
        }
        ::closedir(dir);

        QVector<UsageNode> fileNodes;
        for (const Entry &e : entries) {
            if (*cancel)
                return;
            const quint64 used = quint64(e.st.st_blocks) * 512;
            const QString childPath = path + QLatin1Char('/') + QFile::decodeName(e.name);
            if (S_ISDIR(e.st.st_mode)) {
                UsageNode child;
                child.name = QFile::decodeName(e.name);
                child.isDir = true;
                // Another file system (a mount point, a Btrfs subvolume or snapshot) isn't
                // part of this one's usage.
                if (e.st.st_dev != device || mountPoints.contains(childPath)) {
                    child.skipped = true;
                } else if (depth >= kMaxDepth) {
                    child.skipped = true;
                    ++unreadable;
                } else {
                    child.size = used;
                    const int fd = ::openat(dirFd, e.name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                    if (fd < 0) {
                        child.skipped = true;
                        ++unreadable;
                    } else {
                        walk(fd, childPath, &child, depth + 1);
                        ::close(fd);
                    }
                }
                node->size += child.size;
                node->files += child.files;
                node->children.append(std::move(child));
                continue;
            }
            if (e.st.st_nlink > 1 && !S_ISDIR(e.st.st_mode)) {
                const QPair<dev_t, ino_t> key{e.st.st_dev, e.st.st_ino};
                if (seenLinks.contains(key))
                    continue;
                seenLinks.insert(key);
            }
            UsageNode file;
            file.name = QFile::decodeName(e.name);
            file.size = used;
            file.files = 1;
            node->size += used;
            node->files += 1;
            bytes += used;
            files += 1;
            fileNodes.append(std::move(file));
        }

        addFiles(node, std::move(fileNodes));

        if (sinceReport.elapsed() > 200) {
            sinceReport.restart();
            emit scan->progress(bytes, files);
        }
    }
};

QSet<QString> mountPoints()
{
    QSet<QString> points;
    QFile f(QStringLiteral("/proc/self/mountinfo"));
    if (!f.open(QIODevice::ReadOnly))
        return points;
    for (const QByteArray &line : f.readAll().split('\n')) {
        const QList<QByteArray> fields = line.split(' ');
        if (fields.size() > 4)
            points.insert(QString::fromLocal8Bit(fields[4]).replace(QStringLiteral("\\040"), QStringLiteral(" ")));
    }
    return points;
}

#else

// On Windows: what a folder's files take on disk is their size rounded up to whole
// clusters (compressed and sparse files take less, but finding out costs a call per file).
// Junctions, links and volumes mounted in a folder are left out, like other file systems on
// Linux. Hard links are counted each time; telling them apart takes a call per file too.
struct Walker {
    quint64 cluster = 4096;
    std::atomic<bool> *cancel = nullptr;
    int unreadable = 0;
    quint64 bytes = 0, files = 0;
    QElapsedTimer sinceReport;
    UsageScan *scan = nullptr;

    void walk(const QString &path, UsageNode *node, int depth)
    {
        WIN32_FIND_DATAW data {};
        const std::wstring pattern = win::wide(QDir::toNativeSeparators(path) + QStringLiteral("\\*"));
        HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                       FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            node->skipped = true;
            ++unreadable;
            return;
        }
        QVector<UsageNode> fileNodes;
        do {
            if (*cancel)
                break;
            const QString name = win::fromWide(data.cFileName);
            if (name == QLatin1String(".") || name == QLatin1String(".."))
                continue;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                UsageNode child;
                child.name = name;
                child.isDir = true;
                if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    child.skipped = true;
                } else if (depth >= kMaxDepth) {
                    child.skipped = true;
                    ++unreadable;
                } else {
                    walk(path + QLatin1Char('/') + name, &child, depth + 1);
                }
                node->size += child.size;
                node->files += child.files;
                node->children.append(std::move(child));
                continue;
            }
            // Files kept in the cloud (OneDrive's placeholders) take no room here.
            const bool elsewhere = data.dwFileAttributes
                                 & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN);
            const quint64 size = quint64(data.nFileSizeHigh) << 32 | data.nFileSizeLow;
            const quint64 used = elsewhere ? 0 : (size + cluster - 1) / cluster * cluster;
            UsageNode file;
            file.name = name;
            file.size = used;
            file.files = 1;
            node->size += used;
            node->files += 1;
            bytes += used;
            files += 1;
            fileNodes.append(std::move(file));
        } while (FindNextFileW(find, &data));
        FindClose(find);

        addFiles(node, std::move(fileNodes));

        if (sinceReport.elapsed() > 200) {
            sinceReport.restart();
            emit scan->progress(bytes, files);
        }
    }
};

#endif

} // namespace

UsageScan::UsageScan(const QString &root)
    : m_root(root)
{
    qRegisterMetaType<std::shared_ptr<UsageNode>>();
}

#ifdef Q_OS_WIN

void UsageScan::run()
{
    auto root = std::make_shared<UsageNode>();
    root->name = m_root;
    root->isDir = true;
    QString path = QDir::fromNativeSeparators(m_root);
    if (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    Walker w;
    wchar_t volume[MAX_PATH] = {};
    DWORD sectorsPerCluster = 0, bytesPerSector = 0, freeClusters = 0, clusters = 0;
    if (GetVolumePathNameW(win::wide(QDir::toNativeSeparators(m_root)).c_str(), volume, MAX_PATH)
        && GetDiskFreeSpaceW(volume, &sectorsPerCluster, &bytesPerSector, &freeClusters, &clusters))
        w.cluster = std::max<quint64>(quint64(sectorsPerCluster) * bytesPerSector, 512);
    w.cancel = &m_cancel;
    w.scan = this;
    w.sinceReport.start();
    w.walk(path, root.get(), 0);
    if (root->skipped) {
        emit finished(false, root, 1);
        return;
    }
    emit progress(w.bytes, w.files);
    emit finished(!m_cancel, root, w.unreadable);
}

#else

void UsageScan::run()
{
    auto root = std::make_shared<UsageNode>();
    root->name = m_root;
    root->isDir = true;
    const QByteArray path = QFile::encodeName(m_root);
    const int fd = ::open(path.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct stat st {};
    if (fd < 0 || ::fstat(fd, &st) != 0) {
        if (fd >= 0)
            ::close(fd);
        emit finished(false, root, 1);
        return;
    }
    Walker w;
    w.device = st.st_dev;
    w.mountPoints = mountPoints();
    w.mountPoints.remove(m_root);
    w.cancel = &m_cancel;
    w.scan = this;
    w.sinceReport.start();
    root->size = quint64(st.st_blocks) * 512;
    w.walk(fd, m_root == QLatin1String("/") ? QString() : m_root, root.get(), 0);
    ::close(fd);
    emit progress(w.bytes, w.files);
    emit finished(!m_cancel, root, w.unreadable);
}

#endif
