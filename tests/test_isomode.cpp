// SPDX-FileCopyrightText: 2026 Danny S
// SPDX-License-Identifier: GPL-3.0-or-later

// Copying a Linux ISO's files: what an ISO allows, the FAT label, and the boot menu patching,
// on lines like the ones real distros ship. Then the whole way through a real ISO.

#include "testkit.h"

#include "../src/usb/filecopy.h"
#include "../src/usb/isomode.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace {

using isomode::Persistence;

QVector<filecopy::Entry> entries(const QStringList &files, quint64 bigSize = 0)
{
    QVector<filecopy::Entry> out;
    for (const QString &f : files)
        out.push_back({f, f.endsWith(QLatin1Char('/')), f == QLatin1String("big.img") ? bigSize : 1000});
    return out;
}

void analysis()
{
    const isomode::Analysis ubuntu = isomode::analyse(entries({QStringLiteral("EFI/BOOT/BOOTX64.EFI"), QStringLiteral("casper/vmlinuz"),
                                                              QStringLiteral("casper/filesystem.squashfs")}),
                                                      QStringLiteral("Ubuntu 24.04.1 LTS amd64"));
    report(ubuntu.canCopy() && ubuntu.persistence == Persistence::Casper, QStringLiteral("Ubuntu: can be copied, with casper persistence"));
    const isomode::Analysis debian = isomode::analyse(entries({QStringLiteral("EFI/boot/bootx64.efi"), QStringLiteral("live/filesystem.squashfs")}),
                                                      QStringLiteral("d-live 13.1.0 gn amd64"));
    report(debian.canCopy() && debian.persistence == Persistence::LiveBoot, QStringLiteral("Debian live: can be copied, with live-boot persistence"));
    const isomode::Analysis arch = isomode::analyse(entries({QStringLiteral("EFI/BOOT/BOOTx64.EFI"), QStringLiteral("arch/x86_64/airootfs.sfs")}),
                                                    QStringLiteral("ARCH_202410"));
    report(arch.canCopy() && arch.persistence == Persistence::None && arch.fatLabel == QLatin1String("ARCH_202410"),
           QStringLiteral("Arch: can be copied, no persistence, the label fits as it is"));
    const isomode::Analysis windows = isomode::analyse(entries({QStringLiteral("bootmgr"), QStringLiteral("efi/boot/bootx64.efi"),
                                                               QStringLiteral("sources/install.wim")}), QStringLiteral("CCCOMA_X64FRE_EN-US_DV9"));
    report(!windows.canCopy() && windows.windows && windows.whyNot().contains(QLatin1String("Make a Windows USB")),
           QStringLiteral("a Windows ISO is sent to Make a Windows USB"));
    const isomode::Analysis udf = isomode::analyse(entries({QStringLiteral("README.TXT")}), QStringLiteral("CCCOMA_X64FRE_EN-US_DV9"));
    report(udf.windows, QStringLiteral("so is one that only shows a README (Microsoft's keep their files in UDF)"));
    const isomode::Analysis bios = isomode::analyse(entries({QStringLiteral("isolinux/isolinux.bin"), QStringLiteral("live/vmlinuz")}), QStringLiteral("OLD"));
    report(!bios.canCopy() && !bios.whyNot().isEmpty(), QStringLiteral("one without UEFI files has to be written as it is"));
    const isomode::Analysis proxmox = isomode::analyse(entries({QStringLiteral("EFI/BOOT/BOOTX64.EFI"), QStringLiteral("proxmox/")}), QStringLiteral("PVE"));
    const isomode::Analysis suse = isomode::analyse(entries({QStringLiteral("EFI/BOOT/BOOTX64.EFI")}), QStringLiteral("openSUSE-Tumbleweed-DVD-x86_64"));
    report(!proxmox.canCopy() && !suse.canCopy(), QStringLiteral("Proxmox and openSUSE only work written as they are (like Rufus says)"));
    const isomode::Analysis big = isomode::analyse(entries({QStringLiteral("EFI/BOOT/BOOTX64.EFI"), QStringLiteral("big.img")}, 5ULL << 30),
                                                   QStringLiteral("BIG"));
    report(!big.canCopy() && big.bigFile == QLatin1String("big.img"), QStringLiteral("a file over 4 GB rules it out, naming the file"));

    report(isomode::fatLabel(QStringLiteral("Fedora-WS-Live-40-1-14")) == QLatin1String("FEDORA-WS-L")
               && isomode::fatLabel(QStringLiteral("My Linux 1.0")) == QLatin1String("MY_LINUX_1_")
               && isomode::fatLabel(QStringLiteral("a:b*c?")) == QLatin1String("ABC") && isomode::fatLabel(QString()) == QLatin1String("LIVEUSB"),
           QStringLiteral("FAT labels: upper case, 11 at most, nothing FAT refuses"));
    report(isomode::isBootConfig(QStringLiteral("boot/grub/grub.cfg")) && isomode::isBootConfig(QStringLiteral("loader/entries/arch.conf"))
               && !isomode::isBootConfig(QStringLiteral("etc/foo.conf")) && !isomode::isBootConfig(QStringLiteral("live/vmlinuz")),
           QStringLiteral("boot menus are told from other files"));
}

