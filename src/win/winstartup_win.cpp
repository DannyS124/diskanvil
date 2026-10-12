// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// What starts with the Windows that's running: the same places as for a Windows on another
// drive, read through the registry API. A user who isn't signed in has no registry loaded;
// their NTUSER.DAT isn't in use then, so it's read from the file.

#include "winstartup.h"

#include "winutil.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

#include <iterator>

namespace startup {

namespace {

class RegKey
{
public:
    RegKey(HKEY root, const QString &path)
    {
        if (RegOpenKeyExW(root, win::wide(path).c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &m_key) != ERROR_SUCCESS)
            m_key = nullptr;
    }
    ~RegKey()
    {
        if (m_key)
            RegCloseKey(m_key);
    }
    RegKey(const RegKey &) = delete;
    RegKey &operator=(const RegKey &) = delete;
    HKEY get() const { return m_key; }

private:
    HKEY m_key = nullptr;
};

class LiveKeys : public Keys
{
public:
    LiveKeys(HKEY root, QString prefix)
        : m_root(root)
        , m_prefix(std::move(prefix))
    {
    }

    bool exists(const QString &path) const override { return RegKey(m_root, full(path)).get() != nullptr; }

    QVector<reg::Value> values(const QString &path) const override
    {
        QVector<reg::Value> out;
        RegKey key(m_root, full(path));
        if (!key.get())
            return out;
        DWORD count = 0, longestName = 0, largestData = 0;
        if (RegQueryInfoKeyW(key.get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &count, &longestName, &largestData, nullptr, nullptr)
            != ERROR_SUCCESS)
            return out;
        std::wstring name(longestName + 1, L'\0');
        QByteArray data(qsizetype(largestData) + 2, '\0');
        for (DWORD i = 0; i < count; ++i) {
            DWORD nameLength = DWORD(name.size()), type = 0, dataSize = DWORD(data.size());
            if (RegEnumValueW(key.get(), i, name.data(), &nameLength, nullptr, &type, reinterpret_cast<BYTE *>(data.data()), &dataSize)
                != ERROR_SUCCESS)
                continue;
            reg::Value v;
            v.name = win::fromWide(name.c_str(), int(nameLength));
            v.type = type;
            v.data = data.left(dataSize); // the same bytes a hive file keeps
            out.push_back(std::move(v));
        }
        return out;
    }

    QStringList subkeys(const QString &path) const override
    {
        QStringList out;
        RegKey key(m_root, full(path));
        if (!key.get())
            return out;
        wchar_t name[256];
        for (DWORD i = 0;; ++i) {
            DWORD length = 256;
            const LONG result = RegEnumKeyExW(key.get(), i, name, &length, nullptr, nullptr, nullptr, nullptr);
            if (result == ERROR_NO_MORE_ITEMS)
                break;
            if (result == ERROR_SUCCESS)
                out << win::fromWide(name, int(length));
        }
        return out;
    }

private:
    QString full(const QString &path) const { return path.isEmpty() ? m_prefix : m_prefix + QLatin1Char('\\') + path; }

    HKEY m_root;
    QString m_prefix;
};

QString expandHere(const QString &text)
{
    wchar_t out[MAX_PATH * 4];
    const DWORD n = ExpandEnvironmentStringsW(win::wide(text).c_str(), out, DWORD(std::size(out)));
    return n > 0 && n <= std::size(out) ? win::fromWide(out) : text;
}

} // namespace

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("startup", text);
}

bool setDword(HKEY key, const wchar_t *name, DWORD value)
{
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof value) == ERROR_SUCCESS;
}

bool setText(HKEY key, const QString &name, const QString &text, DWORD type)
{
    const std::wstring data = win::wide(text);
    const DWORD bytes = DWORD((data.size() + 1) * sizeof(wchar_t)); // with the NUL, or the second NUL of a list
    return RegSetValueExW(key, win::wide(name).c_str(), 0, type, reinterpret_cast<const BYTE *>(data.c_str()), bytes) == ERROR_SUCCESS;
}

int backups = 0;

} // namespace

