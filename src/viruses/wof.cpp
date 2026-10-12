// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wof.h"

#include "applog.h"
#include "lostfs.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>

#include <atomic>

#ifndef Q_OS_WIN
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

namespace wof {

namespace {

std::atomic<bool> interactive{true};

QString tr(const char *text)
{
    return QCoreApplication::translate("wof", text);
}

#ifndef Q_OS_WIN
// mountinfo writes spaces and the like as \ and three octal digits.
QString unescape(const QByteArray &field)
{
    QByteArray out;
    for (qsizetype i = 0; i < field.size(); ++i) {
        if (field[i] == '\\' && i + 3 < field.size()) {
            out += char(field.mid(i + 1, 3).toInt(nullptr, 8));
            i += 3;
        } else {
            out += field[i];
        }
    }
    return QString::fromLocal8Bit(out);
}

// The device and file system type of what's mounted as `dev` (a file's st_dev).
bool mountOf(dev_t dev, QString *device, QString *type)
{
    QFile file(QStringLiteral("/proc/self/mountinfo"));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QList<QByteArray> lines = file.readAll().split('\n');
    for (const QByteArray &line : lines) {
        const QList<QByteArray> fields = line.split(' ');
        const qsizetype dash = fields.indexOf("-");
        if (fields.size() < 7 || dash < 6 || dash + 2 >= fields.size())
            continue;
        const QList<QByteArray> numbers = fields[2].split(':');
        if (numbers.size() != 2 || makedev(numbers[0].toUInt(), numbers[1].toUInt()) != dev)
            continue;
        *type = QString::fromUtf8(fields[dash + 1]);
        *device = unescape(fields[dash + 2]);
        return true;
    }
    return false;
}

// The device read-only, through UDisks (which asks polkit, and maybe for the password).
int openThroughUdisks(const QString &device, bool interactive, QString *why)
{
    const QString service = QStringLiteral("org.freedesktop.UDisks2");
    QDBusMessage resolve = QDBusMessage::createMethodCall(service, QStringLiteral("/org/freedesktop/UDisks2/Manager"),
                                                          QStringLiteral("org.freedesktop.UDisks2.Manager"), QStringLiteral("ResolveDevice"));
    resolve << QVariantMap{{QStringLiteral("path"), device}} << QVariantMap();
    const QDBusMessage found = QDBusConnection::systemBus().call(resolve, QDBus::Block, 30000);
    QString object;
    if (found.type() == QDBusMessage::ReplyMessage && !found.arguments().isEmpty() && found.arguments().constFirst().canConvert<QDBusArgument>()) {
        const QDBusArgument paths = found.arguments().constFirst().value<QDBusArgument>();
        paths.beginArray();
        if (!paths.atEnd()) {
            QDBusObjectPath path;
            paths >> path;
            object = path.path();
        }
        paths.endArray();
    }
    if (object.isEmpty()) {
        *why = tr("UDisks doesn't know %1.").arg(device);
        return -1;
    }
    QDBusMessage open = QDBusMessage::createMethodCall(service, object, QStringLiteral("org.freedesktop.UDisks2.Block"), QStringLiteral("OpenForBenchmark"));
    QVariantMap options{{QStringLiteral("writable"), false}};
    if (!interactive)
        options.insert(QStringLiteral("auth.no_user_interaction"), true);
    open << options;
    open.setInteractiveAuthorizationAllowed(interactive);
    const QDBusMessage reply = QDBusConnection::systemBus().call(open, QDBus::Block, 5 * 60 * 1000); // time to type the password
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        *why = reply.errorMessage().isEmpty() ? tr("UDisks didn't open it.") : reply.errorMessage();
        return -1;
    }
    const QDBusUnixFileDescriptor fd = qdbus_cast<QDBusUnixFileDescriptor>(reply.arguments().constFirst());
    if (!fd.isValid()) {
        *why = tr("UDisks didn't open it.");
        return -1;
    }
    return ::fcntl(fd.fileDescriptor(), F_DUPFD_CLOEXEC, 0); // fd closes its own when it goes
}
#endif

} // namespace

