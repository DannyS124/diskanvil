// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// LUKS header backups, checked against cryptsetup's own: the same bytes as luksHeaderBackup,
// and a wrecked header put back from either one opens with its passphrase again.

#include "testkit.h"

#include "../src/drives/blockio.h"
#include "../src/drives/luksheader.h"
#include "../src/drives/opal.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>

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

namespace {

// A pretend self-encrypting drive: Level 0 Discovery like a real Opal 2 drive's, and a session
// manager and Admin SP that answer StartSession with the right PSID and then Revert. Its first
// answer to each command is "not ready yet", the way drives make you ask again.
struct FakeDrive {
    QByteArray psid = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    bool reverted = false;
    bool sessionOpen = false;
    int sessions = 0;
    QByteArray queued;
    bool pendingOnce = false;
    QStringList seen;

    static QByteArray reply(const QByteArray &payload)
    {
        return opal::packet(0x1004, 0, 0, payload);
    }
    opal::Transport transport()
    {
        opal::Transport t;
        t.discover = [](QByteArray *level0) {
            QByteArray d(48, '\0');
            auto feature = [&d](quint16 code, const QByteArray &data) {
                QByteArray f(4, '\0');
                qToBigEndian<quint16>(code, f.data());
                f[2] = 0x10;
                f[3] = char(data.size());
                d += f + data;
            };
            feature(0x0001, QByteArray::fromHex("110000000000000000000000"));
            feature(0x0002, QByteArray::fromHex("190000000000000000000000")); // supported, media encryption, MBR enabled
            feature(0x0203, QByteArray::fromHex("100400010000000000000000000000000000"));
            feature(0x0402, QByteArray::fromHex("020000000000000000000000"));
            qToBigEndian<quint32>(quint32(d.size() - 4), d.data());
            d.append(2048 - d.size(), '\0');
            *level0 = d;
            return true;
        };
        t.send = [this](quint16 comId, const QByteArray &data) {
            seen << QStringLiteral("send %1 %2").arg(comId).arg(data.size());
            if (comId != 0x1004 || data.size() % 512)
                return false;
            const quint32 tsn = qFromBigEndian<quint32>(data.constData() + 20);
            const QByteArray payload = data.mid(56, qFromBigEndian<quint32>(data.constData() + 52));
            const QByteArray end = QByteArray::fromHex("F9F0000000F1");
            if (payload.startsWith(opal::startSessionPayload(0x44415631, psid).left(20))) {
                ++sessions;
                const bool right = payload == opal::startSessionPayload(0x44415631, psid);
                sessionOpen = right;
                // CALL SMUID SyncSession [ HostSessionID SPSessionID ] and the status.
                queued = reply(QByteArray::fromHex("F8A800000000000000FFA8000000000000FF03F0") + QByteArray::fromHex("8444415631") +
                               QByteArray::fromHex("82") + QByteArray::fromHex("1234") + QByteArray::fromHex("F1F9F0") +
                               QByteArray(1, right ? char(0) : char(1)) + QByteArray::fromHex("0000F1"));
            } else if (payload == opal::revertPayload() && sessionOpen && tsn == 0x1234) {
                reverted = true;
                sessionOpen = false;
                queued = reply(QByteArray::fromHex("F0F1") + end);
            } else {
                queued = reply(QByteArray::fromHex("F0F1F9F03F0000F1"));
            }
            pendingOnce = true;
            return true;
        };
        t.receive = [this](quint16, QByteArray *data, int length) {
            if (pendingOnce) {
                pendingOnce = false;
                QByteArray empty(length, '\0');
                qToBigEndian<quint32>(1, empty.data() + 8); // outstanding data, none yet
                *data = empty;
                return true;
            }
            *data = queued.leftJustified(length, '\0');
            return true;
        };
        return t;
    }
};

} // namespace

