// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "opal.h"

#include "blockio.h"

#include <QCoreApplication>
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>
#include <QThread>
#include <QtEndian>

#include <memory>

#include <cstdio>
#include <cstring>

#ifdef Q_OS_LINUX
#include <fcntl.h>
#include <linux/nvme_ioctl.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#include <ntddscsi.h>
#endif

namespace opal {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("opal", text);
}

QString tr(const char *text, const char *comment, int n)
{
    return QCoreApplication::translate("opal", text, comment, n);
}

// Tokens (TCG Storage Architecture Core, 3.2.2.3).
constexpr char kCall = char(0xF8);
constexpr char kStartList = char(0xF0);
constexpr char kEndList = char(0xF1);
constexpr char kStartName = char(0xF2);
constexpr char kEndName = char(0xF3);
constexpr char kEndOfData = char(0xF9);

// UIDs.
const QByteArray kSessionManager = QByteArray::fromHex("00000000000000FF");
const QByteArray kStartSession = QByteArray::fromHex("000000000000FF02");
const QByteArray kSyncSession = QByteArray::fromHex("000000000000FF03");
const QByteArray kAdminSp = QByteArray::fromHex("0000020500000001");
const QByteArray kPsidAuthority = QByteArray::fromHex("000000090001FF01");
const QByteArray kRevert = QByteArray::fromHex("0000000600000202");

QByteArray uintAtom(quint64 v)
{
    if (v < 64)
        return QByteArray(1, char(v));
    QByteArray bytes;
    while (v) {
        bytes.prepend(char(v & 0xFF));
        v >>= 8;
    }
    return char(0x80 | bytes.size()) + bytes;
}

QByteArray bytesAtom(const QByteArray &b)
{
    if (b.size() < 16)
        return char(0xA0 | b.size()) + b;
    if (b.size() < 2048) {
        QByteArray out(2, '\0');
        out[0] = char(0xD0 | (b.size() >> 8));
        out[1] = char(b.size() & 0xFF);
        return out + b;
    }
    QByteArray out(4, '\0');
    out[0] = char(0xE2);
    out[1] = char(b.size() >> 16);
    out[2] = char(b.size() >> 8);
    out[3] = char(b.size());
    return out + b;
}

QByteArray statusList()
{
    return QByteArray(1, kEndOfData) + kStartList + char(0) + char(0) + char(0) + kEndList;
}

// A token read back: a control byte, a number or bytes.
struct Token {
    enum Kind { Control, Number, Bytes } kind = Control;
    quint8 control = 0;
    quint64 number = 0;
    QByteArray bytes;
};

QVector<Token> tokens(const QByteArray &payload)
{
    QVector<Token> out;
    qsizetype i = 0;
    while (i < payload.size()) {
        const quint8 b = quint8(payload[i]);
        Token t;
        qsizetype length = 0, header = 1;
        bool isBytes = false;
        if (b < 0x80) { // tiny
            t.kind = Token::Number;
            t.number = b & 0x3F;
            out << t;
            ++i;
            continue;
        } else if (b < 0xC0) { // short
            length = b & 0x0F;
            isBytes = b & 0x20;
        } else if (b < 0xE0) { // medium
            if (i + 1 >= payload.size())
                break;
            length = ((b & 0x07) << 8) | quint8(payload[i + 1]);
            isBytes = b & 0x10;
            header = 2;
        } else if (b < 0xF0) { // long
            if (i + 3 >= payload.size())
                break;
            length = (quint8(payload[i + 1]) << 16) | (quint8(payload[i + 2]) << 8) | quint8(payload[i + 3]);
            isBytes = b & 0x02;
            header = 4;
        } else {
            t.kind = Token::Control;
            t.control = b;
            if (b != 0xFF) // 0xFF: an empty atom, nothing
                out << t;
            ++i;
            continue;
        }
        if (i + header + length > payload.size())
            break;
        const QByteArray data = payload.mid(i + header, length);
        if (isBytes) {
            t.kind = Token::Bytes;
            t.bytes = data;
        } else {
            t.kind = Token::Number;
            for (char c : data)
                t.number = (t.number << 8) | quint8(c);
        }
        out << t;
        i += header + length;
    }
    return out;
}

