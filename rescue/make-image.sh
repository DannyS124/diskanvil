#!/bin/bash
# Runs inside the builder container; rescue/build.sh starts it. Makes the live system with
# mmdebstrap, then puts the ISO together around it.
#
# Everything here is ours except Debian's packages and its signed boot files (shim, GRUB and
# the kernel). Those are what Secure Boot trusts, so they're used exactly as Debian ships them.
set -euo pipefail
version=$1
rescue=/src/rescue
work=/work
chroot=$work/chroot
iso=$work/iso
built=$(date -u +%Y-%m-%d)
build_id=$(cat /proc/sys/kernel/random/uuid)
name=diskanvil-live-$version-$(date -u +%Y%m%d)
deb=/src/packaging/deb/out/diskanvil_${version}_amd64.deb
[ -f "$deb" ] || { echo "No DiskAnvil package at $deb"; exit 1; }

packages=$(grep -v '^#' "$rescue/packages.txt" | grep -v '^ *$' | paste -sd, -)
mirror=http://deb.debian.org/debian
components="main contrib non-free-firmware"
mkdir -p "$work" "$iso"

# The pictures: one drawing for the boot menu and the desktop.
mkdir -p "$work/art"
rsvg-convert -w 1920 -h 1080 "$rescue/art/boot.svg" -o "$work/art/background.png"
rsvg-convert -w 1920 -h 1080 "$rescue/art/desktop.svg" -o "$work/art/wallpaper.png"
# The boot animation (Plymouth): its script, and its pictures from the SVGs.
plymouth=$work/plymouth/diskanvil-live
mkdir -p "$plymouth"
cp "$rescue/plymouth/diskanvil-live.plymouth" "$rescue/plymouth/diskanvil-live.script" "$plymouth/"
rsvg-convert -w 160 -h 160 "$rescue/art/diskanvil-live-icon.svg" -o "$plymouth/logo.png"
for picture in glow name track fill; do
    rsvg-convert "$rescue/plymouth/$picture.svg" -o "$plymouth/$picture.png"
done
# Our icons, with the logo as the start button and the app icon.
mkdir -p "$work/icons"
cp -a "$rescue/icons/DiskAnvil-Live" "$work/icons/"
cp "$rescue/art/diskanvil-live-icon.svg" "$work/icons/DiskAnvil-Live/scalable/apps/diskanvil-live.svg"
ln -sf diskanvil-live.svg "$work/icons/DiskAnvil-Live/scalable/apps/start-here-lxqt.svg"
ln -sf diskanvil-live.svg "$work/icons/DiskAnvil-Live/scalable/apps/start-here.svg"

echo "--> The live system"
# Downloaded packages live in /cache between builds. The hooks run in order: our files, then
# DiskAnvil, then setup.sh, which cleans up after itself.
# Only what packages.txt names and what that really needs: Debian's "recommended" extras made
# it a whole desktop (a sound server, speech recognition, printing, a mail server...).
mmdebstrap --mode=root --variant=important \
    --components="${components// /,}" \
    --aptopt='Apt::Install-Recommends "false"' \
    --include="$packages" \
    --skip=download/empty \
    --setup-hook='mkdir -p "$1"/var/cache/apt/archives/' \
    --setup-hook='sync-in /cache /var/cache/apt/archives/' \
    --customize-hook="sync-in $rescue/files /" \
    --customize-hook="sync-in $work/plymouth /usr/share/plymouth/themes" \
    --customize-hook='mkdir -p "$1/usr/local/share/icons"' \
    --customize-hook="sync-in $work/icons /usr/local/share/icons" \
    --customize-hook='mkdir -p "$1/usr/local/share/diskanvil-live"' \
    --customize-hook="copy-in $work/art/wallpaper.png $rescue/README.txt $rescue/art/diskanvil-live-icon.svg $rescue/art/diskanvil-live-wordmark.svg /usr/local/share/diskanvil-live" \
    --customize-hook="copy-in $deb /tmp" \
    --customize-hook="chroot \"\$1\" apt-get install -y -q /tmp/$(basename "$deb")" \
    --customize-hook='sync-out /var/cache/apt/archives /cache' \
    --customize-hook="copy-in $rescue/setup.sh $rescue/programs.txt /tmp" \
    --customize-hook="chroot \"\$1\" bash /tmp/setup.sh '$version' '$build_id' '$built'" \
    trixie "$chroot" \
    "deb $mirror trixie $components" \
    "deb $mirror trixie-updates $components" \
    "deb http://security.debian.org/debian-security trixie-security $components"

