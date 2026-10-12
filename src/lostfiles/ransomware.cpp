// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ransomware.h"

#include <QCoreApplication>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace ransomware {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("ransomware", text);
}

struct Family {
    const char *name;
    QStringList endings;      // added to locked files, without the dot, lower case
    QStringList notes;        // its notes' names, as patterns (case doesn't matter)
    const char *decryptor;    // made public, where and for what; nullptr when none is known
};

// Well-known families: the endings they add and the notes they leave are what their victims and
// security companies have published about them. A decryptor is named only where one was made
// public and is listed by No More Ransom.
const QVector<Family> &families()
{
    static const QVector<Family> list = {
        {"STOP/Djvu", {"djvu", "djvuu", "uudjvu", "djvus", "djvur", "tro", "rumba", "gero", "hese", "nppp", "kiop", "maas", "nbes", "wrui"},
         {QStringLiteral("^_readme\\.txt$")},
         QT_TRANSLATE_NOOP("ransomware", "Emsisoft's, for files locked with its \"offline key\" (mostly in its early years)")},
        {"WannaCry", {"wncry", "wncryt", "wcry"}, {QStringLiteral("^@please_read_me@\\.txt$"), QStringLiteral("^@wanadecryptor@")}, nullptr},
        {"Locky", {"locky", "zepto", "odin", "thor", "aesir", "osiris", "lukitus", "diablo6", "ykcol", "asasin"},
         {QStringLiteral("_locky_recover_instructions"), QStringLiteral("^_?help_instructions\\.html$"), QStringLiteral("-instruction\\.(html|bmp)$")}, nullptr},
        {"Cerber", {"cerber", "cerber2", "cerber3"}, {QStringLiteral("decrypt my files"), QStringLiteral("^_help_help_help_")}, nullptr},
        {"TeslaCrypt", {"ecc", "ezz", "exx", "xyz", "zzz", "aaa", "abc", "ccc", "vvv", "micro", "ttt"},
         {QStringLiteral("^help_restore_files"), QStringLiteral("^_recovery_\\+"), QStringLiteral("^\\+?recove(r|ry)\\+")},
         QT_TRANSLATE_NOOP("ransomware", "ESET's and others', since its makers gave its master key away in 2016")},
        {"Jigsaw", {"fun", "kkk", "gws", "btc"}, {}, QT_TRANSLATE_NOOP("ransomware", "Emsisoft's and Check Point's")},
        {"GandCrab", {"gdcb", "krab", "crab"}, {QStringLiteral("^(gdcb|krab|crab)-decrypt\\.txt$"), QStringLiteral("^[a-z]{5,10}-decrypt\\.(txt|html)$")},
         QT_TRANSLATE_NOOP("ransomware", "Bitdefender's, for its versions 1, 4 and 5 (to 5.2)")},
        {"Shade (Troldesh)", {"xtbl", "ytbl", "breaking_bad", "heisenberg", "better_call_saul", "da_vinci_code", "no_more_ransom", "crypted000007"},
         {QStringLiteral("^readme\\d+\\.txt$")}, QT_TRANSLATE_NOOP("ransomware", "Kaspersky's and ESET's, since its keys were released in 2019")},
        {"Dharma (CrySiS)", {"dharma", "wallet", "arena", "java", "bip", "combo", "gamma", "adobe", "cezar", "brrr", "bkp", "write"},
         {QStringLiteral("^files encrypted\\.txt$"), QStringLiteral("^info\\.hta$")},
         QT_TRANSLATE_NOOP("ransomware", "ESET's and Kaspersky's, for some of its older versions")},
        {"Phobos", {"phobos", "eking", "eight", "elbie", "faust", "acuna", "devos", "elking", "8base"},
         {QStringLiteral("^info\\.(hta|txt)$")}, QT_TRANSLATE_NOOP("ransomware", "Japan's National Police Agency's (2025), for Phobos and 8Base")},
        {"LockBit", {"lockbit"}, {QStringLiteral("^restore-my-files\\.txt$"), QStringLiteral("^[a-z0-9]{9}\\.readme\\.txt$")},
         QT_TRANSLATE_NOOP("ransomware", "the police's (Operation Cronos, 2024), for some LockBit 3.0 victims")},
        {"Ryuk", {"ryk"}, {QStringLiteral("^ryukreadme\\.(txt|html)$")}, nullptr},
        {"REvil (Sodinokibi)", {}, {QStringLiteral("^[a-z0-9]{5,10}-readme\\.txt$")},
         QT_TRANSLATE_NOOP("ransomware", "Bitdefender's, for files locked before 13 July 2021")},
        {"Maze", {}, {QStringLiteral("^decrypt-files\\.(txt|html)$")}, QT_TRANSLATE_NOOP("ransomware", "Emsisoft's, since its master keys were released in 2022")},
        {"Akira", {"akira"}, {QStringLiteral("^akira_readme\\.txt$")}, QT_TRANSLATE_NOOP("ransomware", "Avast's, for its versions from early 2023")},
        {"Conti", {"conti"}, {QStringLiteral("^conti_readme\\.txt$")}, nullptr},
        {"MedusaLocker", {"medusa"}, {QStringLiteral("^how_to_recover_data\\.html$")}, nullptr},
        {"Makop", {"makop"}, {QStringLiteral("^readme-warning\\.txt$")}, nullptr},
        {"BlackCat (ALPHV)", {}, {QStringLiteral("^recover-[a-z0-9]{7}-files\\.txt$")}, nullptr},
    };
    return list;
}