// The payload inside a ComPacket / Packet / SubPacket.
QByteArray payloadOf(const QByteArray &reply)
{
    if (reply.size() < 56)
        return {};
    const quint32 length = qFromBigEndian<quint32>(reply.constData() + 52);
    return reply.mid(56, qsizetype(std::min<quint32>(length, quint32(reply.size() - 56))));
}

QString statusName(int status)
{
    switch (status) {
    case 0x01:
        return tr("not authorised (the PSID isn't this drive's)");
    case 0x03:
        return tr("the drive is busy with another session");
    case 0x05:
        return tr("not allowed now (the drive refuses it in this state)");
    case 0x0C:
        return tr("the drive has no room for another session");
    case 0x12:
        return tr("the drive refused, after too many wrong tries: power it off and on again first");
    case 0x3F:
        return tr("the drive failed doing it");
    default:
        return tr("the drive said no (status %1)").arg(status);
    }
}

// Sends `payload` and waits for the answer.
bool exchange(Transport &t, quint16 comId, quint32 tsn, quint32 hsn, const QByteArray &payload, QByteArray *answer, QString *why)
{
    if (!t.send(comId, packet(comId, tsn, hsn, payload))) {
        *why = tr("The drive didn't take the command.");
        return false;
    }
    for (int tries = 0; tries < 200; ++tries) {
        QByteArray reply;
        if (!t.receive(comId, &reply, 2048)) {
            *why = tr("The drive didn't answer.");
            return false;
        }
        const quint32 outstanding = reply.size() >= 20 ? qFromBigEndian<quint32>(reply.constData() + 8) : 0;
        const quint32 length = reply.size() >= 20 ? qFromBigEndian<quint32>(reply.constData() + 16) : 0;
        if (length > 0) {
            *answer = payloadOf(reply);
            if (qEnvironmentVariableIsSet("DISKANVIL_OPAL_TRACE"))
                std::fprintf(stderr, "sent %s\nanswer %s\n", payload.toHex().constData(), reply.left(56 + int(answer->size())).toHex().constData());
            return true;
        }
        if (outstanding == 0 && tries > 2)
            break;
        QThread::msleep(25); // still working on it
    }
    *why = tr("The drive gave no answer in time.");
    return false;
}

} // namespace

QByteArray packet(quint16 comId, quint32 tsn, quint32 hsn, const QByteArray &payload)
{
    QByteArray sub = payload;
    sub.append((4 - sub.size() % 4) % 4, '\0');
    QByteArray out(56, '\0');
    char *p = out.data();
    qToBigEndian<quint16>(comId, p + 4);
    qToBigEndian<quint32>(quint32(24 + 12 + sub.size()), p + 16); // ComPacket: the packet after it
    qToBigEndian<quint32>(tsn, p + 20);
    qToBigEndian<quint32>(hsn, p + 24);
    qToBigEndian<quint32>(quint32(12 + sub.size()), p + 40);       // Packet: the subpacket after it
    qToBigEndian<quint32>(quint32(payload.size()), p + 52);        // SubPacket: its payload, unpadded
    out += sub;
    out.append((512 - out.size() % 512) % 512, '\0');
    return out;
}

QByteArray startSessionPayload(quint32 hostSession, const QByteArray &psid)
{
    QByteArray p;
    p += kCall;
    p += bytesAtom(kSessionManager);
    p += bytesAtom(kStartSession);
    p += kStartList;
    p += uintAtom(hostSession);
    p += bytesAtom(kAdminSp);
    p += uintAtom(1); // a write session
    p += kStartName + uintAtom(0) + bytesAtom(psid) + kEndName;           // HostChallenge
    p += kStartName + uintAtom(3) + bytesAtom(kPsidAuthority) + kEndName; // HostSigningAuthority
    p += kEndList;
    p += statusList();
    return p;
}

QByteArray revertPayload()
{
    QByteArray p;
    p += kCall;
    p += bytesAtom(kAdminSp);
    p += bytesAtom(kRevert);
    p += kStartList;
    p += kEndList;
    p += statusList();
    return p;
}

