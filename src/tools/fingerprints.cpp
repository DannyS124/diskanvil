// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fingerprints.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

namespace fingerprints {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("fingerprints", text);
}

const QByteArray kMadeLine = "# DiskAnvil fingerprints, made ";

} // namespace

QString ownCopyFor(const QString &folder)
{
    const QString clean = QDir::cleanPath(QFileInfo(folder).absoluteFilePath());
    const QString id = QString::fromLatin1(QCryptographicHash::hash(clean.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/fingerprints/") + id + QStringLiteral(".sha256");
}

QString bookFor(const QString &folder, bool *exists)
{
    const QString inside = QDir(folder).filePath(QString::fromLatin1(kFileName));
    const QString own = ownCopyFor(folder);
    if (QFileInfo::exists(inside)) {
        *exists = true;
        return inside;
    }
    if (QFileInfo::exists(own)) {
        *exists = true;
        return own;
    }
    *exists = false;
    return QFileInfo(folder).isWritable() ? inside : own;
}

Book fromText(const QByteArray &text)
{
    Book book;
    for (QByteArray line : text.split('\n')) {
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.startsWith(kMadeLine)) {
            book.made = QDateTime::fromString(QString::fromUtf8(line.mid(kMadeLine.size())).trimmed(), Qt::ISODate);
            continue;
        }
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        // "<64 hex>  <name>" (or " *<name>": binary mode). A name with a newline or backslash
        // starts the line with "\" and escapes them, the way sha256sum does.
        bool escaped = line.startsWith('\\');
        if (escaped)
            line.remove(0, 1);
        if (line.size() < 66 || line[64] != ' ' || (line[65] != ' ' && line[65] != '*'))
            continue;
        Entry e;
        e.sha = line.left(64).toLower();
        QByteArray name = line.mid(66);
        if (escaped) {
            QByteArray plain;
            for (qsizetype i = 0; i < name.size(); ++i) {
                if (name[i] == '\\' && i + 1 < name.size()) {
                    plain += name[i + 1] == 'n' ? '\n' : name[i + 1];
                    ++i;
                } else {
                    plain += name[i];
                }
            }
            name = plain;
        }
        e.path = QString::fromUtf8(name);
        book.files << e;
    }
    return book;
}

QByteArray toText(const Book &book)
{
    QByteArray out = kMadeLine + book.made.toUTC().toString(Qt::ISODate).toUtf8() + '\n';
    out += "# Check it again with DiskAnvil's Are My Files Still OK?, or here with: sha256sum -c \"" + QByteArray(kFileName) + "\"\n";
    for (const Entry &e : book.files) {
        QByteArray name = e.path.toUtf8();
        if (name.contains('\n') || name.contains('\\'))
            out += '\\' + e.sha + "  " + name.replace("\\", "\\\\").replace("\n", "\\n") + '\n';
        else
            out += e.sha + "  " + name + '\n';
    }
    return out;
}

bool read(const QString &file, Book *book, QString *error)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = f.errorString();
        return false;
    }
    *book = fromText(f.readAll());
    if (!book->made.isValid())
        book->made = QFileInfo(file).lastModified();
    return true;
}

QString stateName(State state)
{
    switch (state) {
    case State::Same:
        return tr("The same");
    case State::Damaged:
        return tr("Damaged");
    case State::Edited:
        return tr("Changed (saved since)");
    case State::Missing:
        return tr("Missing");
    case State::Unreadable:
        return tr("Can't be read");
    case State::New:
        return tr("New since");
    }
    return {};
}