bool switchOffRunning(const Entry &e, const QString &backup, QString *message)
{
    if (e.kind == Kind::Task) {
        const QString name = e.location.section(QStringLiteral(": "), 1);
        QFile::copy(e.entryFile, backup + QLatin1Char('/') + name.section(QLatin1Char('\\'), -1) + QStringLiteral(".xml"));
        QProcess schtasks;
        schtasks.start(QStringLiteral("schtasks.exe"), {QStringLiteral("/Change"), QStringLiteral("/TN"), name, QStringLiteral("/Disable")});
        if (!schtasks.waitForFinished(30000) || schtasks.exitCode() != 0) {
            *message = tr("Task Scheduler wouldn't switch it off: %1").arg(QString::fromLocal8Bit(schtasks.readAllStandardError()).trimmed());
            return false;
        }
        *message = tr("switched off in Task Scheduler (schtasks /Change /TN \"%1\" /Enable puts it back)").arg(name);
        return true;
    }
    if (e.kind == Kind::StartupFolder) {
        const QString into = backup + QStringLiteral("/Startup folder") + (e.user.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(e.user));
        QDir().mkpath(into);
        if (!QFile::rename(e.entryFile, into + QLatin1Char('/') + e.name)) {
            *message = tr("couldn't be moved away");
            return false;
        }
        *message = tr("moved to the backup");
        return true;
    }
    // "registry:HKEY_LOCAL_MACHINE\SOFTWARE" and the key under it.
    const QString full = e.hiveFile.mid(9) + QLatin1Char('\\') + e.key;
    const QString file = backup + QStringLiteral("/%1 %2.reg").arg(++backups, 2, 10, QLatin1Char('0')).arg(e.name.left(40).replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_")));
    if (QProcess::execute(QStringLiteral("reg.exe"), {QStringLiteral("export"), full, QDir::toNativeSeparators(file), QStringLiteral("/y")}) != 0) {
        *message = tr("a backup of its settings couldn't be made, so it isn't changed");
        return false;
    }
    const HKEY root = full.startsWith(QLatin1String("HKEY_USERS\\")) ? HKEY_USERS : HKEY_LOCAL_MACHINE;
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, win::wide(full.section(QLatin1Char('\\'), 1)).c_str(), 0, KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &key)
        != ERROR_SUCCESS) {
        *message = tr("its registry key couldn't be opened: %1").arg(win::lastErrorText());
        return false;
    }
    bool ok = false;
    switch (e.kind) {
    case Kind::Service:
    case Kind::Driver:
        ok = setDword(key, L"Start", 4);
        *message = tr("set to Disabled");
        break;
    case Kind::AppInit:
        ok = setDword(key, L"LoadAppInit_DLLs", 0);
        *message = tr("switched off (LoadAppInit_DLLs set to 0)");
        break;
    case Kind::Winlogon:
        if (e.user.isEmpty()) {
            wchar_t root[MAX_PATH];
            const UINT n = GetWindowsDirectoryW(root, MAX_PATH);
            const QString usual = e.value == QLatin1String("Shell") ? QStringLiteral("explorer.exe")
                                                                     : win::fromWide(root, int(n)) + QStringLiteral("\\system32\\userinit.exe,");
            ok = setText(key, e.value, usual, REG_SZ);
            *message = tr("set back to Windows' own: %1").arg(usual);
            break;
        }
        [[fallthrough]];
    default:
        if (e.kind == Kind::BootExecute || e.kind == Kind::Lsa) {
            LiveKeys keys(root, full.section(QLatin1Char('\\'), 1, 1));
            QStringList items = keys.value(full.section(QLatin1Char('\\'), 2), e.value).value_or(reg::Value()).strings();
            items.removeIf([&e](const QString &item) { return item.trimmed().compare(e.item.trimmed(), Qt::CaseInsensitive) == 0; });
            QString all;
            for (const QString &item : std::as_const(items))
                all += item + QChar(0);
            ok = setText(key, e.value, all, REG_MULTI_SZ);
            *message = tr("taken out of the list");
        } else {
            ok = RegDeleteValueW(key, win::wide(e.value).c_str()) == ERROR_SUCCESS;
            *message = tr("taken out of the registry");
        }
        break;
    }
    if (!ok)
        *message = tr("Windows wouldn't change it: %1").arg(win::lastErrorText());
    RegCloseKey(key);
    return ok;
}

Machine openRunningMachine()
{
    Machine m;
    m.software = std::make_shared<LiveKeys>(HKEY_LOCAL_MACHINE, QStringLiteral("SOFTWARE"));
    m.system = std::make_shared<LiveKeys>(HKEY_LOCAL_MACHINE, QStringLiteral("SYSTEM"));
    m.softwareFile = QStringLiteral("registry:HKEY_LOCAL_MACHINE\\SOFTWARE");
    m.systemFile = QStringLiteral("registry:HKEY_LOCAL_MACHINE\\SYSTEM");
    m.controlSet = QStringLiteral("CurrentControlSet");
    m.live = true;
    m.local = [](const QString &path) {
        const QString here = QDir::fromNativeSeparators(path);
        return QFileInfo::exists(here) ? here : QString();
    };
    const QString profiles = QStringLiteral("Microsoft\\Windows NT\\CurrentVersion\\ProfileList");
    for (const QString &sid : m.software->subkeys(profiles)) {
        if (!sid.startsWith(QLatin1String("S-1-5-21-")))
            continue;
        const QString profile = expandHere(m.software->text(profiles + QLatin1Char('\\') + sid, QStringLiteral("ProfileImagePath")));
        if (profile.isEmpty())
            continue;
        Machine::User user;
        user.name = profile.section(QLatin1Char('\\'), -1);
        user.profile = profile;
        if (RegKey(HKEY_USERS, sid).get()) {
            user.keys = std::make_shared<LiveKeys>(HKEY_USERS, sid);
            user.hiveFile = QStringLiteral("registry:HKEY_USERS\\") + sid;
        } else {
            user.hiveFile = QDir::fromNativeSeparators(profile + QStringLiteral("\\NTUSER.DAT"));
            auto hive = std::make_shared<reg::Hive>();
            QString error;
            if (hive->open(user.hiveFile, &error))
                user.keys = std::make_shared<HiveKeys>(hive);
            else
                m.notes << QCoreApplication::translate("startup", "%1's settings couldn't be read: %2").arg(user.name, error);
        }
        m.users << user;
    }
    return m;
}

Report findRunning()
{
    return collect(openRunningMachine());
}

} // namespace startup
