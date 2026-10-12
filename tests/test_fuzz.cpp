// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Crash tests. Everything DiskForge reads that someone else could have made (add-ons, the
// online list and its signature, backup descriptions, rescue maps, partition tables,
// GitHub's update answer, mount tables, drive names) gets thousands of damaged versions of
// a good file. Nothing may crash or hang, and nothing broken may come out looking valid.
// release.sh also runs this in a build with AddressSanitizer, which catches memory errors
// that don't crash.
//
//   DISKFORGE_FUZZ_SEED=n    repeat a run (the seed is printed)
//   DISKFORGE_FUZZ_ROUNDS=n  rounds per kind of file (default 3000)

#include "testkit.h"

#include "../src/backup/ewf.h"
#include "../src/backup/zstdseek.h"
#include "../src/backup/partclone.h"
#include "../src/viruses/peinfo.h"
#include "../src/drives/luksheader.h"
#include "../src/addons/addons.h"
#include "../src/backup/clone.h"
#include "../src/common/format.h"
#include "../src/drives/gpt.h"
#include "../src/health/btrfscheck.h"
#include "../src/health/firmware.h"
#include "../src/health/health.h"
#include "../src/drives/partrecover.h"
#include "../src/drives/partscan.h"
#include "../src/backup/imagebackup.h"
#include "../src/addons/outputfilter.h"
#include "../src/backup/rescuecopy.h"
#include "../src/addons/signature.h"
#include "../src/tools/snapper.h"
#include "../src/app/updates.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSet>
#include <QJsonDocument>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>

#include <zstd.h>

#include <algorithm>
#include <cctype>

#include <fcntl.h>
#include <unistd.h>

namespace {

QRandomGenerator rng;

const QList<QByteArray> kTokens = {
    "{", "}", "[", "]", "\"", ",", ":", "null", "true", "-1", "0", "1e999", "18446744073709551615", "4294967296",
    "\\u0000", "\\ud800", "\\u202e", "\n", "\r", " ", "-", "--", "..", "/", "{label}", "{mountpoint}", "{home}", "<b>",
    "\xff\xfe", "\xc3\x28", "#", "+", "?", "0x", "-----END SSH SIGNATURE-----", "=", "{ask:a}", "{setting:d}", "{ask:}",
    "\"ask\":[{\"id\":\"a\"}],", "\"output\":\"window\",", "\"type\":\"choice\",", "\"min\":-1,", "#ff0000", "#12345",
};
const QList<QByteArray> kNumbers = {
    "0", "1", "-1", "4294967295", "4294967296", "18446744073709551615", "18446744073709551616",
    "0xffffffffffffffff", "99999999999999999999999", "1e308",
};

QByteArray mutate(QByteArray data)
{
    const int edits = 1 + int(rng.bounded(8));
    for (int i = 0; i < edits; ++i) {
        const qsizetype pos = data.isEmpty() ? 0 : qsizetype(rng.bounded(quint32(data.size())));
        switch (rng.bounded(7)) {
        case 0:
            if (!data.isEmpty())
                data[pos] = char(data[pos] ^ char(1 << rng.bounded(8)));
            break;
        case 1:
            if (!data.isEmpty())
                data[pos] = char(rng.bounded(256));
            break;
        case 2:
            data.insert(pos, kTokens[qsizetype(rng.bounded(quint32(kTokens.size())))]);
            break;
        case 3:
            data.remove(pos, qsizetype(rng.bounded(16)) + 1);
            break;
        case 4:
            data.insert(pos, data.mid(pos, qsizetype(rng.bounded(64))));
            break;
        case 5:
            data.truncate(pos + qsizetype(rng.bounded(4)));
            break;
        case 6: { // swap a number for an extreme one
            qsizetype start = pos;
            while (start < data.size() && !std::isdigit(uchar(data[start])))
                ++start;
            qsizetype end = start;
            while (end < data.size() && std::isxdigit(uchar(data[end])))
                ++end;
            if (start < data.size())
                data.replace(start, end - start, kNumbers[qsizetype(rng.bounded(quint32(kNumbers.size())))]);
            break;
        }
        }
    }
    return data;
}

int rounds()
{
    bool ok = false;
    const int n = qEnvironmentVariableIntValue("DISKFORGE_FUZZ_ROUNDS", &ok);
    return ok && n > 0 ? n : 3000;
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

const QRegularExpression kId(QStringLiteral("^[a-z0-9][a-z0-9-]*$"));

void addonFiles(const QString &tmp)
{
    QList<QByteArray> seeds;
    const QString examples = QStringLiteral(SOURCE_DIR "/examples/addons");
    for (const QString &name : QDir(examples).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        seeds << readFile(examples + QLatin1Char('/') + name + QStringLiteral("/addon.json"));
    seeds << R"({"id":"x","actions":[{"label":"L","look_only":true,"system_disks":true,"applies_to":"any",
                "when":["mounted","filesystem:ext4|vfat"],"confirm":"Sure about {label}?","command":["{home}/bin/x","--n={label}","{uuid}"]}]})";
    seeds << R"({"id":"full","settings":[{"id":"dest","type":"folder","default":"{home}/Backups"}],
                "theme":{"palette":{"window":"#0a0a0f"},"colors":{"danger":"#ff2d55"},"filesystems":{"ext4":"#00b4ff"},"usage":["#112233"]},
                "actions":[{"label":"Copy","output":"window","command":["rsync","{ask:c}","{ask:m}","--n={ask:t}","{setting:dest}/{label}"],
                "ask":[{"id":"c","type":"check","on":"--checksum","off":""},{"id":"m","type":"choice","choices":["-q","-v"]},
                       {"id":"t","type":"text","default":"{label}"},{"id":"n","type":"number","min":1,"max":9}]}]})";
    int accepted = 0;
    QStringList bad;
    Disk disk;
    disk.device = QStringLiteral("/dev/sdz");
    Volume vol;
    vol.device = QStringLiteral("/dev/sdz1");
    vol.label = QStringLiteral("stick");
    vol.mountPoints = {QStringLiteral("/run/media/me/stick")};
    disk.volumes = {vol};
    for (int i = 0; i < rounds(); ++i) {
        const Addon a = Addons::parseData(mutate(seeds[qsizetype(rng.bounded(quint32(seeds.size())))]), QString());
        if (!a.error.isEmpty())
            continue;
        ++accepted;
        bool ok = kId.match(a.id).hasMatch() && (!a.actions.isEmpty() || !a.theme.isEmpty());
        for (const AddonAction &act : a.actions) {
            ok = ok && !act.label.isEmpty() && !act.command.isEmpty() && !act.command.first().isEmpty();
            // The program can't come from the drive or a form: no "{" but {home}.
            ok = ok && !QString(act.command.value(0)).remove(QStringLiteral("{home}")).contains(QLatin1Char('{'));
            AddonAction unboxed = act;
            unboxed.lookOnly = false;
            ok = ok && !(act.lookOnly && !Addons::risks(unboxed, a.settings).admin.isEmpty());
            for (const AddonField &f : act.ask)
                ok = ok && !f.id.isEmpty() && f.min >= 0 && f.max >= f.min && (f.type != AddonField::Type::Choice || !f.choices.isEmpty());
            // Whatever it fills in for a drive and random answers: no hidden characters, no
            // empty arguments, and no typed answer at the start of an argument beginning with "-".
            QMap<QString, QString> answers;
            for (const AddonField &f : act.ask)
                answers.insert(f.id, rng.bounded(3) == 0 ? QStringLiteral("-x") : rng.bounded(2) ? QString() : QStringLiteral("value"));
            QString error;
            const QStringList argv = Addons::fillCommand(a, act, disk, &disk.volumes[0], answers, &error);
            for (const QString &part : argv)
                ok = ok && !hasHiddenCharacters(part);
        }
        if (!ok)
            bad << a.id;
    }
    report(bad.isEmpty(), QStringLiteral("%1 damaged add-ons: no crash, and the %2 that still load follow the rules").arg(rounds()).arg(accepted),
           bad.join(QLatin1Char(' ')));

    // Through the file too, the way Install from File and the add-on folder read them.
    const QString file = tmp + QStringLiteral("/addon.json");
    for (int i = 0; i < 200; ++i) {
        writeFile(file, mutate(seeds.value(0)));
        Addons::parse(file);
    }
}