void patching()
{
    const QByteArray fedora =
        "set default=\"1\"\n"
        "search --no-floppy --set=root -l 'Fedora-WS-Live-40-1-14'\n"
        "menuentry 'Start Fedora-WS-Live-40-1-14' --class fedora {\n"
        "\tlinux /images/pxeboot/vmlinuz root=live:CDLABEL=Fedora-WS-Live-40-1-14 rd.live.image quiet rhgb\n"
        "\tinitrd /images/pxeboot/initrd.img\n"
        "}\n";
    const QByteArray fedoraOut = isomode::patchConfig(fedora, QStringLiteral("Fedora-WS-Live-40-1-14"), QStringLiteral("FEDORA-WS-L"), Persistence::None);
    report(fedoraOut.contains("root=live:CDLABEL=FEDORA-WS-L rd.live.image") && fedoraOut.contains("-l 'FEDORA-WS-L'")
               && fedoraOut.contains("menuentry 'Start Fedora-WS-Live-40-1-14'"),
           QStringLiteral("Fedora: CDLABEL and the search line get the stick's label, the menu title doesn't"));

    const QByteArray ubuntu = "menuentry \"Try or Install Ubuntu\" {\n\tlinux\t/casper/vmlinuz  --- quiet splash\n\tinitrd\t/casper/initrd\n}\n";
    const QByteArray ubuntuOut = isomode::patchConfig(ubuntu, QStringLiteral("Ubuntu 24.04.1 LTS amd64"), QStringLiteral("UBUNTU_24_0"), Persistence::Casper);
    report(ubuntuOut.contains("/casper/vmlinuz persistent  --- quiet splash") && !ubuntuOut.contains("initrd\t/casper/initrd persistent"),
           QStringLiteral("Ubuntu: \"persistent\" goes right after the kernel"));
    const QByteArray loopback = "\tlinux\t/casper/vmlinuz boot=casper iso-scan/filename=${iso_path} quiet splash\n";
    report(isomode::patchConfig(loopback, QString(), QString(), Persistence::Casper).contains("boot=casper persistent iso-scan"),
           QStringLiteral("and after boot=casper where there is one"));

    const QByteArray debian = "menuentry \"Live system\" {\r\n    linux /live/vmlinuz boot=live components quiet splash\r\n    initrd /live/initrd.img\r\n}\r\n";
    const QByteArray debianOut = isomode::patchConfig(debian, QStringLiteral("d-live 13.1.0 gn amd64"), QStringLiteral("D-LIVE_13_1"), Persistence::LiveBoot);
    report(debianOut.contains("boot=live persistence components quiet splash\r\n") && debianOut.count("\r\n") == debian.count("\r\n"),
           QStringLiteral("Debian live: \"persistence\" after boot=live, Windows line endings kept"));
    report(isomode::patchConfig(debianOut, QStringLiteral("d-live 13.1.0 gn amd64"), QStringLiteral("D-LIVE_13_1"), Persistence::LiveBoot) == debianOut
               && isomode::patchConfig(ubuntuOut, QString(), QString(), Persistence::Casper) == ubuntuOut,
           QStringLiteral("patching twice changes nothing more"));

    const QByteArray spaced = "search --set=root --label My\\x20Linux\\x201.0\nlinux /vmlinuz root=LABEL=My\\x20Linux\\x201.0\n";
    report(isomode::patchConfig(spaced, QStringLiteral("My Linux 1.0"), QStringLiteral("MY_LINUX_1_"), Persistence::None)
               == "search --set=root --label MY_LINUX_1_\nlinux /vmlinuz root=LABEL=MY_LINUX_1_\n",
           QStringLiteral("a label with spaces, written the way GRUB escapes them"));
    const QByteArray arch = "linux /arch/boot/x86_64/vmlinuz-linux archisobasedir=arch archisolabel=ARCH_202410\n";
    report(isomode::patchConfig(arch, QStringLiteral("ARCH_202410"), QStringLiteral("ARCH_202410"), Persistence::None) == arch,
           QStringLiteral("Arch's label already fits, so nothing changes"));
}