quint32 spSessionFrom(const QByteArray &reply, quint8 *status)
{
    const QVector<Token> t = tokens(reply);
    *status = 0xFF;
    const int s = methodStatus(reply);
    if (s >= 0)
        *status = quint8(s);
    // CALL SMUID SyncSession [ HostSessionID SPSessionID ... ]
    for (int i = 0; i + 5 < t.size(); ++i) {
        if (t[i].kind == Token::Control && t[i].control == 0xF8 && t[i + 2].kind == Token::Bytes && t[i + 2].bytes == kSyncSession
            && t[i + 3].kind == Token::Control && t[i + 3].control == 0xF0 && t[i + 4].kind == Token::Number && t[i + 5].kind == Token::Number)
            return quint32(t[i + 5].number);
    }
    return 0;
}

int methodStatus(const QByteArray &reply)
{
    const QVector<Token> t = tokens(reply);
    for (int i = int(t.size()) - 1; i >= 0; --i) {
        if (t[i].kind == Token::Control && t[i].control == 0xF9) {
            if (i + 2 < t.size() && t[i + 1].kind == Token::Control && t[i + 1].control == 0xF0 && t[i + 2].kind == Token::Number)
                return int(t[i + 2].number);
            return -1;
        }
    }
    return -1;
}

Discovery parseDiscovery(const QByteArray &d)
{
    Discovery out;
    if (d.size() < 48)
        return out;
    const quint32 length = qFromBigEndian<quint32>(d.constData());
    const qsizetype end = std::min<qsizetype>(d.size(), qsizetype(length) + 4);
    if (length < 44)
        return out;
    out.answered = true;
    qsizetype at = 48;
    auto comId = [&](qsizetype p) { return p + 6 <= end ? qFromBigEndian<quint16>(d.constData() + p + 4) : quint16(0); };
    while (at + 4 <= end) {
        const quint16 code = qFromBigEndian<quint16>(d.constData() + at);
        const int len = quint8(d[at + 3]);
        if (at + 4 + len > end)
            break;
        const quint8 b4 = len > 0 ? quint8(d[at + 4]) : 0;
        auto ssc = [&](const char *name, bool psid) {
            // The better one wins when a drive lists more than one.
            if (out.ssc.isEmpty() || out.ssc.startsWith(QLatin1String("Opal 1")) || out.ssc.startsWith(QLatin1String("Pyrite"))) {
                out.ssc = QString::fromLatin1(name);
                out.baseComId = comId(at);
                out.psidPossible = psid;
            }
        };
        switch (code) {
        case 0x0002:
            out.lockingSupported = b4 & 0x01;
            out.lockingEnabled = b4 & 0x02;
            out.locked = b4 & 0x04;
            out.mediaEncryption = b4 & 0x08;
            out.mbrEnabled = b4 & 0x10;
            out.mbrDone = b4 & 0x20;
            break;
        case 0x0100:
            if (out.ssc.isEmpty()) {
                out.ssc = QStringLiteral("Enterprise");
                out.baseComId = comId(at);
            }
            break;
        case 0x0200:
            ssc("Opal 1", true);
            break;
        case 0x0203:
            ssc("Opal 2", true);
            break;
        case 0x0301:
            ssc("Opalite", true);
            break;
        case 0x0302:
            ssc("Pyrite 1", true);
            break;
        case 0x0303:
            ssc("Pyrite 2", true);
            break;
        case 0x0304:
            ssc("Ruby", true);
            break;
        case 0x0402:
            out.blockSid = b4 & 0x02;
            break;
        default:
            break;
        }
        at += 4 + len;
    }
    return out;
}

Discovery discover(Transport &t, QString *why)
{
    QByteArray level0;
    if (!t.discover || !t.discover(&level0)) {
        *why = tr("The drive didn't answer the question: it isn't a self-encrypting drive, or what it's plugged into (a USB "
                  "adapter, say) doesn't pass the question on.");
        return {};
    }
    const Discovery d = parseDiscovery(level0);
    if (!d.answered)
        *why = tr("The drive's answer wasn't one: it isn't a self-encrypting drive.");
    return d;
}

QString cleanPsid(const QString &typed)
{
    QString out;
    for (QChar c : typed)
        if (c.isLetterOrNumber() && c.unicode() < 128)
            out += c.toUpper();
    return out;
}

