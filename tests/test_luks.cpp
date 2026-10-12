// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// LUKS header backups, checked against cryptsetup's own: the same bytes as luksHeaderBackup,
// and a wrecked header put back from either one opens with its passphrase again.

#include "testkit.h"

#include "../src/drives/blockio.h"
#include "../src/drives/luksheader.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <unistd.h>

namespace {

int cryptsetup(const QStringList &args, const QByteArray &input = {}, QString *said = nullptr)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(QStringLiteral("cryptsetup"), args);
    p.write(input);
    p.closeWriteChannel();
    p.waitForFinished(120000);
    if (said)
        *said = QString::fromLocal8Bit(p.readAll());
    return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
}

QByteArray fileBytes(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

void luksTests()
{
    if (QStandardPaths::findExecutable(QStringLiteral("cryptsetup")).isEmpty()) {
        out << "SKIP  LUKS: needs cryptsetup" << Qt::endl;
        return;
    }
    QTemporaryDir dir;
    const QByteArray pass = "correct horse";
    for (const QString &type : {QStringLiteral("luks2"), QStringLiteral("luks1")}) {
        const QString image = dir.filePath(type + QStringLiteral(".img"));
        QFile f(image);
        if (f.open(QIODevice::WriteOnly)) {
            f.resize(40 * 1024 * 1024);
            f.close();
        }
        QString said;
        const int made = cryptsetup({QStringLiteral("luksFormat"), QStringLiteral("-q"), QStringLiteral("--type"), type, QStringLiteral("--pbkdf"), QStringLiteral("pbkdf2"),
                                     QStringLiteral("--pbkdf-force-iterations"), QStringLiteral("1000"), image, QStringLiteral("-")},
                                    pass, &said);
        const QString theirs = dir.filePath(type + QStringLiteral("-theirs.img"));
        cryptsetup({QStringLiteral("luksHeaderBackup"), image, QStringLiteral("--header-backup-file"), theirs});

        // Ours, read the way the app reads a drive.
        int fd = blockio::openFile(image, blockio::FileMode::Update);
        luksheader::Info info;
        const QByteArray ours = fd >= 0 ? luksheader::read(fd, &info) : QByteArray();
        report(made == 0 && info.valid() && !ours.isEmpty() && ours == fileBytes(theirs),
               QStringLiteral("LUKS: %1's header backup is the same bytes as cryptsetup's luksHeaderBackup").arg(type),
               QStringLiteral("%1 bytes vs %2, %3 %4").arg(ours.size()).arg(fileBytes(theirs).size()).arg(info.problem, said.left(200)));
        report(info.version == (type == QLatin1String("luks2") ? 2 : 1) && info.uuid.size() == 36 && info.dataOffset > 0,
               QStringLiteral("LUKS: %1's version, ID and where its data starts are read").arg(type), info.uuid);

        // Wrecked: the first MiB zeroed. Put back from ours, it opens again.
        if (fd >= 0) {
            QByteArray zeros(1024 * 1024, '\0');
            blockio::writeAt(fd, zeros.constData(), quint64(zeros.size()), 0);
            blockio::flush(fd);
        }
        const int broken = cryptsetup({QStringLiteral("luksDump"), image});
        QString error;
        const bool put = fd >= 0 && luksheader::write(fd, ours, &error);
        const int opens = cryptsetup({QStringLiteral("open"), QStringLiteral("--test-passphrase"), image, QStringLiteral("--key-file"), QStringLiteral("-")}, pass);
        report(broken != 0 && put && opens == 0, QStringLiteral("LUKS: a wrecked %1 header put back from the copy opens with its passphrase").arg(type), error);

        // And cryptsetup's own copy works the same way.
        if (fd >= 0) {
            QByteArray zeros(1024 * 1024, '\0');
            blockio::writeAt(fd, zeros.constData(), quint64(zeros.size()), 0);
        }
        const bool fromTheirs = fd >= 0 && luksheader::write(fd, fileBytes(theirs), &error);
        report(fromTheirs && cryptsetup({QStringLiteral("open"), QStringLiteral("--test-passphrase"), image, QStringLiteral("--key-file"), QStringLiteral("-")}, pass) == 0,
               QStringLiteral("LUKS: cryptsetup's own %1 backup is put back too").arg(type), error);
        if (fd >= 0)
            ::close(fd);
    }
    // What isn't a whole LUKS header is turned down.
    const QString plain = dir.filePath(QStringLiteral("plain.img"));
    QFile p(plain);
    if (p.open(QIODevice::WriteOnly)) {
        p.write(QByteArray(1024 * 1024, 'x'));
        p.close();
    }
    const int fd = blockio::openFile(plain, blockio::FileMode::Update);
    luksheader::Info info;
    QString error;
    const QByteArray none = luksheader::read(fd, &info);
    const QByteArray half = fileBytes(dir.filePath(QStringLiteral("luks2-theirs.img"))).left(1024 * 1024);
    report(none.isEmpty() && !info.problem.isEmpty() && !luksheader::write(fd, half, &error) && fileBytes(plain) == QByteArray(1024 * 1024, 'x'),
           QStringLiteral("LUKS: no header found says so, and a copy cut short is never written"), info.problem + QStringLiteral(" | ") + error);
    ::close(fd);
}
