// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

#include "linuxboot.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace linuxboot {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("linuxboot", text);
}

QString osRelease(const QString &root, const QString &key)
{
    for (const QString &path : {root + QStringLiteral("/etc/os-release"), root + QStringLiteral("/usr/lib/os-release")}) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QRegularExpression line(QStringLiteral("^%1=\"?([^\"\\n]*)\"?$").arg(key), QRegularExpression::MultilineOption);
        return line.match(QString::fromUtf8(f.readAll())).captured(1);
    }
    return {};
}

bool exists(const QString &root, const QStringList &paths)
{
    for (const QString &p : paths) {
        // Merged /usr: /usr/sbin and /sbin are often links; QFileInfo follows those inside the
        // tree only when they're relative, so each is looked for as it is.
        if (QFileInfo::exists(root + p))
            return true;
    }
    return false;
}

// A quoted word for the shell.
QString q(const QString &text)
{
    QString out = text;
    out.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + out + QLatin1Char('\'');
}

} // namespace

QVector<FstabEntry> parseFstab(const QByteArray &fstab)
{
    QVector<FstabEntry> out;
    for (const QByteArray &raw : fstab.split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const QStringList f = line.split(QRegularExpression(QStringLiteral("\\s+")));
        if (f.size() < 3)
            continue;
        // Spaces in names are written \040.
        auto unescape = [](QString s) { return s.replace(QStringLiteral("\\040"), QStringLiteral(" ")); };
        out.push_back({unescape(f[0]), unescape(f[1]), f[2], f.value(3)});
    }
    return out;
}

QString deviceFor(const QString &what, const QVector<Disk> &disks)
{
    const QString key = what.section(QLatin1Char('='), 0, 0).toUpper();
    const QString value = what.section(QLatin1Char('='), 1).remove(QLatin1Char('"'));
    for (const Disk &d : disks) {
        for (const Volume &v : d.volumes) {
            if ((key == QLatin1String("UUID") && v.uuid.compare(value, Qt::CaseInsensitive) == 0)
                || (key == QLatin1String("PARTUUID") && v.partUuid.compare(value, Qt::CaseInsensitive) == 0)
                || (key == QLatin1String("LABEL") && v.label == value) || (key == QLatin1String("PARTLABEL") && v.partName == value)
                || (what.startsWith(QLatin1Char('/')) && v.device == what))
                return v.device;
        }
    }
    return {};
}

QStringList rootSubvolumes(const QString &top)
{
    QStringList out;
    for (const QString &name : {QStringLiteral("@"), QStringLiteral("root"), QStringLiteral("@root"), QStringLiteral("@rootfs")}) {
        if (QFileInfo::exists(top + QLatin1Char('/') + name + QStringLiteral("/etc/fstab")))
            out << name;
    }
    return out;
}

Install inspect(const QString &root, const QString &rootDevice, const QString &subvolume, const QVector<Disk> &disks)
{
    Install in;
    in.rootDevice = rootDevice;
    in.subvolume = subvolume;
    in.name = osRelease(root, QStringLiteral("PRETTY_NAME"));
    in.id = osRelease(root, QStringLiteral("ID"));
    for (const Disk &d : disks) {
        for (const Volume &v : d.volumes) {
            if (v.device == rootDevice)
                in.disk = d.device;
        }
    }
    QFile fstab(root + QStringLiteral("/etc/fstab"));
    if (fstab.open(QIODevice::ReadOnly)) {
        for (const FstabEntry &e : parseFstab(fstab.readAll())) {
            if (e.where == QLatin1String("/boot") && e.type != QLatin1String("vfat")) {
                in.bootDevice = deviceFor(e.what, disks);
                if (in.bootDevice.isEmpty())
                    in.problems << tr("its /boot partition (%1) isn't here").arg(e.what);
            } else if ((e.where == QLatin1String("/boot/efi") || e.where == QLatin1String("/efi") || e.where == QLatin1String("/boot"))
                       && e.type == QLatin1String("vfat")) {
                in.espMount = e.where;
                in.espDevice = deviceFor(e.what, disks);
                if (in.espDevice.isEmpty())
                    in.problems << tr("its EFI partition (%1) isn't here").arg(e.what);
            }
        }
    }
    for (const Disk &d : disks) {
        for (const Volume &v : d.volumes) {
            if (v.device == in.espDevice) {
                in.espDisk = d.device;
                in.espPartition = v.number;
            }
        }
    }
    if (exists(root, {QStringLiteral("/usr/bin/bootctl")}) && !in.espDevice.isEmpty()
        && (exists(root, {QStringLiteral("/boot/loader/loader.conf"), QStringLiteral("/efi/loader/loader.conf"), QStringLiteral("/boot/efi/loader/loader.conf")})
            || (!exists(root, {QStringLiteral("/usr/sbin/grub-install"), QStringLiteral("/usr/bin/grub-install"), QStringLiteral("/usr/sbin/grub2-install"),
                               QStringLiteral("/usr/bin/grub2-install")}))))
        in.loader = Install::Loader::SystemdBoot;
    else if (exists(root, {QStringLiteral("/usr/sbin/grub2-install"), QStringLiteral("/usr/bin/grub2-install"), QStringLiteral("/usr/sbin/grub2-mkconfig")}))
        in.loader = Install::Loader::Grub2;
    else if (exists(root, {QStringLiteral("/usr/sbin/grub-install"), QStringLiteral("/usr/bin/grub-install")}))
        in.loader = Install::Loader::Grub;
    else
        in.problems << tr("no boot loader DiskAnvil knows (GRUB or systemd-boot) is installed in it");
    if (in.name.isEmpty())
        in.problems << tr("it doesn't say what it is (no os-release)");
    return in;
}