bool compressed(const QString &path)
{
#ifdef Q_OS_WIN
    Q_UNUSED(path)
    return false;
#else
    const QByteArray name = path.toLocal8Bit();
    // ntfs-3g: a link, with the reparse data (the tag first) in an attribute of its own.
    struct stat st;
    if (::lstat(name.constData(), &st) == 0 && S_ISLNK(st.st_mode)) {
        QByteArray data(16 * 1024 + 8, '\0'); // the most a reparse point holds
        const ssize_t n = ::lgetxattr(name.constData(), "system.ntfs_reparse_data", data.data(), size_t(data.size()));
        return n >= 4 && qFromLittleEndian<quint32>(data.constData()) == 0x80000017; // IO_REPARSE_TAG_WOF
    }
    // ntfs3 without its LZX/XPRESS support: reading isn't supported.
    const int fd = ::open(name.constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    char c;
    const ssize_t n = ::read(fd, &c, 1);
    const int error = errno;
    ::close(fd);
    return n < 0 && (error == EOPNOTSUPP || error == ENOTSUP);
#endif
}

QDateTime changed(const QString &path)
{
#ifdef Q_OS_WIN
    return QFileInfo(path).lastModified();
#else
    struct stat st;
    if (::lstat(path.toLocal8Bit().constData(), &st) != 0)
        return {};
    return QDateTime::fromSecsSinceEpoch(st.st_mtime);
#endif
}

bool inWindowsFolder(const QString &root, const QString &file)
{
    return QDir(root).relativeFilePath(file).startsWith(QLatin1String("windows/"), Qt::CaseInsensitive);
}

void setInteractive(bool yes)
{
    interactive = yes;
}

Reader::Reader() = default;
Reader::~Reader() = default;

std::shared_ptr<lost::Source> Reader::open(const QString &path, QString *why)
{
#ifdef Q_OS_WIN
    Q_UNUSED(path)
    *why = tr("Windows reads its compressed files itself.");
    return {};
#else
    struct stat st;
    if (::lstat(path.toLocal8Bit().constData(), &st) != 0) {
        *why = QString::fromLocal8Bit(strerror(errno));
        return {};
    }
    QString device, type;
    if (!mountOf(st.st_dev, &device, &type) || !(type.startsWith(QLatin1String("ntfs")) || type == QLatin1String("fuseblk"))
        || !device.startsWith(QLatin1String("/dev/"))) {
        *why = tr("It isn't on an NTFS drive.");
        return {};
    }
    std::shared_ptr<lost::Source> drive = m_drives.value(device);
    if (!drive && m_failed.contains(device)) {
        *why = m_failed.value(device);
        return {};
    }
    if (!drive) {
        // Itself when it may (root, or the disk group), otherwise through UDisks: read-only either way.
        int fd = ::open(device.toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC);
        bool direct = false;
        QString problem;
        if (fd < 0 && (errno == EACCES || errno == EPERM)) {
            fd = openThroughUdisks(device, interactive, &problem);
            direct = true; // UDisks opens it with O_DIRECT
        } else if (fd < 0) {
            problem = QString::fromLocal8Bit(strerror(errno));
        }
        if (fd < 0) {
            qCInfo(lcOps).noquote() << "Compressed files: couldn't open" << device << "-" << problem;
            m_failed.insert(device, tr("DiskAnvil couldn't open %1 to read it itself: %2").arg(device, problem));
            *why = m_failed.value(device);
            return {};
        }
        qCInfo(lcOps).noquote() << "Compressed files: reading them from" << device << "itself";
        drive = lost::Source::fromFd(fd, direct);
        m_drives.insert(device, drive);
    }
    return lost::ntfsFile(drive, quint64(st.st_ino), QFileInfo(path).fileName(), why);
#endif
}

qint64 Reader::size(const QString &path, QString *why)
{
    const std::shared_ptr<lost::Source> file = open(path, why);
    return file ? qint64(file->size()) : -1;
}

bool Reader::read(const QString &path, QByteArray *out, QString *why, quint64 most)
{
    const std::shared_ptr<lost::Source> file = open(path, why);
    if (!file)
        return false;
    if (file->size() > most) {
        *why = tr("It's too big to read whole.");
        return false;
    }
    QByteArray data(qsizetype(file->size()), Qt::Uninitialized);
    file->read(0, data.data(), file->size());
    if (file->unreadable(0, file->size())) {
        *why = tr("Part of it couldn't be read or unpacked.");
        return false;
    }
    *out = std::move(data);
    return true;
}

bool Reader::copy(const QString &path, const QString &to, QString *why)
{
    const std::shared_ptr<lost::Source> file = open(path, why);
    if (!file)
        return false;
    QFile out(to);
    if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        *why = out.errorString();
        return false;
    }
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    for (quint64 at = 0; at < file->size();) {
        const quint64 n = std::min<quint64>(quint64(buffer.size()), file->size() - at);
        file->read(at, buffer.data(), n);
        if (file->unreadable(at, n)) {
            *why = tr("Part of it couldn't be read or unpacked.");
            out.remove();
            return false;
        }
        if (out.write(buffer.constData(), qint64(n)) != qint64(n)) {
            *why = out.errorString();
            out.remove();
            return false;
        }
        at += n;
    }
    out.setFileTime(changed(path), QFileDevice::FileModificationTime);
    return true;
}

} // namespace wof