QByteArray catalogSeed()
{
    const QString url = QStringLiteral("https://raw.githubusercontent.com/DannyS124/diskforge-addons/0123456789abcdef0123456789abcdef01234567/addons/a/addon.json");
    return QStringLiteral(R"({"format":"diskforge-addon-catalog","version":1,"addons":[
        {"id":"a","name":"A","version":"1.0","author":"x","description":"d","url":"%1","sha256":"%2"},
        {"id":"b-2","name":"B","version":"2","url":"%1","sha256":"%2"}]})")
        .arg(url, QString(64, QLatin1Char('a')))
        .toUtf8();
}

void catalogs()
{
    const QByteArray seed = catalogSeed();
    static const QRegularExpression sha(QStringLiteral("^[0-9a-f]{64}$"));
    int listed = 0;
    QStringList bad;
    for (int i = 0; i < rounds(); ++i) {
        QString error;
        for (const CatalogEntry &e : Addons::parseCatalog(mutate(seed), &error)) {
            ++listed;
            if (!kId.match(e.id).hasMatch() || !sha.match(e.sha256).hasMatch() || !Addons::isPinnedUrl(e.url))
                bad << e.id;
        }
    }
    report(bad.isEmpty(), QStringLiteral("%1 damaged add-on lists: no crash, and the %2 entries left all check out").arg(rounds()).arg(listed),
           bad.join(QLatin1Char(' ')));
}

// The bytes inside the armor, for comparing two signature files.
QByteArray signaturePayload(QByteArray armored)
{
    armored = armored.trimmed();
    armored.replace("-----BEGIN SSH SIGNATURE-----", "").replace("-----END SSH SIGNATURE-----", "");
    armored.replace('\n', QByteArray()).replace('\r', QByteArray()).replace(' ', QByteArray());
    const auto decoded = QByteArray::fromBase64Encoding(armored, QByteArray::AbortOnBase64DecodingErrors);
    return decoded ? *decoded : QByteArray("not base64");
}

void signatures(const QString &tmp)
{
    if (QStandardPaths::findExecutable(QStringLiteral("ssh-keygen")).isEmpty()) {
        out << "SKIP  ssh-keygen isn't installed, so signatures aren't crash-tested" << Qt::endl;
        return;
    }
    const QString key = tmp + QStringLiteral("/key");
    sh(QStringLiteral("ssh-keygen"), {QStringLiteral("-q"), QStringLiteral("-t"), QStringLiteral("ed25519"), QStringLiteral("-N"), QString(),
                                      QStringLiteral("-C"), QStringLiteral("fuzz"), QStringLiteral("-f"), key});
    const QStringList keys = {QString::fromLatin1(readFile(key + QStringLiteral(".pub"))).trimmed()};
    const QByteArray data = catalogSeed();
    writeFile(tmp + QStringLiteral("/list"), data);
    sh(QStringLiteral("ssh-keygen"), {QStringLiteral("-Y"), QStringLiteral("sign"), QStringLiteral("-f"), key, QStringLiteral("-n"),
                                      QStringLiteral("diskforge-addons"), tmp + QStringLiteral("/list")});
    const QByteArray sig = readFile(tmp + QStringLiteral("/list.sig"));
    QString error;
    report(signature::verify(data, sig, QStringLiteral("diskforge-addons"), keys, &error), QStringLiteral("the untouched signature checks out"), error);

    const QByteArray payload = signaturePayload(sig);
    int forged = 0;
    for (int i = 0; i < rounds(); ++i) {
        const QByteArray damaged = mutate(sig);
        // Only a change that leaves the signed bytes the same (spacing, say) may still pass.
        if (signature::verify(data, damaged, QStringLiteral("diskforge-addons"), keys, nullptr) && signaturePayload(damaged) != payload)
            ++forged;
        const QByteArray changed = mutate(data);
        if (changed != data && signature::verify(changed, sig, QStringLiteral("diskforge-addons"), keys, nullptr))
            ++forged;
    }
    report(forged == 0, QStringLiteral("%1 damaged signatures and lists: none passes").arg(rounds()), QString::number(forged));
}