// Each family's note patterns, made once.
const QVector<QPair<QRegularExpression, QString>> &notePatterns()
{
    static const QVector<QPair<QRegularExpression, QString>> list = [] {
        QVector<QPair<QRegularExpression, QString>> out;
        for (const Family &family : families()) {
            for (const QString &pattern : family.notes)
                out.push_back({QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption), QString::fromLatin1(family.name)});
        }
        return out;
    }();
    return list;
}

// Notes that don't say which family they're from.
const QRegularExpression &anyNote()
{
    static const QRegularExpression note(QStringLiteral("(how[_ -]?to[_ -]?(decrypt|recover|restore|unlock)|help[_ -]?decrypt|decrypt[_ -]?instructions|"
                                                        "read[_ -]?me[_ -]?(for|to)[_ -]?decrypt|recover[_ -]?files|your[_ -]?files[_ -]?(are|were)[_ -]?encrypted|"
                                                        "!+read[_ -]?me!+)"),
                                         QRegularExpression::CaseInsensitiveOption);
    return note;
}

// The kinds of files ransomware locks, as the ending before the one it adds.
bool lockable(const QString &ending)
{
    static const QSet<QString> kinds = {QStringLiteral("jpg"),  QStringLiteral("jpeg"), QStringLiteral("png"),  QStringLiteral("gif"),
                                        QStringLiteral("doc"),  QStringLiteral("docx"), QStringLiteral("xls"),  QStringLiteral("xlsx"),
                                        QStringLiteral("ppt"),  QStringLiteral("pptx"), QStringLiteral("pdf"),  QStringLiteral("txt"),
                                        QStringLiteral("mp4"),  QStringLiteral("mov"),  QStringLiteral("mp3"),  QStringLiteral("zip"),
                                        QStringLiteral("odt"),  QStringLiteral("ods"),  QStringLiteral("psd"),  QStringLiteral("csv"),
                                        QStringLiteral("rtf"),  QStringLiteral("heic"), QStringLiteral("nef"),  QStringLiteral("cr2"),
                                        QStringLiteral("avi"),  QStringLiteral("wav"),  QStringLiteral("7z"),   QStringLiteral("rar"),
                                        QStringLiteral("sql"),  QStringLiteral("db"),   QStringLiteral("pst"),  QStringLiteral("mdb")};
    return kinds.contains(ending);
}

} // namespace