RevertResult revertWithPsid(Transport &t, const Discovery &d, const QString &psid, QString *why)
{
    if (!d.psidPossible || !d.baseComId) {
        *why = d.ssc == QLatin1String("Enterprise")
                   ? tr("This is an Enterprise drive: its reset is done with the maker's own tool.")
                   : tr("This drive doesn't have a PSID reset.");
        return RevertResult::NotSupported;
    }
    const QString clean = cleanPsid(psid);
    if (clean.size() != 32) {
        *why = tr("A PSID has 32 letters and digits; that's %n.", nullptr, int(clean.size()));
        return RevertResult::WrongPsid;
    }
    constexpr quint32 hostSession = 0x44415631; // "DAV1"
    QByteArray answer;
    if (!exchange(t, d.baseComId, 0, 0, startSessionPayload(hostSession, clean.toLatin1()), &answer, why))
        return RevertResult::Failed;
    quint8 status = 0;
    const quint32 spSession = spSessionFrom(answer, &status);
    if (status == 0x01 || status == 0x12) {
        *why = statusName(status);
        return RevertResult::WrongPsid;
    }
    if (!spSession || status != 0) {
        *why = tr("The drive wouldn't start a session: %1").arg(statusName(status));
        return RevertResult::Failed;
    }
    if (!exchange(t, d.baseComId, spSession, hostSession, revertPayload(), &answer, why))
        return RevertResult::Failed;
    const int done = methodStatus(answer);
    if (done != 0) {
        *why = statusName(done);
        return RevertResult::Failed;
    }
    return RevertResult::Done;
}

QStringList describe(const Discovery &d)
{
    QStringList out;
    if (!d.answered || d.ssc.isEmpty()) {
        out << tr("It isn't a self-encrypting drive (or it doesn't say so through what it's plugged into). Nothing to do here.");
        return out;
    }
    const bool encrypts = d.mediaEncryption && !d.ssc.startsWith(QLatin1String("Pyrite"));
    out << (encrypts ? tr("It's a self-encrypting drive (TCG %1): it encrypts everything it stores, always.").arg(d.ssc)
                     : tr("It follows TCG %1: it can be locked with a password, but doesn't encrypt.").arg(d.ssc));
    if (!d.lockingEnabled)
        out << tr("Its locking is off: it works like any other drive, and nobody has set a password on it.");
    else if (d.locked)
        out << tr("It's locked right now: its contents can't be read until it's unlocked with its password.");
    else
        out << tr("Its locking is on (a password was set), but it's unlocked right now.");
    if (d.mbrEnabled && !d.mbrDone)
        out << tr("It shows a small stand-in disk (a \"shadow MBR\") until it's unlocked: that's the unlock screen some computers "
                  "start from. Its real contents are behind it.");
    if (d.lockingEnabled)
        out << (d.psidPossible ? tr("Without the password, the only way back is a PSID reset with the code on its label: it erases everything.")
                               : tr("Without the password, the maker's own tool is the way back."));
    if (d.blockSid)
        out << tr("Its firmware has blocked taking ownership until it's next switched off and on (normal on many PCs).");
    return out;
}

QString discoveryToLines(const Discovery &d)
{
    QStringList out;
    out << QStringLiteral("answered=%1").arg(d.answered) << QStringLiteral("ssc=%1").arg(d.ssc) << QStringLiteral("comid=%1").arg(d.baseComId)
        << QStringLiteral("supported=%1").arg(d.lockingSupported) << QStringLiteral("enabled=%1").arg(d.lockingEnabled)
        << QStringLiteral("locked=%1").arg(d.locked) << QStringLiteral("encrypts=%1").arg(d.mediaEncryption)
        << QStringLiteral("mbr=%1").arg(d.mbrEnabled) << QStringLiteral("mbrdone=%1").arg(d.mbrDone)
        << QStringLiteral("blocksid=%1").arg(d.blockSid) << QStringLiteral("psid=%1").arg(d.psidPossible);
    return out.join(QLatin1Char('\n'));
}