void updateAnswers()
{
    const QByteArray seed = R"({"url":"https://api.github.com/repos/DannyS124/diskforge/releases/1","html_url":"https://github.com/DannyS124/diskforge/releases/tag/v0.5.0",
        "tag_name":"v0.5.0","name":"DiskForge 0.5.0","draft":false,"prerelease":false,"assets":[{"name":"diskforge-0.5.0.tar.gz","size":123}],
        "body":"notes"})";
    static const QRegularExpression version(QStringLiteral("^\\d{1,4}(\\.\\d{1,4}){1,3}$"));
    QStringList bad;
    for (int i = 0; i < rounds(); ++i) {
        QString error;
        const QString v = parseLatestRelease(mutate(seed), &error);
        if (!v.isEmpty() && (!version.match(v).hasMatch() || !releasePageUrl(v).startsWith(QLatin1String(APP_HOMEPAGE "/releases/tag/v"))))
            bad << v;
    }
    QString error;
    report(bad.isEmpty() && parseLatestRelease(seed, &error) == QLatin1String("0.5.0"),
           QStringLiteral("%1 damaged update answers: only plain version numbers come out").arg(rounds()), bad.join(QLatin1Char(' ')));
    report(parseLatestRelease(R"({"tag_name":"v1.0\"><a href=x>","html_url":"file:///etc/passwd"})", &error).isEmpty(),
           QStringLiteral("a version that isn't a version is refused"));
}

void backupDescriptions(const QString &tmp)
{
    const QByteArray seed = R"({"format":"diskforge-backup","version":1,"kind":"disk","device":"/dev/sdb","model":"Stick","serial":"123",
        "label":"","filesystem":"","table":"gpt","size":"1048576","sectorSize":512,"sha256":"00","fileSha256":"11",
        "app":"DiskForge 0.5.0","created":"2026-10-08T12:00:00Z"})";
    const QString image = tmp + QStringLiteral("/backup.img.zst");
    // A zstd frame header that records its size, then junk; describe() only reads the header.
    QByteArray frame = QByteArray::fromHex("28b52ffd2400010000");
    writeFile(image, frame);
    for (int i = 0; i < rounds(); ++i) {
        const QByteArray damaged = mutate(seed);
        BackupInfo::fromJson(QJsonDocument::fromJson(damaged).object());
        if (i % 10 == 0) {
            writeFile(imagebackup::infoPath(image), damaged);
            imagebackup::describe(image);
        }
    }
    QFile::remove(imagebackup::infoPath(image));
    for (int i = 0; i < 500; ++i) {
        writeFile(image, mutate(frame));
        imagebackup::describe(image);
    }
    const BackupInfo hidden = BackupInfo::fromJson(QJsonDocument::fromJson(R"({"format":"diskforge-backup","version":1,"size":"5",
        "model":"Stick\u202e\u200bX"})").object());
    report(hidden.model == QLatin1String("StickX"), QStringLiteral("damaged backup descriptions: no crash, and hidden characters in names are dropped"),
           hidden.model);
}

void rescueMaps(const QString &tmp)
{
    const quint64 size = 0x1000000;
    const QByteArray seed = "# Mapfile. Created by DiskForge\n# current_pos  current_status  current_pass\n0x00100000     ?               1\n"
                            "#      pos        size  status\n0x00000000  0x00100000  +\n0x00100000  0x00010000  -\n"
                            "0x00110000  0x00010000  *\n0x00120000  0x00EE0000  ?\n";
    const QString file = tmp + QStringLiteral("/rescue.map");
    QStringList bad;
    int loaded = 0;
    for (int i = 0; i < rounds(); ++i) {
        writeFile(file, mutate(seed));
        RescueMap map;
        QString error;
        if (!map.load(file, size, &error))
            continue;
        ++loaded;
        quint64 pos = 0;
        bool ok = map.currentPos <= size;
        for (int b = 0; b < map.blocks().size(); ++b) {
            const RescueMap::Block &block = map.blocks()[b];
            ok = ok && block.pos == pos && block.size <= size - pos && QByteArray("?*/-+").contains(block.status)
                && (b == 0 || map.blocks()[b - 1].status != block.status);
            pos += block.size;
        }
        if (!ok || pos != size)
            bad << QString::number(i);
    }
    report(bad.isEmpty(), QStringLiteral("%1 damaged rescue maps: no crash, and the %2 that load cover the drive exactly").arg(rounds()).arg(loaded),
           bad.join(QLatin1Char(' ')));
    // Two huge blocks that wrap around past 2^64 and land on the right total.
    writeFile(file, "0 ? 1\n0x0 0xffffffffffffffff +\n0xffffffffffffffff 0x1000001 ?\n");
    RescueMap map;
    QString error;
    report(!map.load(file, size, &error), QStringLiteral("a map whose blocks wrap around is refused"), error);
}