// Self-Encrypting Drive: what's put on the wire, read back against TCG's token rules by hand;
// a pretend drive's discovery; the PSID reset refused with the wrong PSID and done with the right one.
void opalTests()
{
    // StartSession to the session manager, PSID authority, Admin SP, as the Opal spec lays it out.
    const QByteArray psid = "0123456789ABCDEF0123456789ABCDEF";
    const QByteArray expected = QByteArray::fromHex("F8" "A800000000000000FF" "A8000000000000FF02" "F0" "8401020304" "A80000020500000001" "01"
                                                    "F200D020") + psid + QByteArray::fromHex("F3" "F203A8000000090001FF01F3" "F1" "F9F0000000F1");
    report(opal::startSessionPayload(0x01020304, psid) == expected, QStringLiteral("opal: StartSession is encoded token by token as the spec has it"),
           QString::fromLatin1(opal::startSessionPayload(0x01020304, psid).toHex()));
    report(opal::revertPayload() == QByteArray::fromHex("F8A80000020500000001A80000000600000202F0F1F9F0000000F1"),
           QStringLiteral("opal: Revert on the Admin SP"));
    const QByteArray p = opal::packet(0x1004, 7, 9, QByteArray(10, 'x'));
    report(p.size() == 512 && qFromBigEndian<quint16>(p.constData() + 4) == 0x1004 && qFromBigEndian<quint32>(p.constData() + 16) == 24 + 12 + 12
               && qFromBigEndian<quint32>(p.constData() + 20) == 7 && qFromBigEndian<quint32>(p.constData() + 24) == 9
               && qFromBigEndian<quint32>(p.constData() + 40) == 12 + 12 && qFromBigEndian<quint32>(p.constData() + 52) == 10,
           QStringLiteral("opal: ComPacket, Packet and SubPacket lengths, padded to 512"));
    report(opal::cleanPsid(QStringLiteral(" abcd-EFGH 1234 ")) == QLatin1String("ABCDEFGH1234"), QStringLiteral("opal: a PSID typed with dashes and spaces"));

    FakeDrive drive;
    opal::Transport t = drive.transport();
    QString why;
    const opal::Discovery d = opal::discover(t, &why);
    report(d.answered && d.ssc == QLatin1String("Opal 2") && d.baseComId == 0x1004 && d.lockingSupported && !d.lockingEnabled && d.mediaEncryption
               && d.mbrEnabled && d.blockSid && d.psidPossible,
           QStringLiteral("opal: a drive's discovery read: Opal 2, its ComID, locking, encryption, shadow MBR, Block SID"), opal::discoveryToLines(d).simplified());
    report(opal::discoveryFromLines(opal::discoveryToLines(d)).baseComId == 0x1004, QStringLiteral("opal: the root helper's lines read back"));
    QString wrongWhy;
    const opal::RevertResult wrong = opal::revertWithPsid(t, d, QStringLiteral("ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ"), &wrongWhy);
    report(wrong == opal::RevertResult::WrongPsid && !drive.reverted && wrongWhy.contains(QLatin1String("isn't this drive's")),
           QStringLiteral("opal: the wrong PSID: refused, nothing erased"), wrongWhy);
    const opal::RevertResult right = opal::revertWithPsid(t, d, QStringLiteral("abcdefgh-jklmnpqr-stuvwxyz-23456789"), &why);
    report(right == opal::RevertResult::Done && drive.reverted && drive.sessions == 2, QStringLiteral("opal: the right PSID: the drive reverted"), why);
    QString shortWhy;
    report(opal::revertWithPsid(t, d, QStringLiteral("ABC"), &shortWhy) == opal::RevertResult::WrongPsid && drive.sessions == 2,
           QStringLiteral("opal: a PSID that's too short isn't even sent"), shortWhy);
    opal::Discovery none;
    report(opal::describe(none).value(0).startsWith(QLatin1String("It isn't a self-encrypting drive")), QStringLiteral("opal: a drive that doesn't answer isn't one"));
    report(opal::parseDiscovery(QByteArray(2048, '\0')).ssc.isEmpty(), QStringLiteral("opal: an empty answer is no drive"));
}

void luksTests()
{
    opalTests();
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
