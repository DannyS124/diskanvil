// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "winutil.h"

#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QVector>

namespace win {

std::wstring wide(const QString &s)
{
    return s.toStdWString();
}

QString fromWide(const wchar_t *s, int length)
{
    if (!s)
        return {};
    return length < 0 ? QString::fromWCharArray(s) : QString::fromWCharArray(s, length);
}

QString errorText(DWORD error)
{
    wchar_t *buffer = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, error, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    QString text = n ? QString::fromWCharArray(buffer, int(n)).trimmed() : QStringLiteral("error %1").arg(error);
    if (buffer)
        LocalFree(buffer);
    if (text.endsWith(QLatin1Char('.')))
        text.chop(1);
    return text;
}

QString lastErrorText()
{
    return errorText(GetLastError());
}

Handle openDevice(const QString &path, DWORD access, DWORD flags)
{
    return Handle(CreateFileW(wide(path).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              flags, nullptr));
}

QByteArray ioctl(HANDLE h, DWORD code, const void *in, DWORD inSize, DWORD startSize)
{
    QByteArray out(qsizetype(startSize), '\0');
    for (int tries = 0; tries < 8; ++tries) {
        DWORD got = 0;
        if (DeviceIoControl(h, code, const_cast<void *>(in), inSize, out.data(), DWORD(out.size()), &got, nullptr)) {
            out.resize(qsizetype(got));
            return out;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_INSUFFICIENT_BUFFER && error != ERROR_MORE_DATA)
            return {};
        out.resize(out.size() * 4);
    }
    return {};
}

bool ioctl(HANDLE h, DWORD code, const void *in, DWORD inSize, void *out, DWORD outSize)
{
    DWORD got = 0;
    return DeviceIoControl(h, code, const_cast<void *>(in), inSize, out, outSize, &got, nullptr);
}

QString guidString(const GUID &g)
{
    QString text = QStringLiteral("%1-%2-%3-").arg(uint(g.Data1), 8, 16, QLatin1Char('0'))
                       .arg(uint(g.Data2), 4, 16, QLatin1Char('0'))
                       .arg(uint(g.Data3), 4, 16, QLatin1Char('0'));
    for (int i = 0; i < 8; ++i) {
        if (i == 2)
            text += QLatin1Char('-');
        text += QStringLiteral("%1").arg(uint(g.Data4[i]), 2, 16, QLatin1Char('0'));
    }
    return text;
}

QString windowsWords(QString said)
{
    said = said.simplified();
    const qsizetype extended = said.indexOf(QLatin1String("Extended information:"));
    if (extended >= 0)
        said = said.mid(extended + 21);
    static const QRegularExpression activity(QStringLiteral("\\s*Activity ID: \\{[^}]*\\}"));
    return said.remove(activity).trimmed();
}

void powerShell(QObject *owner, const QString &script, const QHash<QString, QString> &values,
                const std::function<void(bool, const QString &)> &done)
{
    auto *ps = new QProcess(owner);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        env.insert(it.key(), it.value());
    ps->setProcessEnvironment(env);
    QObject::connect(ps, &QProcess::finished, owner, [ps, done](int code, QProcess::ExitStatus status) {
        ps->deleteLater();
        done(status == QProcess::NormalExit && code == 0, windowsWords(QString::fromUtf8(ps->readAllStandardOutput())));
    });
    // Progress records (modules being set up the first time) would come out as XML.
    const QString wrapped = QStringLiteral("[Console]::OutputEncoding = [Text.Encoding]::UTF8; $ErrorActionPreference = 'Stop'; "
                                           "$ProgressPreference = 'SilentlyContinue'; "
                                           "try { %1 } catch { Write-Output $_.Exception.Message; exit 1 }")
                                .arg(script);
    QByteArray utf16;
    for (const QChar c : wrapped) {
        utf16 += char(c.unicode() & 0xff);
        utf16 += char(c.unicode() >> 8);
    }
    ps->start(QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-ExecutionPolicy"),
                                                 QStringLiteral("Bypass"), QStringLiteral("-EncodedCommand"), QString::fromLatin1(utf16.toBase64())});
}

} // namespace win