void partitionTables(const QString &tmp)
{
    if (QStandardPaths::findExecutable(QStringLiteral("sfdisk")).isEmpty()) {
        out << "SKIP  sfdisk isn't installed, so partition tables aren't crash-tested" << Qt::endl;
        return;
    }
    const QString image = tmp + QStringLiteral("/gpt.img");
    const qint64 imageSize = 4 * 1024 * 1024, edge = 1024 * 1024;
    {
        QFile f(image);
        if (f.open(QIODevice::WriteOnly))
            f.resize(imageSize);
    }
    QProcess sfdisk;
    sfdisk.start(QStringLiteral("sfdisk"), {QStringLiteral("-q"), QStringLiteral("--no-reread"), QStringLiteral("--no-tell-kernel"), image});
    sfdisk.write("label: gpt\nstart=2048, size=1024\nstart=4096, size=2048\n");
    sfdisk.closeWriteChannel();
    sfdisk.waitForFinished();
    const QByteArray pristine = readFile(image);
    report(pristine.size() == imageSize && pristine.mid(512, 8) == "EFI PART", QStringLiteral("made a GPT image to damage"));
    if (pristine.mid(512, 8) != "EFI PART")
        return;

    // Damage the header fields and the partition list, then fix the checksums so the
    // code past the checksum checks gets the damage too.
    auto crc32 = [](const char *data, qsizetype len) {
        quint32 crc = 0xFFFFFFFFu;
        for (qsizetype i = 0; i < len; ++i) {
            crc ^= uchar(data[i]);
            for (int k = 0; k < 8; ++k)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    };
    auto put32 = [](char *p, quint32 v) {
        for (int i = 0; i < 4; ++i)
            p[i] = char(v >> (8 * i));
    };
    auto put64 = [](char *p, quint64 v) {
        for (int i = 0; i < 8; ++i)
            p[i] = char(v >> (8 * i));
    };
    auto get32 = [](const char *p) {
        quint32 v = 0;
        for (int i = 3; i >= 0; --i)
            v = v << 8 | uchar(p[i]);
        return v;
    };
    const QList<quint64> extremes = {0, 1, 2, 33, 34, 127, 128, 4095, 4096, 0xFFFFFFFFu, 0x100000000ull, 0xFFFFFFFFFFFFFFFFull, 1ull << 48};
    const int fd = ::open(QFile::encodeName(image).constData(), O_RDWR);
    int relocated = 0, grew = 0;
    for (int i = 0; i < rounds() / 3; ++i) {
        QByteArray disk = pristine;
        char *h = disk.data() + 512;
        const int fields[] = {12, 24, 32, 40, 48, 72, 80, 84};
        const int changes = 1 + int(rng.bounded(3));
        for (int c = 0; c < changes; ++c) {
            const int field = fields[rng.bounded(8)];
            const quint64 value = rng.bounded(2) ? extremes[qsizetype(rng.bounded(quint32(extremes.size())))] : rng.generate64() % 8192;
            if (field == 12 || field == 80 || field == 84)
                put32(h + field, quint32(value));
            else
                put64(h + field, value);
        }
        for (int c = int(rng.bounded(4)); c > 0; --c) // and some bytes in the partition list
            disk[1024 + qsizetype(rng.bounded(128 * 128))] = char(rng.bounded(256));
        const quint64 entriesBytes = std::min<quint64>(quint64(get32(h + 80)) * get32(h + 84), quint64(edge - 1024));
        put32(h + 88, crc32(disk.constData() + 1024, qsizetype(entriesBytes)));
        put32(h + 16, 0);
        put32(h + 16, crc32(h, qsizetype(std::min<quint32>(get32(h + 12), 512))));
        // Only the first and last MiB matter (tables, backup); the middle stays as it was.
        if (::pwrite(fd, disk.constData(), size_t(edge), 0) != edge
            || ::pwrite(fd, disk.constData() + imageSize - edge, size_t(edge), imageSize - edge) != edge || ::ftruncate(fd, imageSize) != 0)
            break;
        gpt::isGpt(fd, 512);
        if (gpt::relocateBackup(fd, 512, rng.bounded(2)).ok)
            ++relocated;
        grew += QFileInfo(image).size() != imageSize;
    }
    ::close(fd);
    report(grew == 0, QStringLiteral("%1 damaged partition tables: no crash, %2 still relocated, none wrote past the disk's end")
                          .arg(rounds() / 3).arg(relocated), QString::number(grew));
}

void mountTables()
{
    QByteArray seed = readFile(QStringLiteral("/proc/self/mountinfo"));
    seed += "36 35 0:31 /@home /home rw,relatime shared:2 - btrfs /dev/nvme0n1p2 rw,ssd,subvolid=257,subvol=/@home\n"
            "37 35 8:17 / /run/media/me/My\\040Stick rw,nosuid,nodev - vfat /dev/sdb1 rw,uid=1000\n";
    for (int i = 0; i < rounds(); ++i)
        snapper::mountedSubvolumes(mutate(seed));
    report(true, QStringLiteral("%1 damaged mount tables: no crash").arg(rounds()));
}

void clonePlans()
{
    auto upTo = [](quint64 n) { return n == ~quint64(0) ? rng.generate64() : rng.generate64() % (n + 1); };
    auto randomDisk = [&upTo](const QString &path) {
        Disk d;
        d.blockPath = path;
        d.device = QStringLiteral("/dev/") + path.section(QLatin1Char('/'), -1);
        const quint64 sizes[] = {0, 512, 1ull << 20, 1ull << 30, 1ull << 40, 0xFFFFFFFFFFFFFFFFull};
        d.size = rng.bounded(3) ? rng.generate64() % (1ull << 40) : sizes[rng.bounded(6)];
        d.sectorSize = rng.bounded(4) ? 512 : 4096;
        d.tableType = QStringList{QStringLiteral("gpt"), QStringLiteral("dos"), QString()}[rng.bounded(3)];
        for (int n = int(rng.bounded(9)); n > 0; --n) {
            Volume v;
            v.number = n;
            v.offset = rng.bounded(4) ? upTo(d.size) : rng.generate64();
            v.size = rng.bounded(4) ? upTo(d.size) : rng.generate64();
            v.isContainer = rng.bounded(6) == 0;
            v.isContained = !v.isContainer && rng.bounded(4) == 0;
            d.volumes << v;
        }
        return d;
    };
    QStringList bad;
    int planned = 0;
    for (int i = 0; i < rounds(); ++i) {
        const Disk source = randomDisk(QStringLiteral("/org/freedesktop/UDisks2/block_devices/sdy"));
        Disk target = randomDisk(QStringLiteral("/org/freedesktop/UDisks2/block_devices/sdz"));
        if (rng.bounded(2))
            target.size = source.size + rng.generate64() % (1ull << 30);
        target.sectorSize = source.sectorSize;
        const diskclone::Plan plan = diskclone::plan(source, target);
        if (!plan.error.isEmpty())
            continue;
        ++planned;
        for (const blockcopy::Extent &e : plan.extents) {
            if (e.source > source.size || e.length > source.size - e.source || e.target > target.size || e.length > target.size - e.target)
                bad << QString::number(i);
        }
    }
    report(bad.isEmpty(), QStringLiteral("%1 random partition layouts: the %2 clone plans made stay inside both drives").arg(rounds()).arg(planned),
           bad.mid(0, 5).join(QLatin1Char(' ')));
}

void commandOutput()
{
    // Random bytes, escape codes cut anywhere, broken UTF-8: never a crash, never a hidden
    // character or an overlong line in what's shown.
    const QList<QByteArray> pieces = {"\x1b", "[", "31m", "]", "0;t", "\x07", "\\", "\r", "\n", "\b", "\t", "\xc3", "\xa9",
                                      "\xe2\x80\xae", "\xff", "abc", "%", "\x00"};
    QStringList bad;
    for (int i = 0; i < rounds(); ++i) {
        OutputFilter filter;
        QStringList lines;
        for (int n = int(rng.bounded(20)); n > 0; --n) {
            QByteArray chunk;
            for (int k = int(rng.bounded(8)); k > 0; --k)
                chunk += rng.bounded(4) ? pieces[qsizetype(rng.bounded(quint32(pieces.size())))] : QByteArray(1, char(rng.bounded(256)));
            lines += filter.feed(chunk);
        }
        lines << filter.current();
        for (const QString &line : std::as_const(lines)) {
            if (hasHiddenCharacters(line) || line.size() > OutputFilter::kMaxLine + 1)
                bad << QString::number(i);
        }
    }
    report(bad.isEmpty(), QStringLiteral("%1 streams of junk output: shown as plain, short lines").arg(rounds()), bad.mid(0, 5).join(QLatin1Char(' ')));
}

void healthVerdicts()
{
    // Random SMART values: never a crash, and the verdict always matches its worst reason.
    const QList<qint64> extremes = {-1, 0, 1, 15, 16, 17, 0xFFFF, 0x10000, 0x7fffffffffffffffLL};
    QStringList bad;
    for (int i = 0; i < rounds(); ++i) {
        health::AtaInput ata;
        for (int n = int(rng.bounded(12)); n > 0; --n) {
            SmartAttribute a;
            a.id = int(rng.bounded(256));
            a.rawValue = rng.bounded(3) ? extremes[qsizetype(rng.bounded(quint32(extremes.size())))] : qint64(rng.generate64() >> 1);
            a.value = int(rng.bounded(256));
            a.threshold = int(rng.bounded(256));
            a.failing = a.threshold > 0 && a.value <= a.threshold;
            ata.attributes << a;
        }
        ata.driveSaysFailing = rng.bounded(10) == 0;
        ata.failingNow = int(rng.bounded(3));
        ata.temperatureC = double(rng.bounded(120)) - 20;
        ata.ssd = rng.bounded(2);
        if (rng.bounded(2))
            ata.seen.insert(199, qint64(rng.bounded(100)));
        health::NvmeInput nvme;
        nvme.availableSpare = int(rng.bounded(300)) - 100;
        nvme.spareThreshold = int(rng.bounded(300)) - 100;
        nvme.percentUsed = int(rng.bounded(300)) - 10;
        nvme.mediaErrors = rng.bounded(3) ? 0 : qint64(rng.generate64() >> 1);
        nvme.temperatureC = double(rng.bounded(150)) - 20;
        nvme.warningTempC = double(rng.bounded(100));
        if (rng.bounded(4) == 0)
            nvme.criticalWarnings << QStringList{QStringLiteral("spare"), QStringLiteral("temperature"), QStringLiteral("x")}[rng.bounded(3)];
        for (const health::Verdict &v : {health::ata(ata), health::nvme(nvme)}) {
            const HealthReason::Level worst = v.reasons.isEmpty() ? HealthReason::Level::Note : v.reasons.first().level;
            const Health::State expected = worst == HealthReason::Level::Failing ? Health::State::Failing
                : worst == HealthReason::Level::Warning                       ? Health::State::Warning
                                                                              : Health::State::Healthy;
            bool sorted = true;
            for (int k = 1; k < v.reasons.size(); ++k)
                sorted = sorted && v.reasons[k - 1].level >= v.reasons[k].level;
            if (v.state != expected || !sorted || v.summary.isEmpty())
                bad << QString::number(i);
        }
    }
    report(bad.isEmpty(), QStringLiteral("%1 random SMART readings: the verdict always matches its worst reason").arg(rounds()),
           bad.mid(0, 5).join(QLatin1Char(' ')));
}

// Text made of the characters that cause trouble (hidden, bidi, separators, emoji).
QString randomText(int length)
{
    static const QList<char32_t> pieces = {U'a', U'Z', U'0', U' ', U'-', U'.', U'/', U'\\', U'"', U'\n', U'\0', 0x202E,
                                           0x200B, 0xFEFF, 0x2028, 0x1F4BE, 0x00E4, U'{', U'<'};
    QString out;
    for (int n = 0; n < length; ++n) {
        const char32_t c = pieces[qsizetype(rng.bounded(quint32(pieces.size())))];
        out += QString::fromUcs4(&c, 1);
    }
    return out;
}

void firmwareAnswers()
{
    // Random fwupd dictionaries: never a crash, and "available" only with an update listed for
    // a drive it can update.
    const QStringList keys = {QStringLiteral("DeviceId"), QStringLiteral("Name"), QStringLiteral("Version"), QStringLiteral("Serial"),
                              QStringLiteral("Flags"), QStringLiteral("Plugin"), QStringLiteral("Enabled"), QStringLiteral("Type"),
                              QStringLiteral("ModificationTime")};
    auto randomMap = [&keys] {
        QVariantMap m;
        for (const QString &key : keys) {
            switch (rng.bounded(5)) {
            case 0: break;
            case 1: m.insert(key, randomText(int(rng.bounded(40)))); break;
            case 2: m.insert(key, quint64(rng.generate64())); break;
            case 3: m.insert(key, bool(rng.bounded(2))); break;
            default: m.insert(key, QVariant()); break;
            }
        }
        return m;
    };
    QStringList bad;
    for (int i = 0; i < rounds(); ++i) {
        QList<QVariantMap> reply, upgrades, remotes;
        for (int n = int(rng.bounded(5)); n > 0; --n)
            reply << randomMap();
        for (int n = int(rng.bounded(3)); n > 0; --n)
            upgrades << randomMap();
        for (int n = int(rng.bounded(3)); n > 0; --n)
            remotes << randomMap();
        const QVector<firmware::Device> list = firmware::devices(reply);
        const int found = firmware::match(list, randomText(int(rng.bounded(8))), randomText(int(rng.bounded(20))), randomText(int(rng.bounded(8))));
        if (found < -1 || found >= list.size())
            bad << QStringLiteral("match %1").arg(i);
        firmware::Device device = list.value(qMax(0, found));
        const firmware::Result r = firmware::outcome(device, upgrades, rng.bounded(2) ? QStringLiteral("org.freedesktop.fwupd.NothingToDo") : randomText(10),
                                                     randomText(int(rng.bounded(300))), remotes);
        if ((r.state == firmware::Result::State::Available) != (device.updatable() && !upgrades.isEmpty()) || r.error.size() > 200)
            bad << QStringLiteral("outcome %1").arg(i);
        firmware::describe(r);
    }
    report(bad.isEmpty(), QStringLiteral("%1 random fwupd answers: no crash, and an update only when there is one").arg(rounds()),
           bad.mid(0, 5).join(QLatin1Char(' ')));
}

void tableInspector(const QString &tmp)
{
    // Damaged MBRs, GPT headers and partition lists: no crash, nothing read past the buffer
    // (ASan watches), and a header only counts as checked when its size could be.
    const QString image = tmp + QStringLiteral("/inspect.img");
    {
        QFile f(image);
        if (!f.open(QIODevice::WriteOnly) || !f.resize(8 * 1024 * 1024))
            return report(false, QStringLiteral("make a table to damage"));
    }
    QProcess sfdisk;
    sfdisk.start(QStringLiteral("sfdisk"), {QStringLiteral("--quiet"), image});
    sfdisk.waitForStarted();
    sfdisk.write("label: gpt\nsize=2MiB, type=linux, name=\"one\"\ntype=linux\n");
    sfdisk.closeWriteChannel();
    sfdisk.waitForFinished();
    const QByteArray disk = readFile(image);
    if (disk.size() < 34 * 512)
        return report(false, QStringLiteral("make a table to damage"));
    const QByteArray mbr = disk.left(512), header = disk.mid(512, 512), list = disk.mid(1024, 32 * 512);
    QStringList bad;
    for (int i = 0; i < rounds(); ++i) {
        QByteArray m = mutate(mbr);
        m.resize(512, '\0');
        if (gpt::parseMbr(m.constData()).entries.size() > 4)
            bad << QStringLiteral("mbr %1").arg(i);
        QByteArray h = rng.bounded(4) ? mutate(header) : header;
        h.resize(512, '\0');
        gpt::Header parsed = gpt::parseHeader(h.constData(), 512, 1);
        if (parsed.headerCrcOk && !parsed.sizeOk)
            bad << QStringLiteral("header %1").arg(i);
        const QByteArray l = rng.bounded(4) ? mutate(list) : list;
        if (rng.bounded(3) == 0) {
            parsed.entryCount = rng.generate();
            parsed.entrySize = rng.bounded(3) ? 128 : rng.generate();
        }
        gpt::parseEntries(parsed, l.constData(), quint64(l.size()));
        if (parsed.entries.size() > qsizetype(parsed.entryCount) || (!parsed.entriesRead && !parsed.entries.isEmpty()))
            bad << QStringLiteral("entries %1").arg(i);
    }
    report(bad.isEmpty(), QStringLiteral("%1 damaged partition tables: the inspector stays inside what it read").arg(rounds()),
           bad.mid(0, 5).join(QLatin1Char(' ')));
}

void savedLayouts(const QString &tmp)
{
    // Damaged layout files: anything accepted is sane. And random layouts that pass the
    // checks write cleanly and read back with the same partitions.
    recover::Layout seed;
    seed.saved = QDateTime::currentDateTimeUtc();
    seed.table = QStringLiteral("gpt");
    seed.diskSize = 32 * 1024 * 1024;
    seed.diskId = QStringLiteral("81e11847-bbe1-4575-877b-9b568c68e928");
    recover::Part p;
    p.number = 1;
    p.start = 1024 * 1024;
    p.size = 8 * 1024 * 1024;
    p.type = QStringLiteral("0fc63daf-8483-4772-8e79-3d69d8477de4");
    p.name = QStringLiteral("root");
    seed.parts = {p};
    const QByteArray json = QJsonDocument(recover::toJson(seed)).toJson();
    QStringList bad;
    for (int i = 0; i < rounds(); ++i) {
        recover::Layout l;
        if (!recover::fromJson(QJsonDocument::fromJson(mutate(json)).object(), &l))
            continue;
        bool sane = (l.table == QLatin1String("gpt") || l.table == QLatin1String("dos")) && !l.parts.isEmpty() && l.parts.size() <= 128;
        for (const recover::Part &part : std::as_const(l.parts))
            sane = sane && part.number >= 1 && part.number <= 128 && !hasHiddenCharacters(part.name) && part.name.size() <= 36;
        if (!sane)
            bad << QStringLiteral("json %1").arg(i);
    }
    const QString image = tmp + QStringLiteral("/layouts.img");
    int written = 0;
    for (int i = 0; i < rounds() / 20; ++i) {
        recover::Layout l = seed;
        l.parts.clear();
        const int count = 1 + int(rng.bounded(5));
        for (int n = 0; n < count; ++n) {
            recover::Part part = p;
            part.number = 1 + int(rng.bounded(8));
            part.start = quint64(rng.bounded(64)) * 512 * 1024 + (rng.bounded(4) ? 0 : 512 * quint64(rng.bounded(3)));
            part.size = quint64(1 + rng.bounded(16)) * 512 * 1024;
            part.guid.clear();
            l.parts.push_back(part);
        }
        if (!recover::problem(l, seed.diskSize, 512).isEmpty())
            continue;
        QFile::remove(image);
        QFile f(image);
        if (!f.open(QIODevice::WriteOnly) || !f.resize(qint64(seed.diskSize)))
            continue;
        f.close();
        const int fd = ::open(QFile::encodeName(image).constData(), O_RDWR | O_CLOEXEC);
        const gpt::Result r = recover::write(fd, l, 512);
        const gpt::Report back = gpt::inspect(fd, 512);
        ::close(fd);
        QSet<quint64> starts;
        for (const recover::Part &part : std::as_const(l.parts))
            starts << part.start / 512;
        QSet<quint64> read;
        for (const gpt::Entry &e : back.primary.entries)
            read << e.firstLba;
        // Numbers may clash (the writer refuses that); otherwise it has to come back as written.
        if (r.ok && (!back.problems.isEmpty() || read != starts))
            bad << QStringLiteral("write %1").arg(i);
        written += r.ok ? 1 : 0;
    }
    report(bad.isEmpty() && written > 0,
           QStringLiteral("%1 damaged layout files and random layouts: nothing odd accepted, %2 written and read back").arg(rounds()).arg(written),
           bad.mid(0, 5).join(QLatin1Char(' ')));
}

void scanResults()
{
    // Random finds (any order, overlapping, past the end, sizes or not): what tidy() keeps is
    // in order, never overlaps, stays clear of both ends and is never empty.
    const QStringList types = {QStringLiteral("ext4"), QStringLiteral("vfat"), QStringLiteral("swap"), QStringLiteral("crypto_LUKS")};
    QStringList bad;
    for (int i = 0; i < rounds(); ++i) {
        const quint64 disk = (1 + rng.bounded(4096)) * quint64(1024 * 1024) + rng.bounded(512) * 512;
        QVector<partscan::Found> found;
        for (int n = int(rng.bounded(12)); n > 0; --n) {
            partscan::Found f;
            f.offset = rng.bounded(3) ? rng.bounded(quint32(disk / 512 + 100)) * quint64(512) : quint64(rng.generate64());
            f.size = rng.bounded(3) ? rng.bounded(quint32(disk / 512)) * quint64(512) : (rng.bounded(2) ? 0 : quint64(rng.generate64()));
            f.type = types[rng.bounded(quint32(types.size()))];
            found << f;
        }
        const QVector<partscan::Found> kept = partscan::tidy(found, disk, rng.bounded(2) ? 512 : 4096);
        quint64 end = 0;
        for (const partscan::Found &f : kept) {
            if (f.size == 0 || f.offset < end || f.offset < 34 * 512 || f.offset + f.size > disk || f.offset + f.size < f.offset)
                bad << QString::number(i);
            end = f.offset + f.size;
        }
        if (!partscan::toLayout(kept, QStringLiteral("gpt"), disk).parts.isEmpty()
            && !recover::problem(partscan::toLayout(kept, QStringLiteral("gpt"), disk), disk, 512).isEmpty() && kept.size() <= 128)
            bad << QStringLiteral("layout %1").arg(i);
    }
    report(bad.isEmpty(), QStringLiteral("%1 random scan results: what's kept is in order, apart, and fits the drive").arg(rounds()),
           bad.mid(0, 5).join(QLatin1Char(' ')));
}

void btrfsCounts()
{
    // Damaged error_stats text: no crash, nothing negative, and a total that can't overflow.
    const QByteArray seed = "write_errs 0\nread_errs 2\nflush_errs 0\ncorruption_errs 1\ngeneration_errs 0\n";
    QStringList bad;
    for (int i = 0; i < rounds(); ++i) {
        const btrfscheck::Counts c = btrfscheck::parse(mutate(seed));
        if (c.write < 0 || c.read < 0 || c.flush < 0 || c.corruption < 0 || c.generation < 0 || c.total() < 0 || c.total() > 5000000000000LL)
            bad << QString::number(i);
        btrfscheck::describe(c);
    }
    report(bad.isEmpty(), QStringLiteral("%1 damaged Btrfs error counts: nothing negative or overflowing").arg(rounds()), bad.mid(0, 5).join(QLatin1Char(' ')));
}

void driveNames()
{
    // Names made of the characters that cause trouble, through the backup add-on's command.
    const QList<char32_t> pieces = {U'a', U'Z', U'0', U' ', U'-', U'.', U'/', U'\\', U'"', U'\'', U'$', U'\n', U'\t', U'\0',
                                    0x202E, 0x200B, 0x2066, 0xFEFF, 0x00AD, 0x2028, 0x1F4BE, 0x00E4, U'{', U'}', U'<', U'>'};
    const QStringList command = {QStringLiteral("rsync"), QStringLiteral("{label}"), QStringLiteral("{home}/Backups/{label}/"),
                                 QStringLiteral("--name={label}"), QStringLiteral("{model}")};
    QStringList bad;
    int ran = 0;
    for (int i = 0; i < rounds(); ++i) {
        QString name;
        for (int n = 1 + int(rng.bounded(8)); n > 0; --n) {
            const char32_t c = pieces[qsizetype(rng.bounded(quint32(pieces.size())))];
            name += QString::fromUcs4(&c, 1);
        }
        // What UDisks gives DiskForge is cleaned first; check the add-on rules on raw names too.
        const QString label = rng.bounded(2) ? cleanName(name) : name;
        if (hasHiddenCharacters(cleanName(name)) || cleanName(cleanName(name)) != cleanName(name))
            bad << QStringLiteral("cleanName");
        Disk d;
        d.device = QStringLiteral("/dev/sdz");
        d.model = label;
        Volume v;
        v.device = QStringLiteral("/dev/sdz1");
        v.label = label;
        v.mountPoints = {QStringLiteral("/run/media/me/x")};
        d.volumes = {v};
        QString error;
        const QStringList argv = Addons::expand(command, d, &d.volumes[0], &error);
        if (argv.isEmpty())
            continue;
        ++ran;
        const QString prefix = QDir::homePath() + QStringLiteral("/Backups/");
        QString folder = argv[2].mid(prefix.size());
        if (folder.endsWith(QLatin1Char('/')))
            folder.chop(1);
        if (argv[1].startsWith(QLatin1Char('-')) || argv[4].startsWith(QLatin1Char('-')) || hasHiddenCharacters(argv.join(QString()))
            || !argv[2].startsWith(prefix) || folder.isEmpty() || folder.contains(QLatin1Char('/')) || folder == QLatin1String("..")
            || folder == QLatin1String("."))
            bad << name.toHtmlEscaped();
    }
    report(bad.isEmpty(), QStringLiteral("%1 nasty drive names: none turns into an option, another folder or hidden text (%2 ran)").arg(rounds()).arg(ran),
           bad.mid(0, 5).join(QLatin1Char(' ')));
}

} // namespace

