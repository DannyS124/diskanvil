// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "testboot.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#ifndef Q_OS_WIN
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace testboot {

QString qemu()
{
#ifdef Q_OS_WIN
    for (const QString &dir : {qEnvironmentVariable("ProgramFiles") + QStringLiteral("/qemu"), QStringLiteral("C:/Program Files/qemu")}) {
        if (QFileInfo::exists(dir + QStringLiteral("/qemu-system-x86_64.exe")))
            return dir + QStringLiteral("/qemu-system-x86_64.exe");
    }
#endif
    return QStandardPaths::findExecutable(QStringLiteral("qemu-system-x86_64"));
}

bool ovmf(QString *code, QString *vars)
{
    // Where Arch, Debian/Ubuntu, Fedora and QEMU for Windows keep it.
    const QStringList dirs = {QStringLiteral("/usr/share/edk2/x64"), QStringLiteral("/usr/share/OVMF"), QStringLiteral("/usr/share/edk2/ovmf"),
                              QStringLiteral("/usr/share/qemu"), QFileInfo(qemu()).absolutePath() + QStringLiteral("/share")};
    const QStringList codes = {QStringLiteral("OVMF_CODE.4m.fd"), QStringLiteral("OVMF_CODE_4M.fd"), QStringLiteral("OVMF_CODE.fd"),
                               QStringLiteral("edk2-x86_64-code.fd")};
    const QStringList settings = {QStringLiteral("OVMF_VARS.4m.fd"), QStringLiteral("OVMF_VARS_4M.fd"), QStringLiteral("OVMF_VARS.fd"),
                                  QStringLiteral("edk2-i386-vars.fd")};
    for (const QString &dir : dirs) {
        for (int i = 0; i < codes.size(); ++i) {
            if (QFileInfo::exists(dir + QLatin1Char('/') + codes[i]) && QFileInfo::exists(dir + QLatin1Char('/') + settings[i])) {
                *code = dir + QLatin1Char('/') + codes[i];
                *vars = dir + QLatin1Char('/') + settings[i];
                return true;
            }
        }
    }
    return false;
}

bool kvm()
{
#ifdef Q_OS_WIN
    return false; // WHPX, when QEMU has it, is asked for by name
#else
    QFile f(QStringLiteral("/dev/kvm"));
    return f.open(QIODevice::ReadWrite);
#endif
}

QString formatOf(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QStringLiteral("raw");
    const QByteArray start = f.read(512);
    if (start.startsWith("QFI\xfb"))
        return QStringLiteral("qcow2");
    if (start.startsWith("vhdxfile"))
        return QStringLiteral("vhdx");
    if (start.startsWith("KDMV") || start.contains("# Disk DescriptorFile"))
        return QStringLiteral("vmdk");
    if (start.size() >= 0x48 && quint8(start[0x40]) == 0x7F && quint8(start[0x41]) == 0x10 && quint8(start[0x42]) == 0xDA && quint8(start[0x43]) == 0xBE)
        return QStringLiteral("vdi");
    if (start.startsWith("conectix") || (f.size() >= 512 && f.seek(f.size() - 512) && f.read(8) == "conectix"))
        return QStringLiteral("vpc");
    return QStringLiteral("raw");
}

QStringList arguments(const Options &o, const QString &ovmfCode, const QString &varsCopy, bool useKvm)
{
    // The snapshot takes the writes; the drive or image is only read (QEMU reads it read-only when
    // it can't be written, as a drive opened read-only can't). Its disk can't be read-only itself:
    // QEMU's SATA disks refuse that.
    // A drive's handle goes in as it is (reopening it by name would need rights to the drive).
    const QString file = o.fd >= 0 ? QStringLiteral("/dev/fdset/1") : QString(o.file).replace(QLatin1Char(','), QStringLiteral(",,"));
    const QString drive = QStringLiteral("file.filename=%1,file.driver=%2,format=%3,if=none,id=disk0,snapshot=on")
                              .arg(file, o.device ? QStringLiteral("host_device") : QStringLiteral("file"), o.format);
    QStringList args;
    if (o.fd >= 0)
        args << QStringLiteral("-add-fd") << QStringLiteral("fd=%1,set=1,opaque=drive").arg(o.fd);
    args << QStringList{QStringLiteral("-name"), QStringLiteral("DiskAnvil: Try Starting It"), QStringLiteral("-m"), QString::number(o.memoryMiB),
                        QStringLiteral("-smp"), QStringLiteral("2"), QStringLiteral("-drive"), drive,
                        QStringLiteral("-device"), QStringLiteral("ahci,id=ahci"), QStringLiteral("-device"), QStringLiteral("ide-hd,drive=disk0,bus=ahci.0,bootindex=0"),
                        QStringLiteral("-nic"), QStringLiteral("none")};
    if (useKvm)
        args << QStringLiteral("-enable-kvm") << QStringLiteral("-cpu") << QStringLiteral("host");
    if (o.uefi && !ovmfCode.isEmpty())
        args << QStringLiteral("-machine") << QStringLiteral("q35")
             << QStringLiteral("-drive") << QStringLiteral("if=pflash,format=raw,readonly=on,file=%1").arg(ovmfCode)
             << QStringLiteral("-drive") << QStringLiteral("if=pflash,format=raw,file=%1").arg(varsCopy);
    else
        args << QStringLiteral("-machine") << QStringLiteral("q35");
    if (!o.serialLog.isEmpty())
        args << QStringLiteral("-serial") << QStringLiteral("file:%1").arg(o.serialLog);
    if (!o.window)
        args << QStringLiteral("-display") << QStringLiteral("none");
    return args;
}

QProcess *start(const Options &given, QObject *parent, QString *error)
{
    Options o = given;
#ifndef Q_OS_WIN
    const int fd = o.fd;
    struct stat st {};
    if (fd >= 0 && ::fstat(fd, &st) == 0)
        o.device = S_ISBLK(st.st_mode);
#endif
    const QString program = qemu();
    if (program.isEmpty()) {
        *error = QCoreApplication::translate("testboot", "It needs QEMU (qemu-system-x86_64), which isn't on this computer.");
        return nullptr;
    }
    QString code, vars;
    if (o.uefi && !ovmf(&code, &vars)) {
        *error = QCoreApplication::translate("testboot", "Starting it the UEFI way needs OVMF (QEMU's UEFI firmware), which isn't on this computer.");
        return nullptr;
    }
    auto *dir = new QTemporaryDir;
    const QString varsCopy = dir->filePath(QStringLiteral("vars.fd"));
    if (o.uefi && (!QFile::copy(vars, varsCopy) || !QFile::setPermissions(varsCopy, QFileDevice::ReadOwner | QFileDevice::WriteOwner))) {
        delete dir;
        *error = QCoreApplication::translate("testboot", "UEFI's settings couldn't be copied for it.");
        return nullptr;
    }
    auto *process = new QProcess(parent);
#ifndef Q_OS_WIN
    if (fd >= 0) {
        // QEMU reads it the ordinary way: a handle opened for direct reads would refuse those.
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_DIRECT);
        // It's handed on to QEMU (everything DiskAnvil opens is kept from the programs it
        // starts): only while QEMU starts.
        ::fcntl(fd, F_SETFD, 0);
    }
#endif
    QObject::connect(process, &QProcess::finished, process, [dir] { delete dir; });
    process->start(program, arguments(o, code, varsCopy, kvm()));
    const bool started = process->waitForStarted(10000);
#ifndef Q_OS_WIN
    if (fd >= 0)
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
    if (!started) {
        *error = QCoreApplication::translate("testboot", "QEMU didn't start: %1").arg(process->errorString());
        delete process;
        delete dir;
        return nullptr;
    }
    return process;
}

} // namespace testboot