Discovery discoveryFromLines(const QString &lines)
{
    Discovery d;
    for (const QString &line : lines.split(QLatin1Char('\n'))) {
        const QString key = line.section(QLatin1Char('='), 0, 0), value = line.section(QLatin1Char('='), 1);
        const bool on = value == QLatin1String("1");
        if (key == QLatin1String("answered"))
            d.answered = on;
        else if (key == QLatin1String("ssc"))
            d.ssc = value;
        else if (key == QLatin1String("comid"))
            d.baseComId = quint16(value.toUInt());
        else if (key == QLatin1String("supported"))
            d.lockingSupported = on;
        else if (key == QLatin1String("enabled"))
            d.lockingEnabled = on;
        else if (key == QLatin1String("locked"))
            d.locked = on;
        else if (key == QLatin1String("encrypts"))
            d.mediaEncryption = on;
        else if (key == QLatin1String("mbr"))
            d.mbrEnabled = on;
        else if (key == QLatin1String("mbrdone"))
            d.mbrDone = on;
        else if (key == QLatin1String("blocksid"))
            d.blockSid = on;
        else if (key == QLatin1String("psid"))
            d.psidPossible = on;
    }
    return d;
}

// The drive itself.

#ifdef Q_OS_LINUX
namespace {

bool scsiSecurity(int fd, bool in, quint16 comId, char *buf, int length)
{
    unsigned char cdb[12] = {};
    cdb[0] = in ? 0xA2 : 0xB5;
    cdb[1] = 0x01; // the TCG protocol
    cdb[2] = quint8(comId >> 8);
    cdb[3] = quint8(comId);
    cdb[4] = 0x80; // the length in 512-byte blocks
    const quint32 blocks = quint32(length / 512);
    cdb[6] = quint8(blocks >> 24);
    cdb[7] = quint8(blocks >> 16);
    cdb[8] = quint8(blocks >> 8);
    cdb[9] = quint8(blocks);
    unsigned char sense[32] = {};
    sg_io_hdr_t io {};
    io.interface_id = 'S';
    io.dxfer_direction = in ? SG_DXFER_FROM_DEV : SG_DXFER_TO_DEV;
    io.cmd_len = sizeof cdb;
    io.cmdp = cdb;
    io.mx_sb_len = sizeof sense;
    io.sbp = sense;
    io.dxfer_len = unsigned(length);
    io.dxferp = buf;
    io.timeout = 20000;
    return ::ioctl(fd, SG_IO, &io) == 0 && io.status == 0 && io.host_status == 0 && io.driver_status == 0;
}

bool nvmeSecurity(int fd, bool in, quint16 comId, char *buf, int length)
{
    nvme_admin_cmd cmd {};
    cmd.opcode = in ? 0x82 : 0x81;
    cmd.addr = quint64(quintptr(buf));
    cmd.data_len = quint32(length);
    cmd.cdw10 = (0x01u << 24) | (quint32(comId) << 8);
    cmd.cdw11 = quint32(length);
    cmd.timeout_ms = 20000;
    return ::ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd) == 0;
}

} // namespace

