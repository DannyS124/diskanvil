// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fswalk.h"

#include <QElapsedTimer>
#include <QFile>
#include <QSet>

#include <algorithm>
#include <cstring>
#include <vector>
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

// On Windows each folder is listed in big batches that carry every file's ID and the space it
// takes on disk (GetFileInformationByHandleEx, FileIdBothDirectoryInfo), so hard links (Windows
// has many, in WinSxS) count once, as on Linux. Compressed and sparse files, and files Windows
// keeps compressed itself (Compact OS), are asked about one by one; there are few. Junctions,
// links and volumes mounted in a folder are left out, like other file systems on Linux.
struct Walker {
    bool fileIds = false; // NTFS and ReFS: a file has the same ID under each of its names
    QSet<qint64> seenLinks;
    quint64 cluster = 4096;
    std::atomic<bool> *cancel = nullptr;
    int unreadable = 0;
    quint64 bytes = 0, files = 0;
    QElapsedTimer sinceReport;
    UsageScan *scan = nullptr;

    struct Entry {
        QString name;
        DWORD attributes = 0;
        quint64 size = 0;
        quint64 allocated = 0;
        qint64 id = 0;
    };

    // The folder's entries, or false when it can't be opened.
    bool list(const QString &path, QVector<Entry> &entries)
    {
        // A drive's root with its backslash: "C:" alone is the folder Windows is in on C:.
        QString native = QDir::toNativeSeparators(path);
        if (native.size() == 2 && native.at(1) == QLatin1Char(':'))
            native += QLatin1Char('\\');
        const win::Handle dir(CreateFileW(win::wide(native).c_str(), FILE_LIST_DIRECTORY | SYNCHRONIZE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!dir.valid())
            return false;
        std::vector<LONGLONG> buffer(64 * 1024 / sizeof(LONGLONG)); // the entries are 8-byte aligned
        for (bool first = true;; first = false) {
            if (!GetFileInformationByHandleEx(dir.get(), first ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo,
                                              buffer.data(), DWORD(buffer.size() * sizeof(LONGLONG))))
                return !first || GetLastError() == ERROR_NO_MORE_FILES;
            const char *at = reinterpret_cast<const char *>(buffer.data());
            for (;;) {
                const auto *e = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO *>(at);
                Entry entry;
                entry.name = QString::fromWCharArray(e->FileName, int(e->FileNameLength / sizeof(wchar_t)));
                if (entry.name != QLatin1String(".") && entry.name != QLatin1String("..")) {
                    entry.attributes = e->FileAttributes;
                    entry.size = quint64(e->EndOfFile.QuadPart);
                    entry.allocated = quint64(e->AllocationSize.QuadPart);
                    entry.id = e->FileId.QuadPart;
                    entries.append(std::move(entry));
                }
                if (e->NextEntryOffset == 0)
                    break;
                at += e->NextEntryOffset;
            }
        }
    }

    void walk(const QString &path, UsageNode *node, int depth)
    {
        QVector<Entry> entries;
        if (!list(path, entries)) {
            node->skipped = true;
            ++unreadable;
            return;
        }
        QVector<UsageNode> fileNodes;
        for (const Entry &e : entries) {
            if (*cancel)
                return;
            if (e.attributes & FILE_ATTRIBUTE_DIRECTORY) {
                UsageNode child;
                child.name = e.name;
                child.isDir = true;
                if (e.attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    child.skipped = true;
                } else if (depth >= kMaxDepth) {
                    child.skipped = true;
                    ++unreadable;
                } else {
                    walk(path + QLatin1Char('/') + e.name, &child, depth + 1);
                }
                node->size += child.size;
                node->files += child.files;
                node->children.append(std::move(child));
                continue;
            }
            if (fileIds && e.id != 0) {
                if (seenLinks.contains(e.id))
                    continue;
                seenLinks.insert(e.id);
            }
            // Files kept in the cloud (OneDrive's placeholders) take no room here. Ones that take
            // less than their length are compressed (by NTFS, or by Windows itself, as with Compact
            // OS, which the listing doesn't show) or sparse: asked one by one. The smallest live
            // inside the file table itself.
            quint64 used = e.allocated;
            if (e.attributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN)) {
                used = 0;
            } else if ((e.attributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_REPARSE_POINT))
                       || (e.size > 1024 && e.allocated < e.size)) {
                DWORD high = 0;
                SetLastError(NO_ERROR);
                const DWORD low = GetCompressedFileSizeW(win::wide(QDir::toNativeSeparators(path + QLatin1Char('/') + e.name)).c_str(), &high);
                const quint64 bytes = low != INVALID_FILE_SIZE || GetLastError() == NO_ERROR ? quint64(high) << 32 | low : e.size;
                used = (bytes + cluster - 1) / cluster * cluster;
            }
            UsageNode file;
            file.name = e.name;
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

// An administrator's backup rights (there, but switched off) open every folder for reading:
// the system's own ones, like System Volume Information with the restore points, and other
// users'. Only listing them; nothing is opened to read or change.
void enableBackupRights()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return;
    TOKEN_PRIVILEGES privileges = {};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(nullptr, SE_BACKUP_NAME, &privileges.Privileges[0].Luid))
        AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
    CloseHandle(token);
}

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
    enableBackupRights();
    wchar_t volume[MAX_PATH] = {};
    DWORD sectorsPerCluster = 0, bytesPerSector = 0, freeClusters = 0, clusters = 0;
    if (GetVolumePathNameW(win::wide(QDir::toNativeSeparators(m_root)).c_str(), volume, MAX_PATH)) {
        if (GetDiskFreeSpaceW(volume, &sectorsPerCluster, &bytesPerSector, &freeClusters, &clusters))
            w.cluster = std::max<quint64>(quint64(sectorsPerCluster) * bytesPerSector, 512);
        wchar_t fsName[MAX_PATH] = {};
        if (GetVolumeInformationW(volume, nullptr, 0, nullptr, nullptr, nullptr, fsName, MAX_PATH)) {
            const QString fs = QString::fromWCharArray(fsName);
            w.fileIds = fs == QLatin1String("NTFS") || fs == QLatin1String("ReFS");
        }
    }
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