QStringList plan(const Install &in, bool uefi)
{
    QStringList out;
    out << tr("Mount %1 (and its /boot and EFI partitions) at /mnt/diskanvil-repair.").arg(in.name);
    switch (in.loader) {
    case Install::Loader::Grub:
        out << (uefi ? tr("Put GRUB back on its EFI partition, with a boot menu entry for it (grub-install).")
                     : tr("Put GRUB's boot code back on %1 (grub-install).").arg(in.disk));
        out << tr("Make its GRUB menu again, so it lists its kernels and other systems (update-grub or grub-mkconfig).");
        break;
    case Install::Loader::Grub2:
        out << (uefi ? tr("Add its boot menu entry again for Secure Boot's shim, when its files are on the EFI partition (efibootmgr); "
                          "otherwise put GRUB back there (grub2-install).")
                     : tr("Put GRUB's boot code back on %1 (grub2-install).").arg(in.disk));
        out << tr("Make its GRUB menu again (grub2-mkconfig).");
        break;
    case Install::Loader::SystemdBoot:
        out << tr("Put systemd-boot back on its EFI partition, with its boot menu entry (bootctl install).");
        break;
    case Install::Loader::Unknown:
        break;
    }
    out << tr("Unmount it all again.");
    return out;
}

QString repairScript(const Install &in, bool uefi)
{
    const QString r = QStringLiteral("/mnt/diskanvil-repair");
    QStringList s;
    s << QStringLiteral("set -u") << QStringLiteral("R=%1").arg(r) << QStringLiteral("mkdir -p \"$R\"")
      << QStringLiteral("cleanup() { umount -R \"$R\" 2>/dev/null; }") << QStringLiteral("trap cleanup EXIT")
      << QStringLiteral("set -e")
      << QStringLiteral("mount %1%2 \"$R\"").arg(in.subvolume.isEmpty() ? QString() : QStringLiteral("-o subvol=%1 ").arg(q(in.subvolume)), q(in.rootDevice));
    if (!in.bootDevice.isEmpty())
        s << QStringLiteral("mount %1 \"$R/boot\"").arg(q(in.bootDevice));
    if (!in.espDevice.isEmpty())
        s << QStringLiteral("mkdir -p \"$R%1\"").arg(in.espMount) << QStringLiteral("mount %1 \"$R%2\"").arg(q(in.espDevice), in.espMount);
    s << QStringLiteral("for d in dev proc sys run; do mount --rbind \"/$d\" \"$R/$d\"; mount --make-rslave \"$R/$d\"; done");
    const QString id = in.id.isEmpty() ? QStringLiteral("linux") : in.id;
    QStringList inside;
    switch (in.loader) {
    case Install::Loader::Grub:
        inside << (uefi ? QStringLiteral("grub-install --target=x86_64-efi --efi-directory=%1 --bootloader-id=%2").arg(q(in.espMount), q(id))
                        : QStringLiteral("grub-install --target=i386-pc %1").arg(q(in.disk)));
        inside << QStringLiteral("if command -v update-grub >/dev/null; then update-grub; else grub-mkconfig -o /boot/grub/grub.cfg; fi");
        break;
    case Install::Loader::Grub2:
        // Fedora's and openSUSE's GRUB on UEFI comes signed, with shim, from their packages:
        // grub2-install would replace it with one Secure Boot refuses. Its entry is what's lost.
        if (uefi)
            inside << QStringLiteral("if [ -e %1/EFI/%2/shimx64.efi ]; then efibootmgr -c -d %3 -p %4 -l %5 -L %6; "
                                     "else grub2-install --target=x86_64-efi --efi-directory=%1 --bootloader-id=%2; fi")
                          .arg(q(in.espMount), q(id), q(in.espDisk))
                          .arg(in.espPartition)
                          .arg(q(QStringLiteral("\\EFI\\%1\\shimx64.efi").arg(id)), q(in.name.isEmpty() ? id : in.name.section(QLatin1Char(' '), 0, 0)));
        else
            inside << QStringLiteral("grub2-install --target=i386-pc %1").arg(q(in.disk));
        inside << QStringLiteral("grub2-mkconfig -o /boot/grub2/grub.cfg");
        break;
    case Install::Loader::SystemdBoot:
        inside << QStringLiteral("bootctl install --esp-path=%1").arg(q(in.espMount));
        break;
    case Install::Loader::Unknown:
        inside << QStringLiteral("echo 'no boot loader to repair' >&2; exit 1");
        break;
    }
    s << QStringLiteral("chroot \"$R\" /bin/sh -c %1").arg(q(QStringLiteral("set -ex; ") + inside.join(QStringLiteral("; "))));
    s << QStringLiteral("echo DISKANVIL-REPAIR-DONE");
    return s.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

} // namespace linuxboot
