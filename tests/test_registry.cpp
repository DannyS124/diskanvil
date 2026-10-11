// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// DiskForge's own reader of Windows' registry files: test hives Windows made (with our own keys,
// tests/data/registry/make.ps1), changes written back, logs put back, and damaged hives.
//
// DISKFORGE_REAL_HIVES=<folder>: also a real Windows' SOFTWARE, SYSTEM and NTUSER.DAT with
// their logs, copied from a running Windows (they aren't in the repo: they're Microsoft's).

#include "testkit.h"

#include "../src/viruses/reghive.h"

#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QtEndian>

#include <cstring>

namespace {

const QString kData = QStringLiteral(SOURCE_DIR "/tests/data/registry/");

QByteArray readAll(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Every key and value under `key`.
void walk(const reg::Hive &hive, reg::Key key, int *keys, int *values, int depth = 0)
{
    ++*keys;
    *values += int(hive.values(key).size());
    if (depth < 64) {
        for (reg::Key sub : hive.subkeys(key))
            walk(hive, sub, keys, values, depth + 1);
    }
}

QString valueText(const reg::Hive &hive, const QString &path, const QString &name)
{
    const std::optional<reg::Value> v = hive.value(hive.key(path), name);
    return v ? v->text() : QStringLiteral("(missing)");
}

void reading()
{
    reg::Hive hive;
    QString error;
    const bool opened = hive.open(kData + QStringLiteral("software.hiv"), &error);
    report(opened && hive.canWrite() && hive.recoveredFrom() == 0, QStringLiteral("registry: a hive reg save made"), error);
    if (!opened)
        return;
    const QString run = QStringLiteral("Microsoft\\Windows\\CurrentVersion\\Run");
    const std::optional<reg::Value> health = hive.value(hive.key(run), QStringLiteral("securityhealth"));
    report(health && health->type == reg::ExpandSz && health->text() == QLatin1String("%windir%\\system32\\SecurityHealthSystray.exe")
               && valueText(hive, run, QStringLiteral("Helper")).startsWith(QLatin1String("powershell.exe -w hidden -enc"))
               && valueText(hive, run, QStringLiteral("Gone")) == QLatin1String("\"C:\\Program Files\\Gone App\\gone.exe\" /tray")
               && hive.values(hive.key(run)).size() == 4,
           QStringLiteral("registry: a Run key's values, any case"), health ? health->text() : QString());
    report(valueText(hive, QStringLiteral("WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run"), QStringLiteral("OldApp"))
                   == QLatin1String("\"C:\\Program Files (x86)\\Old App\\old.exe\" -tray")
               && valueText(hive, QStringLiteral("Microsoft\\Windows NT\\CurrentVersion\\Winlogon"), QStringLiteral("Shell"))
                      == QLatin1String("explorer.exe, C:\\ProgramData\\svc\\host.exe")
               && valueText(hive, QStringLiteral("Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\SETHC.EXE"),
                            QStringLiteral("Debugger"))
                      == QLatin1String("C:\\Windows\\System32\\cmd.exe")
               && valueText(hive, QStringLiteral("Microsoft\\Windows NT\\CurrentVersion\\Windows"), QStringLiteral("LoadAppInit_DLLs"))
                      == QLatin1String("0"),
           QStringLiteral("registry: Winlogon, a debugger on sethc.exe, AppInit"));

    const reg::Key test = hive.key(QStringLiteral("DFTest"));
    const std::optional<reg::Value> big = hive.value(test, QStringLiteral("Big"));
    bool bigOk = big && big->type == reg::Binary && big->data.size() == 40000;
    for (int i = 0; bigOk && i < 40000; ++i)
        bigOk = uchar(big->data[i]) == i % 251;
    report(bigOk, QStringLiteral("registry: a 40,000-byte value, kept in pieces"), big ? QString::number(big->data.size()) : QString());
    const std::optional<reg::Value> multi = hive.value(test, QStringLiteral("Multi"));
    const std::optional<reg::Value> quad = hive.value(test, QStringLiteral("Quad"));
    const std::optional<reg::Value> number = hive.value(test, QStringLiteral("Number"));
    report(multi && multi->strings() == QStringList{QStringLiteral("first"), QStringLiteral("second"), QStringLiteral("third")} && quad
               && quad->number() == 81985529216486895ull && number && number->number() == 305419896,
           QStringLiteral("registry: lists of strings and numbers"));
    report(valueText(hive, QStringLiteral("DFTest"), QStringLiteral("Wert ä")) == QStringLiteral("Grüße, ключ")
               && valueText(hive, QStringLiteral("DFTest\\ünïcode КЛЮЧ"), QStringLiteral("x")) == QLatin1String("y")
               && valueText(hive, QStringLiteral("DFTest\\Default"), QString()) == QLatin1String("the default value"),
           QStringLiteral("registry: names that aren't ASCII, and a key's default value"));
    const QVector<reg::Key> many = hive.subkeys(hive.key(QStringLiteral("DFTest\\Many")));
    report(many.size() == 1500 && hive.name(many.first()) == QLatin1String("Key0000") && hive.key(QStringLiteral("DFTest\\Many\\key1499")),
           QStringLiteral("registry: 1,500 subkeys, indexed in two levels"), QString::number(many.size()));
    int keys = 0, values = 0;
    walk(hive, hive.root(), &keys, &values);
    report(hive.problems() == 0 && keys > 1500, QStringLiteral("registry: the whole hive reads without a problem"),
           QStringLiteral("%1 keys, %2 values").arg(keys).arg(values));

    reg::Hive system;
    report(system.open(kData + QStringLiteral("system.hiv"), &error)
               && valueText(system, QStringLiteral("ControlSet001\\Services\\EvilSvc"), QStringLiteral("ImagePath")) == QLatin1String("C:\\Users\\Public\\svc.exe")
               && valueText(system, QStringLiteral("ControlSet001\\Control\\Session Manager"), QStringLiteral("BootExecute"))
                      == QLatin1String("autocheck autochk *; evil.exe")
               && valueText(system, QStringLiteral("Select"), QStringLiteral("Current")) == QLatin1String("1"),
           QStringLiteral("registry: services and BootExecute"), error);
}

void writing(const QString &dir)
{
    const QString path = dir + QStringLiteral("/software.hiv");
    QFile::remove(path);
    QFile::copy(kData + QStringLiteral("software.hiv"), path);
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    reg::Hive hive;
    QString error;
    if (!hive.open(path, &error)) {
        report(false, QStringLiteral("registry: open a copy to change"), error);
        return;
    }
    const QString run = QStringLiteral("Microsoft\\Windows\\CurrentVersion\\Run");
    const QString winlogon = QStringLiteral("Microsoft\\Windows NT\\CurrentVersion\\Winlogon");
    const QString windows = QStringLiteral("Microsoft\\Windows NT\\CurrentVersion\\Windows");
    const QString runOnce = QStringLiteral("Microsoft\\Windows\\CurrentVersion\\RunOnce");
    bool ok = hive.removeValue(hive.key(run), QStringLiteral("Updater"), &error)
           && hive.setString(hive.key(winlogon), QStringLiteral("Shell"), QStringLiteral("explorer.exe"), &error)
           && hive.setDword(hive.key(windows), QStringLiteral("LoadAppInit_DLLs"), 1, &error)
           && hive.removeValue(hive.key(runOnce), QStringLiteral("Cleanup"), &error);
    const bool tooLong = !hive.setString(hive.key(winlogon), QStringLiteral("Userinit"), QString(200, QLatin1Char('x')), &error);
    ok = ok && hive.setStrings(hive.key(QStringLiteral("DFTest")), QStringLiteral("Multi"), {QStringLiteral("first"), QStringLiteral("third")}, &error);
    ok = ok && hive.save(path, &error);
    report(ok && tooLong, QStringLiteral("registry: change a copy (remove values, shorter text, a number)"), error);

    reg::Hive again;
    again.open(path, &error);
    QStringList left;
    for (const reg::Value &v : again.values(again.key(run)))
        left << v.name;
    left.sort();
    int keys = 0, values = 0;
    walk(again, again.root(), &keys, &values);
    report(left == QStringList{QStringLiteral("Gone"), QStringLiteral("Helper"), QStringLiteral("SecurityHealth")}
               && valueText(again, winlogon, QStringLiteral("Shell")) == QLatin1String("explorer.exe")
               && valueText(again, winlogon, QStringLiteral("Userinit")) == QLatin1String("C:\\Windows\\system32\\userinit.exe,")
               && valueText(again, windows, QStringLiteral("LoadAppInit_DLLs")) == QLatin1String("1")
               && again.values(again.key(runOnce)).isEmpty() && again.problems() == 0
               && valueText(again, QStringLiteral("DFTest"), QStringLiteral("Multi")) == QLatin1String("first; third"),
           QStringLiteral("registry: the changes are there when it's read again, and nothing else changed"), left.join(QStringLiteral(", ")));
    // Everything the change didn't touch is byte for byte the same.
    const QByteArray before = readAll(kData + QStringLiteral("software.hiv")), after = readAll(path);
    int differing = 0;
    for (qsizetype i = 0; i < before.size() && i < after.size(); ++i)
        differing += before[i] != after[i];
    report(before.size() == after.size() && differing > 0 && differing < 200, QStringLiteral("registry: only a few bytes were written"),
           QString::number(differing));
}

// A log entry as Windows 8.1 writes one: `pages` of the hive bins, numbered `sequence`.
QByteArray logEntry(const QByteArray &hive, const QVector<quint32> &pages, quint32 sequence)
{
    const quint32 binsSize = qFromLittleEndian<quint32>(hive.constData() + 40);
    QByteArray refs, data;
    for (quint32 page : pages) {
        char ref[8];
        qToLittleEndian<quint32>(page, ref);
        qToLittleEndian<quint32>(4096, ref + 4);
        refs.append(ref, 8);
        data.append(hive.mid(4096 + page, 4096));
    }
    QByteArray entry(40, '\0');
    entry += refs + data;
    entry.append((512 - entry.size() % 512) % 512, '\0');
    char *p = entry.data();
    std::memcpy(p, "HvLE", 4);
    qToLittleEndian<quint32>(quint32(entry.size()), p + 4);
    qToLittleEndian<quint32>(sequence, p + 12);
    qToLittleEndian<quint32>(binsSize, p + 16);
    qToLittleEndian<quint32>(quint32(pages.size()), p + 20);
    qToLittleEndian<quint64>(reg::marvin32(p + 40, entry.size() - 40), p + 24);
    qToLittleEndian<quint64>(reg::marvin32(p, 32), p + 32);
    return entry;
}

void setBase(QByteArray &bytes, quint32 seq1, quint32 seq2, quint32 type)
{
    char *p = bytes.data();
    qToLittleEndian<quint32>(seq1, p + 4);
    qToLittleEndian<quint32>(seq2, p + 8);
    qToLittleEndian<quint32>(type, p + 28);
    quint32 sum = 0;
    for (int i = 0; i < 127; ++i)
        sum ^= qFromLittleEndian<quint32>(p + 4 * i);
    qToLittleEndian<quint32>(sum == 0 ? 1 : sum == 0xFFFFFFFF ? 0xFFFFFFFE : sum, p + 508);
}

void logs()
{
    // The hive Windows last wrote, and newer changes in the log: EvilSvc's Start went from 2
    // (with Windows) to 3 (when asked), and the hive wasn't closed.
    const QByteArray clean = readAll(kData + QStringLiteral("system.hiv"));
    reg::Hive original;
    QString error;
    original.load(clean, {}, &error);
    const std::optional<reg::Value> start = original.value(original.key(QStringLiteral("ControlSet001\\Services\\EvilSvc")), QStringLiteral("Start"));
    if (!start) {
        report(false, QStringLiteral("registry: EvilSvc's Start in the test hive"), error);
        return;
    }
    QByteArray changed = clean;
    qToLittleEndian<quint32>(3, changed.data() + 4096 + start->cell + 4 + 8);
    const quint32 page = (start->cell / 4096) * 4096;

    QByteArray hive = clean;
    setBase(hive, 21, 20, 0);
    QByteArray log = clean.left(512);
    setBase(log, 30, 30, 6);
    log += logEntry(changed, {page}, 30);

    auto startNow = [](const reg::Hive &h) {
        const std::optional<reg::Value> v = h.value(h.key(QStringLiteral("ControlSet001\\Services\\EvilSvc")), QStringLiteral("Start"));
        return v ? int(v->number()) : -1;
    };
    reg::Hive recovered;
    recovered.load(hive, {QByteArray(), log}, &error);
    report(startNow(recovered) == 3 && recovered.recoveredFrom() == 1 && !recovered.canWrite() && !recovered.maybeBehind(),
           QStringLiteral("registry: a change only in the log is put back, and the hive isn't changed"),
           QString::number(startNow(recovered)));
    QString refused;
    report(!reg::Hive(recovered).setDword(recovered.key(QStringLiteral("ControlSet001\\Services\\EvilSvc")), QStringLiteral("Start"), 4, &refused)
               && refused.contains(QLatin1String("Shut down")),
           QStringLiteral("registry: a hive Windows didn't close isn't changed"), refused.left(60));

    QByteArray damaged = log;
    damaged[512 + 40 + 8 + 100] = char(damaged[512 + 40 + 8 + 100] ^ 0x55);
    reg::Hive torn;
    torn.load(hive, {damaged}, &error);
    report(startNow(torn) == 2 && torn.recoveredFrom() == 0, QStringLiteral("registry: a log entry that doesn't match its hash is left out"));

    QByteArray old = clean.left(512);
    setBase(old, 10, 10, 6);
    old += logEntry(changed, {page}, 10);
    reg::Hive stale;
    stale.load(hive, {old}, &error);
    report(startNow(stale) == 2 && stale.maybeBehind(), QStringLiteral("registry: a log older than the hive isn't used, and it says it may be behind"));

    // Two logs: the newer one wins.
    QByteArray newer = clean.left(512);
    setBase(newer, 40, 40, 6);
    QByteArray changedAgain = clean;
    qToLittleEndian<quint32>(4, changedAgain.data() + 4096 + start->cell + 4 + 8);
    newer += logEntry(changedAgain, {page}, 40);
    reg::Hive both;
    both.load(hive, {newer, log}, &error);
    report(startNow(both) == 4, QStringLiteral("registry: with two logs, the newer one's changes are the ones put back"), QString::number(startNow(both)));
}

void damaged()
{
    // Damaged hives: never a crash, never stuck. ASan runs this too.
    const QByteArray clean = readAll(kData + QStringLiteral("software.hiv"));
    const int rounds = qEnvironmentVariableIntValue("DISKFORGE_FUZZ_ROUNDS") > 0 ? qEnvironmentVariableIntValue("DISKFORGE_FUZZ_ROUNDS") : 300;
    QRandomGenerator rng(20261010);
    int opened = 0;
    for (int round = 0; round < rounds; ++round) {
        QByteArray bytes = clean;
        const int flips = 1 + int(rng.bounded(40));
        for (int i = 0; i < flips; ++i) {
            // Mostly in the hive bins, where the cells are; sometimes the base block.
            const qsizetype at = rng.bounded(10) == 0 ? qsizetype(rng.bounded(512)) : 4096 + qsizetype(rng.bounded(quint32(bytes.size() - 4096)));
            bytes[at] = char(rng.bounded(256));
        }
        if (round % 7 == 0)
            bytes.truncate(qsizetype(rng.bounded(quint32(bytes.size()))));
        reg::Hive hive;
        QString error;
        if (!hive.load(bytes, {}, &error))
            continue;
        ++opened;
        int keys = 0, values = 0;
        walk(hive, hive.root(), &keys, &values);
        for (const reg::Value &v : hive.values(hive.key(QStringLiteral("DFTest")))) {
            v.text();
            v.strings();
        }
    }
    report(opened > 0, QStringLiteral("registry: %1 damaged hives read without a crash").arg(rounds), QStringLiteral("%1 opened").arg(opened));
}

void realHives()
{
    const QString folder = qEnvironmentVariable("DISKFORGE_REAL_HIVES");
    if (folder.isEmpty()) {
        out << "SKIP  registry: no real Windows hives (DISKFORGE_REAL_HIVES)" << Qt::endl;
        return;
    }
    for (const QString &name : {QStringLiteral("SOFTWARE"), QStringLiteral("SYSTEM"), QStringLiteral("NTUSER.DAT")}) {
        reg::Hive hive;
        QString error;
        const bool opened = hive.open(folder + QLatin1Char('/') + name, &error);
        int keys = 0, values = 0;
        if (opened)
            walk(hive, hive.root(), &keys, &values);
        report(opened && hive.problems() == 0 && keys > 100, QStringLiteral("registry: a real %1").arg(name),
               QStringLiteral("%1 keys, %2 values, %3 log entries put back, %4 problems%5 %6")
                   .arg(keys)
                   .arg(values)
                   .arg(hive.recoveredFrom())
                   .arg(hive.problems())
                   .arg(hive.maybeBehind() ? QStringLiteral(", may be behind") : QString())
                   .arg(error));
        if (name == QLatin1String("SOFTWARE")) {
            QStringList run;
            for (const reg::Value &v : hive.values(hive.key(QStringLiteral("Microsoft\\Windows\\CurrentVersion\\Run"))))
                run << v.name + QLatin1Char('=') + v.text();
            out << "      Run: " << run.join(QStringLiteral(" | ")) << Qt::endl;
        }
    }
}

} // namespace

void registryTests()
{
    QTemporaryDir dir;
    reading();
    writing(dir.path());
    logs();
    damaged();
    realHives();
}
