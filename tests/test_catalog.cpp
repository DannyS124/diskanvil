// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// The add-on catalog, offline: what the list accepts and what an install checks.

#include "testkit.h"

#include "../src/addons/addons.h"

#include <QCryptographicHash>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace {

const QString kCommit = QStringLiteral("0123456789abcdef0123456789abcdef01234567");

QString pinned(const QString &path)
{
    return QStringLiteral("https://raw.githubusercontent.com/DannyS124/diskanvil-addons/%1/%2").arg(kCommit, path);
}

QString sha(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

QByteArray entry(const QString &id, const QString &url, const QString &hash)
{
    return QStringLiteral(R"({"id":"%1","name":"N","version":"1.0","url":"%2","sha256":"%3"})").arg(id, url, hash).toUtf8();
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(data);
}

} // namespace

void catalogTests()
{
    QTemporaryDir data;
    qputenv("XDG_DATA_HOME", QFile::encodeName(data.path()));
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(config.path()));

    const QByteArray manifest = R"({"id":"hello","name":"Hello","actions":[{"label":"Say hello","command":["echo","{device}"]}]})";
    const QString good = sha(manifest);
    const QByteArray list = "{\"format\":\"diskanvil-addon-catalog\",\"version\":1,\"addons\":["
        + entry(QStringLiteral("hello"), pinned(QStringLiteral("addons/hello/addon.json")), good) + ","
        + entry(QStringLiteral("other"), pinned(QStringLiteral("addons/other/addon.json")), good) + ","
        + entry(QStringLiteral("plain-http"), pinned(QStringLiteral("a.json")).replace(QStringLiteral("https"), QStringLiteral("http")), good) + ","
        + entry(QStringLiteral("elsewhere"), QStringLiteral("https://evil.example.com/%1/a.json").arg(kCommit), good) + ","
        + entry(QStringLiteral("branch"), QStringLiteral("https://raw.githubusercontent.com/DannyS124/diskanvil-addons/main/a.json"), good) + ","
        + entry(QStringLiteral("dots"), pinned(QStringLiteral("../../other-repo/a.json")), good) + ","
        + entry(QStringLiteral("short-hash"), pinned(QStringLiteral("a.json")), good.left(63)) + ","
        + entry(QStringLiteral("Bad Id"), pinned(QStringLiteral("a.json")), good) + "]}";
    QString error;
    const QVector<CatalogEntry> entries = Addons::parseCatalog(list, &error);
    report(entries.size() == 2 && entries[0].id == QLatin1String("hello"), QStringLiteral("only add-ons pinned to a commit of the catalog repo are listed"),
           QString::number(entries.size()));
    report(error.contains(QLatin1String("6")), QStringLiteral("the rest are counted as left out"), error);
    error.clear();
    report(Addons::parseCatalog(R"({"format":"something-else","addons":[]})", &error).isEmpty() && !error.isEmpty(),
           QStringLiteral("a file that isn't a catalog is refused"), error);
    error.clear();
    report(Addons::parseCatalog(QByteArray(Addons::kMaxDownload + 1, ' '), &error).isEmpty() && error.contains(QLatin1String("big")),
           QStringLiteral("an oversized list is refused"), error);

    const QString installed = data.filePath(QStringLiteral("diskanvil/addons/hello/addon.json"));
    CatalogEntry hello = entries.value(0);
    error.clear();
    CatalogEntry wrong = hello;
    wrong.sha256 = QString(64, QLatin1Char('0'));
    report(!Addons::installVerified(manifest, wrong, &error) && !QFile::exists(installed), QStringLiteral("a checksum mismatch isn't installed"), error);
    error.clear();
    CatalogEntry renamed = hello;
    renamed.id = QStringLiteral("not-hello");
    report(!Addons::installVerified(manifest, renamed, &error) && error.contains(QLatin1String("calls itself")),
           QStringLiteral("an add-on whose id doesn't match isn't installed"), error);
    error.clear();
    const QByteArray huge = QByteArray(Addons::kMaxDownload + 10, ' ');
    CatalogEntry big = hello;
    big.sha256 = sha(huge);
    report(!Addons::installVerified(huge, big, &error), QStringLiteral("an oversized download isn't installed"), error);
    error.clear();
    const QByteArray broken = R"({"id":"hello","actions":[{"label":"X","command":["rm","{nope}"]}]})";
    CatalogEntry brokenEntry = hello;
    brokenEntry.sha256 = sha(broken);
    report(!Addons::installVerified(broken, brokenEntry, &error) && error.contains(QLatin1String("placeholder")),
           QStringLiteral("a broken add-on isn't installed"), error);
    error.clear();
    report(Addons::installVerified(manifest, hello, &error) && Addons::parse(installed).error.isEmpty(),
           QStringLiteral("a matching add-on installs and parses"), error);

    // The list is only read if it's signed. A throwaway key stands in for the maintainer's.
    if (QStandardPaths::findExecutable(QStringLiteral("ssh-keygen")).isEmpty()) {
        out << "SKIP  ssh-keygen isn't installed, so signatures aren't tested" << Qt::endl;
    } else {
        QTemporaryDir keys;
        const QString key = keys.filePath(QStringLiteral("key"));
        sh(QStringLiteral("ssh-keygen"), {QStringLiteral("-q"), QStringLiteral("-t"), QStringLiteral("ed25519"), QStringLiteral("-N"), QString(),
                                          QStringLiteral("-C"), QStringLiteral("test"), QStringLiteral("-f"), key});
        const QString pub = QString::fromLatin1(readFile(key + QStringLiteral(".pub"))).trimmed();
        auto sign = [&](const QByteArray &what, const QString &ns, const QStringList &options = {}) {
            const QString file = keys.filePath(QStringLiteral("catalog.json"));
            writeFile(file, what);
            QFile::remove(file + QStringLiteral(".sig"));
            sh(QStringLiteral("ssh-keygen"), QStringList{QStringLiteral("-Y"), QStringLiteral("sign"), QStringLiteral("-f"), key, QStringLiteral("-n"), ns}
                                                 + options + QStringList{file});
            return readFile(file + QStringLiteral(".sig"));
        };
        const QByteArray sig = sign(list, QStringLiteral("diskanvil-addons"));
        error.clear();
        report(Addons::parseSignedCatalog(list, sig, {pub}, &error).size() == 2, QStringLiteral("a list signed with a known key is read"), error);
        error.clear();
        QByteArray changed = list;
        changed.replace("\"N\"", "\"M\"");
        report(Addons::parseSignedCatalog(changed, sig, {pub}, &error).isEmpty() && error.contains(QLatin1String("match")),
               QStringLiteral("a list changed after signing isn't"), error);
        error.clear();
        report(Addons::parseSignedCatalog(list, sig, Addons::catalogKeys(), &error).isEmpty() && error.contains(QLatin1String("key")),
               QStringLiteral("nor one signed with a key DiskAnvil doesn't know"), error);
        error.clear();
        report(Addons::parseSignedCatalog(list, sign(list, QStringLiteral("git")), {pub}, &error).isEmpty() && error.contains(QLatin1String("something else")),
               QStringLiteral("nor a signature made for something else (a git commit)"), error);
        error.clear();
        report(Addons::parseSignedCatalog(list, sign(list, QStringLiteral("diskanvil-addons"), {QStringLiteral("-O"), QStringLiteral("hashalg=sha256")}),
                                          {pub}, &error).size() == 2,
               QStringLiteral("sha256 signatures work too"), error);
        error.clear();
        QByteArray flipped = sig;
        flipped[flipped.size() / 2] = flipped[flipped.size() / 2] == 'A' ? 'B' : 'A';
        report(Addons::parseSignedCatalog(list, flipped, {pub}, &error).isEmpty(), QStringLiteral("a damaged signature is refused"), error);
        report(Addons::parseSignedCatalog(list, QByteArray(), {pub}, &error).isEmpty()
                   && Addons::parseSignedCatalog(list, "-----BEGIN SSH SIGNATURE-----\nAAAA\n-----END SSH SIGNATURE-----", {pub}, &error).isEmpty(),
               QStringLiteral("so are a missing or empty one"));
    }

    // The real catalog, if the add-ons repository sits next to this one: every entry has to
    // go through the same checks, using the file exactly as it is in the pinned commit.
    const QString repo = QStringLiteral(SOURCE_DIR "/../diskanvil-addons");
    QFile real(repo + QStringLiteral("/catalog.json"));
    if (!real.open(QIODevice::ReadOnly)) {
        out << "SKIP  the diskanvil-addons repository isn't next to this one" << Qt::endl;
        return;
    }
    error.clear();
    const QByteArray realData = real.readAll();
    const QVector<CatalogEntry> listed = Addons::parseSignedCatalog(realData, readFile(repo + QStringLiteral("/catalog.json.sig")), Addons::catalogKeys(), &error);
    report(!listed.isEmpty() && error.isEmpty(), QStringLiteral("the real catalog is signed with DiskAnvil's key and accepted whole"),
           QStringLiteral("%1 add-on(s) %2").arg(listed.size()).arg(error));
    for (const CatalogEntry &e : listed) {
        const QString commitAndPath = e.url.section(QLatin1Char('/'), 5, 5) + QLatin1Char(':') + e.url.section(QLatin1Char('/'), 6);
        QProcess git;
        git.start(QStringLiteral("git"), {QStringLiteral("-C"), repo, QStringLiteral("show"), commitAndPath});
        git.waitForFinished();
        error.clear();
        report(Addons::installVerified(git.readAllStandardOutput(), e, &error), QStringLiteral("catalog entry %1 installs").arg(e.id), error);
    }
}