QVector<Result> compare(const Book &then, const QVector<Seen> &now)
{
    QHash<QString, const Seen *> seen;
    for (const Seen &s : now)
        seen.insert(s.path, &s);
    QVector<Result> out;
    QSet<QString> known;
    for (const Entry &e : then.files) {
        known.insert(e.path);
        Result r;
        r.path = e.path;
        const Seen *s = seen.value(e.path);
        if (!s) {
            r.state = State::Missing;
        } else {
            r.modified = s->modified;
            if (s->sha.isEmpty()) {
                r.state = State::Unreadable;
                r.detail = s->error;
            } else if (s->sha == e.sha) {
                r.state = State::Same;
            } else {
                // Changed: saved by someone since (a newer date), or not (damaged). A minute's
                // leeway: file systems keep dates to 2 s (FAT) and clocks wander.
                r.state = then.made.isValid() && s->modified.isValid() && s->modified > then.made.addSecs(-60) ? State::Edited : State::Damaged;
            }
        }
        out << r;
    }
    for (const Seen &s : now) {
        if (!known.contains(s.path)) {
            Result r;
            r.path = s.path;
            r.state = State::New;
            r.modified = s.modified;
            out << r;
        }
    }
    return out;
}

Job::Job(QString folder, Mode mode)
    : m_folder(std::move(folder))
    , m_mode(mode)
{
}

void Job::run()
{
    bool exists = false;
    const QString book = bookFor(m_folder, &exists);
    Book then;
    QString error;
    if (m_mode == Mode::Check) {
        if (!exists || !read(book, &then, &error)) {
            Q_EMIT finished(false, {}, book, exists ? error : tr("There are no fingerprints for this folder yet."));
            return;
        }
    }
    // What's there, and how big.
    QStringList files;
    quint64 total = 0;
    const QDir top(m_folder);
    QDirIterator it(m_folder, QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext() && !m_cancel) {
        const QString path = it.next();
        const QString relative = top.relativeFilePath(path);
        if (relative == QLatin1String(kFileName))
            continue;
        files << relative;
        total += quint64(it.fileInfo().size());
    }
    files.sort();
    QVector<Seen> now;
    quint64 done = 0;
    QByteArray buffer(1 << 20, Qt::Uninitialized);
    for (const QString &relative : std::as_const(files)) {
        if (m_cancel)
            break;
        Seen s;
        s.path = relative;
        const QString path = top.filePath(relative);
        s.modified = QFileInfo(path).lastModified();
        QFile f(path);
        Q_EMIT progress(done, total, relative);
        if (!f.open(QIODevice::ReadOnly)) {
            s.error = f.errorString();
        } else {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            bool ok = true;
            for (;;) {
                const qint64 n = f.read(buffer.data(), buffer.size());
                if (n < 0) {
                    ok = false;
                    s.error = f.errorString();
                    break;
                }
                if (n == 0)
                    break;
                hash.addData(QByteArrayView(buffer.constData(), n));
                done += quint64(n);
                if (m_cancel)
                    break;
            }
            if (ok)
                s.sha = hash.result().toHex();
        }
        now << s;
    }
    if (m_cancel) {
        Q_EMIT finished(false, {}, book, tr("Stopped."));
        return;
    }
    Q_EMIT progress(total, total, QString());
    if (m_mode == Mode::Check) {
        Q_EMIT finished(true, compare(then, now), book, QString());
        return;
    }
    Book made;
    made.made = QDateTime::currentDateTimeUtc();
    QStringList unreadable;
    for (const Seen &s : std::as_const(now)) {
        if (s.sha.isEmpty())
            unreadable << s.path;
        else
            made.files << Entry{s.path, s.sha};
    }
    QString where = book;
    auto save = [&made](const QString &file, QString *why) {
        QDir().mkpath(QFileInfo(file).absolutePath());
        QSaveFile out(file);
        if (!out.open(QIODevice::WriteOnly) || out.write(toText(made)) < 0 || !out.commit()) {
            *why = out.errorString();
            return false;
        }
        return true;
    };
    if (!save(where, &error)) {
        // The folder can't be written to after all: DiskAnvil's own folder.
        where = ownCopyFor(m_folder);
        if (!save(where, &error)) {
            Q_EMIT finished(false, {}, where, error);
            return;
        }
    } else if (where != ownCopyFor(m_folder)) {
        QFile::remove(ownCopyFor(m_folder)); // an older one there would be found first otherwise
    }
    QVector<Result> results;
    for (const QString &u : std::as_const(unreadable))
        results << Result{u, State::Unreadable, {}, {}};
    Q_EMIT finished(true, results, where, QString());
}

} // namespace fingerprints