Finding look(const QVector<lost::Found> &files)
{
    Finding f;
    // Endings added after a file's own: "report.docx.locky" adds "locky". Dharma and Phobos put
    // an id and an address in brackets before theirs.
    static const QRegularExpression added(QStringLiteral("\\.([a-z0-9]{2,6})(\\.(id|\\[)[^/]*)?\\.([a-z0-9_]{2,20})$"), QRegularExpression::CaseInsensitiveOption);
    QHash<QString, int> endings;
    QHash<QString, QStringList> examples;
    QSet<QString> deletedNames;
    for (const lost::Found &file : files) {
        if (file.deleted)
            deletedNames.insert((file.folder + QLatin1Char('/') + file.name).toLower());
    }
    QHash<QString, int> originals;
    QHash<QString, int> noteFamilies;
    for (const lost::Found &file : files) {
        const QString name = file.name.toLower();
        const QRegularExpressionMatch m = added.match(name);
        static const QSet<QString> harmless = {QStringLiteral("bak"),    QStringLiteral("old"),     QStringLiteral("orig"),      QStringLiteral("tmp"),
                                               QStringLiteral("temp"),   QStringLiteral("backup"),  QStringLiteral("part"),      QStringLiteral("partial"),
                                               QStringLiteral("crdownload"), QStringLiteral("download"), QStringLiteral("sav"), QStringLiteral("lnk"),
                                               QStringLiteral("swp"),    QStringLiteral("copy"),    QStringLiteral("md5"),       QStringLiteral("sha256"),
                                               QStringLiteral("sig"),    QStringLiteral("asc"),     QStringLiteral("torrent"),   QStringLiteral("xmp"),
                                               QStringLiteral("aae"),    QStringLiteral("json"),    QStringLiteral("txt"),       QStringLiteral("gz"),
                                               QStringLiteral("bz2"),    QStringLiteral("xz"),      QStringLiteral("zst"),       QStringLiteral("filepart")};
        if (m.hasMatch() && lockable(m.captured(1)) && !lockable(m.captured(4)) && !harmless.contains(m.captured(4))) {
            const QString ending = m.captured(4);
            ++endings[ending];
            if (examples[ending].size() < 5)
                examples[ending] << (file.folder.isEmpty() ? file.name : file.folder + QLatin1Char('/') + file.name);
            // The original, deleted, next to it: the name up to its own ending.
            const QString original = (file.folder + QLatin1Char('/') + name.left(m.capturedStart(2) >= 0 ? m.capturedStart(2) : m.capturedStart(4) - 1)).toLower();
            if (deletedNames.contains(original))
                ++originals[ending];
        }
        // Notes: a family's own, or any that reads like one.
        bool note = false;
        for (const auto &[pattern, family] : notePatterns()) {
            if (pattern.match(file.name).hasMatch()) {
                ++noteFamilies[family];
                note = true;
            }
        }
        const bool looksLikeNote = file.name.endsWith(QLatin1String(".txt"), Qt::CaseInsensitive) || file.name.endsWith(QLatin1String(".html"), Qt::CaseInsensitive)
                                || file.name.endsWith(QLatin1String(".hta"), Qt::CaseInsensitive);
        if ((note || (looksLikeNote && anyNote().match(file.name).hasMatch())) && f.notes.size() < 50)
            f.notes << (file.folder.isEmpty() ? file.name : file.folder + QLatin1Char('/') + file.name);
    }
    // The ending added most, when it's on enough files to be no accident.
    for (auto it = endings.cbegin(); it != endings.cend(); ++it) {
        if (it.value() > f.locked) {
            f.locked = it.value();
            f.ending = QLatin1Char('.') + it.key();
        }
    }
    const bool known = [&] {
        for (const Family &family : families()) {
            if (family.endings.contains(f.ending.mid(1)))
                return true;
        }
        return false;
    }();
    if (f.locked < (known ? 3 : 20) && f.notes.isEmpty()) {
        f.locked = 0;
        f.ending.clear();
        return f;
    }
    if (f.locked < (known ? 3 : 20)) { // notes, but not enough locked files to say which ending
        f.locked = 0;
        f.ending.clear();
    }
    f.examples = examples.value(f.ending.mid(1));
    f.deletedOriginals = originals.value(f.ending.mid(1));
    // The family: by its ending, else by its notes. STOP/Djvu adds four letters of its own each
    // time, with its "_readme.txt".
    for (const Family &family : families()) {
        if (!f.ending.isEmpty() && family.endings.contains(f.ending.mid(1))) {
            f.family = QString::fromLatin1(family.name);
            f.decryptor = family.decryptor ? tr(family.decryptor) : QString();
        }
    }
    if (f.family.isEmpty()) {
        int best = 0;
        for (auto it = noteFamilies.cbegin(); it != noteFamilies.cend(); ++it) {
            if (it.value() > best) {
                best = it.value();
                f.family = it.key();
            }
        }
        for (const Family &family : families()) {
            if (f.family == QLatin1String(family.name))
                f.decryptor = family.decryptor ? tr(family.decryptor) : QString();
        }
    }
    return f;
}

} // namespace ransomware