// The readers added for 0.5.2's backups, evidence and Windows tools, fed damaged input: evidence
// files, seekable backups' tables, partclone headers, programs' version resources, LUKS headers.
void newReaders(const QString &tmp)
{
    // Evidence files: libewf's own, damaged, read here and there.
    const QByteArray e01 = readFile(QStringLiteral(SOURCE_DIR "/tests/data/ewf/ref.E01"));
    const QString e01Path = tmp + QStringLiteral("/fuzz.E01");
    int opened = 0;
    for (int i = 0; i < rounds() / 3; ++i) {
        writeFile(e01Path, mutate(e01));
        ewf::Info info;
        if (const auto layer = ewf::open(e01Path, &info)) {
            ++opened;
            QByteArray buf(65536, '\0');
            for (quint64 at : {quint64(0), info.size / 2, info.size > 4096 ? info.size - 4096 : 0})
                layer->read(at, buf.data(), std::min<quint64>(quint64(buf.size()), info.size - std::min(at, info.size)));
        }
    }
    report(true, QStringLiteral("%1 damaged evidence files: no crash (%2 still opened)").arg(rounds() / 3).arg(opened));

    // Seekable backups' tables.
    QByteArray backup(4096, 'z');
    backup += zstdseek::table({{1000, 8388608}, {3096, 8388608}});
    for (int i = 0; i < rounds(); ++i) {
        const QByteArray d = mutate(backup);
        zstdseek::readTable(quint64(d.size()), [&d](quint64 offset, char *buf, quint64 len) {
            if (offset + len > quint64(d.size()))
                return false;
            memcpy(buf, d.constData() + offset, len);
            return true;
        });
    }
    report(true, QStringLiteral("%1 damaged backup tables: no crash").arg(rounds()));

    // partclone's header (from the FAT32 image, unpacked).
    const QByteArray packedPtcl = readFile(QStringLiteral(SOURCE_DIR "/tests/data/clonezilla/image/sda2.vfat-ptcl-img.zst"));
    QByteArray ptcl(4096, '\0');
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    ZSTD_inBuffer in{packedPtcl.constData(), size_t(packedPtcl.size()), 0};
    ZSTD_outBuffer outBuf{ptcl.data(), size_t(ptcl.size()), 0};
    ZSTD_decompressStream(dctx, &outBuf, &in);
    ZSTD_freeDCtx(dctx);
    int valid = 0;
    for (int i = 0; i < rounds(); ++i)
        valid += partclone::parseHeader(mutate(ptcl.left(partclone::kHeaderSize))).valid() ? 1 : 0;
    report(partclone::parseHeader(ptcl.left(partclone::kHeaderSize)).valid(),
           QStringLiteral("%1 damaged partclone headers: no crash, and the CRC turns down almost all (%2 passed)").arg(rounds()).arg(valid));

    // Programs' version resources.
    const QByteArray exe = readFile(QStringLiteral(SOURCE_DIR "/tests/data/pe/versioned.exe"));
    for (int i = 0; i < rounds(); ++i)
        peinfo::versionStrings(mutate(exe));
    report(!exe.isEmpty(), QStringLiteral("%1 damaged programs' version resources: no crash").arg(rounds()));

    // LUKS2 headers: a made-up one, its JSON and all.
    QByteArray luks(16384, '\0');
    luks.replace(0, 6, QByteArray("LUKS\xba\xbe", 6));
    luks[7] = 2;
    qToBigEndian<quint64>(16384, luks.data() + 8);
    luks.replace(168, 36, "0f5c3a1e-8b2d-4c6e-9a7f-1234567890ab");
    const QByteArray json = R"({"keyslots":{},"tokens":{},"segments":{"0":{"type":"crypt","offset":"16777216","size":"dynamic"}},"digests":{},)"
                            R"("config":{"json_size":"12288","keyslots_size":"16744448"}})";
    luks.replace(4096, json.size(), json);
    for (int i = 0; i < rounds(); ++i)
        luksheader::inspect(mutate(luks));
    report(luksheader::inspect(luks).valid() && luksheader::inspect(luks).size == 16777216,
           QStringLiteral("%1 damaged LUKS headers: no crash").arg(rounds()));
}

void fuzzTests()
{
    bool given = false;
    // Unsigned: half the seeds it prints are above INT_MAX, and those have to replay too.
    const quint32 seed = qEnvironmentVariable("DISKFORGE_FUZZ_SEED").toUInt(&given);
    const quint32 used = given ? seed : QRandomGenerator::system()->generate();
    rng.seed(used);
    out << "seed " << used << " (DISKFORGE_FUZZ_SEED=" << used << " repeats this run)" << Qt::endl;

    QTemporaryDir tmp;
    QElapsedTimer timer;
    timer.start();
    addonFiles(tmp.path());
    catalogs();
    signatures(tmp.path());
    updateAnswers();
    backupDescriptions(tmp.path());
    rescueMaps(tmp.path());
    partitionTables(tmp.path());
    mountTables();
    clonePlans();
    driveNames();
    commandOutput();
    healthVerdicts();
    firmwareAnswers();
    btrfsCounts();
    tableInspector(tmp.path());
    savedLayouts(tmp.path());
    scanResults();
    newReaders(tmp.path());
    out << "took " << timer.elapsed() / 1000.0 << " s" << Qt::endl;
}