echo "--> Kernel and the list of what's inside"
mkdir -p "$iso/live" "$iso/.disk" "$iso/boot/grub/fonts" "$iso/boot/grub/i386-pc" "$iso/boot/grub/x86_64-efi" "$iso/EFI/BOOT" "$iso/EFI/debian"
kernel=$(ls "$chroot"/boot/vmlinuz-* | sort -V | tail -1)
kver=${kernel##*/vmlinuz-}
cp "$kernel" "$iso/live/vmlinuz"
cp "$chroot/boot/initrd.img-$kver" "$iso/live/initrd.img"
chroot "$chroot" dpkg-query -W -f '${Package} ${Version}\n' > "$iso/live/packages.txt"

echo "--> Packing the system (this takes a few minutes)"
mksquashfs "$chroot" "$iso/live/filesystem.squashfs" -noappend -no-progress -quiet \
    -comp zstd -Xcompression-level 19 -b 1M -xattrs-exclude '^system\.posix_acl' \
    -wildcards -e 'boot/vmlinuz-*' 'boot/initrd.img-*' 'boot/System.map-*' 'boot/config-*' \
    'proc/*' 'sys/*' 'dev/*' 'run/*' 'tmp/*'

echo "--> Boot files"
# How the stick is recognized: GRUB looks for the build's own file, live-boot compares
# live-uuid with the copy in its initrd, DiskAnvil reads .disk/diskanvil-live.
echo "DiskAnvil Live $version ($built)" > "$iso/.disk/info"
echo "$build_id" > "$iso/.disk/live-uuid-amd64"
printf 'DiskAnvil Live\nversion=%s\nbuilt=%s\nid=%s\n' "$version" "$built" "$build_id" > "$iso/.disk/diskanvil-live"
: > "$iso/.disk/diskanvil-live-$build_id"
cp "$rescue/README.txt" "$iso/README.txt"

# UEFI: shim (signed by Microsoft) starts GRUB (signed by Debian), which starts the kernel
# (signed by Debian). This GRUB is the one Debian makes for CDs and USB sticks: it finds
# /.disk/info, then reads /boot/grub/grub.cfg. The small configs next to it are a fallback.
cp --dereference /usr/lib/shim/shimx64.efi.signed "$iso/EFI/BOOT/BOOTX64.EFI"
cp /usr/lib/shim/mmx64.efi.signed "$iso/EFI/BOOT/mmx64.efi"
cp /usr/lib/grub/x86_64-efi-signed/gcdx64.efi.signed "$iso/EFI/BOOT/grubx64.efi"
sed "s/@ID@/$build_id/g" "$rescue/boot/find-stick.cfg" > "$iso/EFI/BOOT/grub.cfg"
cp "$iso/EFI/BOOT/grub.cfg" "$iso/EFI/debian/grub.cfg"
# Its modules, from the same GRUB version, for what isn't built into it (exFAT, for Other ISOs on
# the files drive). It only loads them with Secure Boot off.
[ "$(dpkg-query -W -f='${Version}' grub-efi-amd64-bin)" = "$(dpkg-query -W -f='${Version}' grub-efi-amd64-signed | sed 's/+.*//')" ] ||
    echo "note: grub-efi-amd64-bin $(dpkg-query -W -f='${Version}' grub-efi-amd64-bin) and the signed GRUB $(dpkg-query -W -f='${Version}' grub-efi-amd64-signed) differ"
cp /usr/lib/grub/x86_64-efi/*.mod /usr/lib/grub/x86_64-efi/*.lst "$iso/boot/grub/x86_64-efi/"

# Old BIOS PCs (and VMs set to BIOS): GRUB's own CD image. No Secure Boot there.
cp /usr/lib/grub/i386-pc/*.mod /usr/lib/grub/i386-pc/*.lst "$iso/boot/grub/i386-pc/"
grub-mkimage -O i386-pc -p /boot/grub -o "$work/core.img" biosdisk iso9660
cat /usr/lib/grub/i386-pc/cdboot.img "$work/core.img" > "$iso/boot/grub/i386-pc/eltorito.img"

# Old BIOS PCs from a stick DiskAnvil made (FAT32, files copied): DiskAnvil puts stick-boot.img
# in the stick's first sector and stick-core.img right after it, in the free space before the
# partition, the way grub-install does. The core finds the stick by this build's file and
# reads the same menu from it. grub-mkimage already points the core at sector 2 and the boot
# sector at sector 1, so nothing needs patching there; like grub-bios-setup does for a hard
# disk, the boot drive check is turned off, for BIOSes that hand over the wrong drive number.
cat > "$work/stick.cfg" <<CFG
search --no-floppy --set=root --file /.disk/diskanvil-live-$build_id
set prefix=(\$root)/boot/grub
CFG
grub-mkimage -O i386-pc -p /boot/grub -c "$work/stick.cfg" -o "$iso/boot/grub/i386-pc/stick-core.img" \
    biosdisk part_msdos fat search search_fs_file
[ "$(stat -c %s "$iso/boot/grub/i386-pc/stick-core.img")" -lt $((1024 * 1024 - 512)) ] ||
    { echo "stick-core.img doesn't fit before the partition"; exit 1; }
cp /usr/lib/grub/i386-pc/boot.img "$work/stick-boot.img"
[ "$(od -An -tx1 -j $((0x66)) -N1 "$work/stick-boot.img" | tr -d ' ')" = eb ] ||
    { echo "GRUB's boot.img has changed: no drive check jump at 0x66"; exit 1; }
printf '\x90\x90' | dd of="$work/stick-boot.img" bs=1 seek=$((0x66)) conv=notrunc status=none
cp "$work/stick-boot.img" "$iso/boot/grub/i386-pc/stick-boot.img"

# Start Other PCs from This One (network start, diskanvil-live-netboot): Debian's signed GRUB
# for network starts after shim on UEFI, and GRUB's own PXE image for old BIOS PCs. Both read
# grub/grub.cfg from the TFTP server, which the helper writes. The live system's initrd already
# has the network cards' drivers.
mkdir -p "$iso/boot/grub/netboot"
cp /usr/lib/grub/x86_64-efi-signed/grubnetx64.efi.signed "$iso/boot/grub/netboot/grubx64.efi"
grub-mkimage -O i386-pc-pxe -p '(tftp)/grub' -o "$work/pxecore.img" \
    pxe tftp net normal linux echo test configfile sleep reboot halt
cat /usr/lib/grub/i386-pc/pxeboot.img "$work/pxecore.img" > "$iso/boot/grub/netboot/core.0"

cp /usr/share/grub/unicode.pf2 "$iso/boot/grub/fonts/"
# The boot menu's own fonts (theme.txt names them) and its selection bar.
dejavu=/usr/share/fonts/truetype/dejavu
for size in 14 16 18; do
    grub-mkfont -s $size -o "$iso/boot/grub/fonts/dejavu-$size.pf2" $dejavu/DejaVuSans.ttf
done
grub-mkfont -s 18 -o "$iso/boot/grub/fonts/dejavu-bold-18.pf2" $dejavu/DejaVuSans-Bold.ttf
for part in w c e; do
    rsvg-convert "$rescue/boot/select/select_$part.svg" -o "$iso/boot/grub/select_$part.png"
done
cp "$work/art/background.png" "$iso/boot/grub/"
cp /boot/memtest86+x64.efi /boot/memtest86+x64.bin "$iso/boot/"
memtest=$(dpkg-query -W -f '${Version}' memtest86+ | sed 's/-.*//')
sed -e "s/@ID@/$build_id/g" -e "s/@VERSION@/$version/g" -e "s/@BUILT@/$built/g" -e "s/@MEMTEST@/$memtest/g" \
    "$rescue/boot/grub.cfg" > "$iso/boot/grub/grub.cfg"
sed -e "s/@VERSION@/$version/g" -e "s/@BUILT@/$built/g" "$rescue/boot/theme.txt" > "$iso/boot/grub/theme.txt"

# The FAT image a PC's firmware boots from when the ISO is a CD (or a VM's CD drive).
efi=$iso/boot/grub/efi.img
mkfs.vfat -C -n BSPARK-EFI "$efi" 8192 >/dev/null
mmd -i "$efi" ::/EFI ::/EFI/BOOT ::/EFI/debian
mcopy -i "$efi" "$iso"/EFI/BOOT/* ::/EFI/BOOT/
mcopy -i "$efi" "$iso/EFI/debian/grub.cfg" ::/EFI/debian/

# For "Check the stick" in the boot menu (live-boot reads this) and for DiskAnvil, which
# checks every file it copies onto a USB stick against it. eltorito.img is left out: xorriso
# writes a table into it while making the ISO, and a FAT32 stick has no use for it anyway.
(cd "$iso" && find . -type f ! -name sha256sum.txt ! -path ./boot/grub/i386-pc/eltorito.img -print0 |
    sort -z | xargs -0 sha256sum) > "$work/sha256sum.txt"
mv "$work/sha256sum.txt" "$iso/sha256sum.txt"

echo "--> The ISO"
# Hybrid: boots as a CD or written straight to a USB stick, on UEFI and BIOS.
xorriso -as mkisofs -quiet \
    -iso-level 3 -full-iso9660-filenames -joliet -joliet-long -rational-rock \
    -volid DISKANVIL_LIVE -appid "DiskAnvil Live $version" -publisher "DiskAnvil Live" \
    -eltorito-boot boot/grub/i386-pc/eltorito.img -no-emul-boot -boot-load-size 4 -boot-info-table \
    --grub2-boot-info --grub2-mbr /usr/lib/grub/i386-pc/boot_hybrid.img \
    -eltorito-catalog boot/grub/boot.cat \
    -eltorito-alt-boot -e boot/grub/efi.img -no-emul-boot -efi-boot-part --efi-boot-image \
    -output "/out/$name.iso" "$iso"
(cd /out && sha256sum "$name.iso" > "$name.iso.sha256")
echo "==> rescue/out/$name.iso ($(du -h "/out/$name.iso" | cut -f1)), build $build_id"
