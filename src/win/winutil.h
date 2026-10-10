// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Small helpers for the Windows side: handles that close themselves, wide strings, error
// text and DeviceIoControl with answers of any size.

#include <QByteArray>
#include <QHash>
#include <QString>

#include <functional>
#include <string>

#include <windows.h>

class QObject;

namespace win {

class Handle
{
public:
    Handle() = default;
    explicit Handle(HANDLE h) : m_h(h) {}
    ~Handle() { reset(); }
    Handle(Handle &&other) noexcept : m_h(other.release()) {}
    Handle &operator=(Handle &&other) noexcept
    {
        if (this != &other) {
            reset();
            m_h = other.release();
        }
        return *this;
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;

    HANDLE get() const { return m_h; }
    bool valid() const { return m_h != INVALID_HANDLE_VALUE && m_h != nullptr; }
    HANDLE release()
    {
        HANDLE h = m_h;
        m_h = INVALID_HANDLE_VALUE;
        return h;
    }
    void reset()
    {
        if (valid())
            CloseHandle(m_h);
        m_h = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE m_h = INVALID_HANDLE_VALUE;
};

std::wstring wide(const QString &s);
QString fromWide(const wchar_t *s, int length = -1);
// What Windows says about an error code, without the full stop and newline on the end.
QString errorText(DWORD error);
QString lastErrorText();

// A drive, partition or volume (\\.\PhysicalDrive1, \\?\GLOBALROOT\Device\Harddisk1\Partition2,
// \\?\Volume{...} without the last backslash). access 0 is enough for most queries and
// needs no administrator.
Handle openDevice(const QString &path, DWORD access, DWORD flags = 0);

// DeviceIoControl, with an answer buffer that grows until it fits. Empty when it failed
// (GetLastError says why).
QByteArray ioctl(HANDLE h, DWORD code, const void *in = nullptr, DWORD inSize = 0, DWORD startSize = 4096);
// For answers of a known size, or none.
bool ioctl(HANDLE h, DWORD code, const void *in, DWORD inSize, void *out, DWORD outSize);

// Lower case without braces, the way Linux shows GUIDs.
QString guidString(const GUID &g);

// Windows' own cmdlets (the Storage Management API that Disk Management uses, Optimize-Volume,
// scheduled tasks...), each in a PowerShell of its own. The script goes in encoded, and the
// values as environment variables, so nothing in them can change what runs. `done` gets whether
// it worked and what the script wrote, or Windows' own words when it didn't.
void powerShell(QObject *owner, const QString &script, const QHash<QString, QString> &values,
                const std::function<void(bool ok, const QString &said)> &done);
// A cmdlet's error the way people read it: without "Activity ID: {...}" and the like.
QString windowsWords(QString said);

} // namespace win