// A Fedora-like ISO made by xorriso, through the copier with the patching on.
void throughAnIso()
{
    if (QStandardPaths::findExecutable(QStringLiteral("xorriso")).isEmpty()) {
        out << "SKIP  xorriso isn't installed, so there's no test ISO" << Qt::endl;
        return;
    }
    QTemporaryDir dir;
    const QString tree = dir.filePath(QStringLiteral("tree"));
    auto put = [&](const QString &path, const QByteArray &data) {
        QDir().mkpath(QFileInfo(tree + QLatin1Char('/') + path).path());
        QFile f(tree + QLatin1Char('/') + path);
        if (f.open(QIODevice::WriteOnly))
            f.write(data);
    };
    put(QStringLiteral("EFI/BOOT/BOOTX64.EFI"), QByteArray(4096, 'e'));
    put(QStringLiteral("EFI/BOOT/grub.cfg"), "search -l 'Fedora-WS-Live-40'\nlinux /images/pxeboot/vmlinuz root=live:CDLABEL=Fedora-WS-Live-40 quiet\n");
    put(QStringLiteral("images/pxeboot/vmlinuz"), QByteArray(100000, 'k'));
    const QString iso = dir.filePath(QStringLiteral("fedora.iso"));
    sh(QStringLiteral("xorriso"), {QStringLiteral("-as"), QStringLiteral("mkisofs"), QStringLiteral("-quiet"), QStringLiteral("-input-charset"), QStringLiteral("UTF-8"), QStringLiteral("-J"),
                                   QStringLiteral("-joliet-long"), QStringLiteral("-R"), QStringLiteral("-V"), QStringLiteral("Fedora-WS-Live-40"),
                                   QStringLiteral("-o"), iso, tree});
    const std::unique_ptr<filecopy::Source> source = filecopy::openIso(iso);
    const isomode::Analysis a = isomode::analyse(source->entries(), source->label());
    report(source->error().isEmpty() && a.canCopy() && a.isoLabel == QLatin1String("Fedora-WS-Live-40") && a.fatLabel == QLatin1String("FEDORA-WS-L"),
           QStringLiteral("a real ISO: label read, can be copied"), source->error() + a.isoLabel);
    filecopy::Options options;
    options.transform = [&a](const QString &path, const QByteArray &data) {
        return isomode::isBootConfig(path) ? isomode::patchConfig(data, a.isoLabel, a.fatLabel, a.persistence) : QByteArray();
    };
    const QString stick = dir.filePath(QStringLiteral("stick"));
    QDir().mkpath(stick);
    filecopy::Copier copier(source.get(), stick, options);
    bool ok = false;
    QObject::connect(&copier, &filecopy::Copier::finished, [&](bool done, const QString &) { ok = done; });
    copier.run();
    QFile cfg(stick + QStringLiteral("/EFI/BOOT/grub.cfg"));
    report(ok && cfg.open(QIODevice::ReadOnly) && cfg.readAll() == "search -l 'FEDORA-WS-L'\nlinux /images/pxeboot/vmlinuz root=live:CDLABEL=FEDORA-WS-L quiet\n",
           QStringLiteral("copied, with the boot menu pointing at the stick's label, and checked"));
}

} // namespace

void isoModeTests()
{
    analysis();
    patching();
    throughAnIso();
}
