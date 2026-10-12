// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Scan for Viruses, its second layer: what starts with Windows. The places malware puts
// itself so it starts again after every restart, read from a Windows' registry, scheduled
// tasks and startup folders, and what looks odd about each entry, in plain words.
//
// A Windows on a drive that isn't running (DiskAnvil Live, or another drive) is read from its
// files; the Windows that's running, through the registry API (the Windows version).

#include "reghive.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

namespace startup {

enum class Kind { Run, RunOnce, Load, Winlogon, Debugger, AppInit, BootExecute, Lsa, Service, Driver, Task, StartupFolder };

struct Entry {
    Kind kind = Kind::Run;
    QString where;    // in plain words: "Starts for every user", "A service"...
    QString location; // exactly where: a registry key, a task's name, a folder
    QString name;     // the value, service, task or shortcut
    QString user;     // whose it is; empty when it's for everyone
    QString command;  // as Windows has it
    QString program;  // the file it starts, as Windows names it ("C:\Windows\System32\cmd.exe")
    QString file;     // that file here (on the mounted drive); empty when it isn't there
    QStringList reasons; // why it looks odd; empty when nothing does
    int severity = 0;    // 0 nothing odd, 1 worth a look, 2 suspicious

    // Where it's kept, to switch it off: a registry file (SOFTWARE, SYSTEM or a user's
    // NTUSER.DAT, as a path here) with the key and value in it, or a file (a task, a shortcut).
    QString hiveFile;
    QString key;
    QString value;
    QString item;      // the one in a list of them (BootExecute, LSA's packages)
    QString entryFile;
    bool live = false; // the Windows that's running: changed through its registry API

    void flag(int level, const QString &reason);
};

struct Report {
    bool windows = false; // there's a Windows there at all
    QVector<Entry> entries;
    QStringList notes;    // what couldn't be read, or may be out of date
};

// Switching entries off. Values come out of the registry (Run and the like), services and
// drivers are set to Disabled, Winlogon goes back to Windows' own, tasks are switched off and
// startup-folder files moved away. Everything is copied to `backup` first, with a note in it
// saying how to put it all back.
struct Change {
    QString name;
    bool done = false;
    QString message; // what was done, or why it wasn't
};
QVector<Change> switchOff(const QVector<Entry> &entries, const QString &backup);
// Whether an entry can be switched off at all.
bool canSwitchOff(const Entry &entry);

// Is there a Windows on the drive mounted at `root`?
bool hasWindows(const QString &root);
// Its startup entries, read from its files.
Report find(const QString &root);
#ifdef Q_OS_WIN
// The Windows that's running.
Report findRunning();
bool switchOffRunning(const Entry &entry, const QString &backup, QString *message);
#endif

// find() or findRunning() on a worker thread (startOnThread).
class Finder : public QObject
{
    Q_OBJECT
public:
    // `root`: a drive with Windows on it; empty: the Windows that's running (the Windows version).
    explicit Finder(QString root);

public Q_SLOTS:
    void run();

Q_SIGNALS:
    void finished(const startup::Report &report);

private:
    QString m_root;
};

// Reading keys, from a hive file or the running Windows' registry.
class Keys
{
public:
    virtual ~Keys() = default;
    virtual bool exists(const QString &path) const = 0;
    virtual QVector<reg::Value> values(const QString &path) const = 0;
    virtual QStringList subkeys(const QString &path) const = 0;
    std::optional<reg::Value> value(const QString &path, const QString &name) const;
    QString text(const QString &path, const QString &name) const; // empty when it isn't there
};

class HiveKeys : public Keys
{
public:
    explicit HiveKeys(std::shared_ptr<reg::Hive> hive)
        : m_hive(std::move(hive))
    {
    }
    bool exists(const QString &path) const override;
    QVector<reg::Value> values(const QString &path) const override;
    QStringList subkeys(const QString &path) const override;

private:
    reg::Key find(const QString &path) const;

    std::shared_ptr<reg::Hive> m_hive;
    mutable QHash<QString, reg::Key> m_found;
};

// A Windows to read: its registry, its users, and how its paths are found here.
struct Machine {
    std::shared_ptr<Keys> software;
    std::shared_ptr<Keys> system;
    QString softwareFile, systemFile; // the files, when it's read from them
    QString controlSet = QStringLiteral("CurrentControlSet");
    struct User {
        QString name;
        QString profile; // as Windows has it: C:\Users\name
        std::shared_ptr<Keys> keys;
        QString hiveFile;
    };
    QVector<User> users;
    // A path as Windows has it ("C:\Windows\..."), as a path here; empty when it isn't there.
    std::function<QString(const QString &)> local;
    bool live = false;
    QStringList notes;
};
Report collect(const Machine &machine);

// The program a command line starts, as Windows would name it, with the environment variables
// in `env` (lower-case names) filled in. `exists` says whether a Windows path is a file.
QString programOf(const QString &command, const QHash<QString, QString> &env, const std::function<bool(const QString &)> &exists);

} // namespace startup

Q_DECLARE_METATYPE(startup::Report)