Transport open(const QString &device, QString *why)
{
    Transport t;
    static const QRegularExpression nvme(QStringLiteral("^/dev/nvme\\d+n\\d+$")), scsi(QStringLiteral("^/dev/sd[a-z]{1,3}$"));
    const bool isNvme = nvme.match(device).hasMatch();
    if (!isNvme && !scsi.match(device).hasMatch()) {
        *why = tr("Only whole SATA, SAS, USB and NVMe drives can be asked.");
        return t;
    }
    const int fd = ::open(QFile::encodeName(device).constData(), (isNvme ? O_RDONLY : O_RDWR) | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        *why = tr("The drive couldn't be opened: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        return t;
    }
    // The fd goes when the last copy of the functions does.
    auto keep = std::shared_ptr<void>(nullptr, [fd](void *) { ::close(fd); });
    auto call = [fd, isNvme, keep](bool in, quint16 comId, char *buf, int length) {
        return isNvme ? nvmeSecurity(fd, in, comId, buf, length) : scsiSecurity(fd, in, comId, buf, length);
    };
    t.send = [call](quint16 comId, const QByteArray &data) {
        blockio::Buffer buf = blockio::alignedBuffer(size_t(data.size()));
        std::memcpy(buf.get(), data.constData(), size_t(data.size()));
        return call(false, comId, buf.get(), int(data.size()));
    };
    t.receive = [call](quint16 comId, QByteArray *data, int length) {
        blockio::Buffer buf = blockio::alignedBuffer(size_t(length));
        std::memset(buf.get(), 0, size_t(length));
        if (!call(true, comId, buf.get(), length))
            return false;
        *data = QByteArray(buf.get(), length);
        return true;
    };
    t.discover = [receive = t.receive](QByteArray *level0) { return receive(0x0001, level0, 2048); };
    return t;
}
#elif defined(Q_OS_WIN)
namespace {

bool scsiSecurity(HANDLE h, bool in, quint16 comId, char *buf, int length)
{
    struct {
        SCSI_PASS_THROUGH_DIRECT sptd;
        UCHAR sense[32];
    } s {};
    s.sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    s.sptd.CdbLength = 12;
    s.sptd.SenseInfoLength = sizeof s.sense;
    s.sptd.SenseInfoOffset = offsetof(decltype(s), sense);
    s.sptd.DataIn = in ? SCSI_IOCTL_DATA_IN : SCSI_IOCTL_DATA_OUT;
    s.sptd.DataTransferLength = ULONG(length);
    s.sptd.DataBuffer = buf;
    s.sptd.TimeOutValue = 20;
    UCHAR *cdb = s.sptd.Cdb;
    cdb[0] = in ? 0xA2 : 0xB5;
    cdb[1] = 0x01;
    cdb[2] = UCHAR(comId >> 8);
    cdb[3] = UCHAR(comId);
    cdb[4] = 0x80;
    const ULONG blocks = ULONG(length / 512);
    cdb[6] = UCHAR(blocks >> 24);
    cdb[7] = UCHAR(blocks >> 16);
    cdb[8] = UCHAR(blocks >> 8);
    cdb[9] = UCHAR(blocks);
    DWORD got = 0;
    return DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH_DIRECT, &s, sizeof s, &s, sizeof s, &got, nullptr) && s.sptd.ScsiStatus == 0;
}

} // namespace

Transport open(const QString &device, QString *why)
{
    Transport t;
    const HANDLE h = CreateFileW(reinterpret_cast<const wchar_t *>(device.utf16()), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *why = tr("The drive couldn't be opened (error %1).").arg(GetLastError());
        return t;
    }
    auto keep = std::shared_ptr<void>(h, [](void *p) { CloseHandle(HANDLE(p)); });
    t.send = [keep](quint16 comId, const QByteArray &data) {
        blockio::Buffer buf = blockio::alignedBuffer(size_t(data.size()));
        std::memcpy(buf.get(), data.constData(), size_t(data.size()));
        return scsiSecurity(HANDLE(keep.get()), false, comId, buf.get(), int(data.size()));
    };
    t.receive = [keep](quint16 comId, QByteArray *data, int length) {
        blockio::Buffer buf = blockio::alignedBuffer(size_t(length));
        std::memset(buf.get(), 0, size_t(length));
        if (!scsiSecurity(HANDLE(keep.get()), true, comId, buf.get(), length))
            return false;
        *data = QByteArray(buf.get(), length);
        return true;
    };
    t.discover = [receive = t.receive](QByteArray *level0) { return receive(0x0001, level0, 2048); };
    return t;
}
#else
Transport open(const QString &, QString *why)
{
    *why = tr("Not on this system.");
    return {};
}
#endif

void close(Transport &t)
{
    t = Transport();
}

int runOption(const QString &device, const QString &what)
{
    QTextStream out(stdout);
    QString why;
    Transport t = open(device, &why);
    if (!t.send) {
        out << "error=" << why << Qt::endl;
        return 1;
    }
    const Discovery d = discover(t, &why);
    out << discoveryToLines(d) << Qt::endl;
    if (what != QLatin1String("revert"))
        return 0;
    // The PSID from standard input, never the command line (anyone can see that).
    QTextStream in(stdin);
    const QString psid = in.readLine();
    const RevertResult r = revertWithPsid(t, d, psid, &why);
    out << "revert=" << int(r) << Qt::endl;
    if (!why.isEmpty())
        out << "error=" << why << Qt::endl;
    return r == RevertResult::Done ? 0 : 1;
}

} // namespace opal
