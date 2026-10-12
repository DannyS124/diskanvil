#!/bin/bash
# Checks the boot menu's "Other ISOs on this stick" in QEMU, with this PC's GRUB, BIOS and UEFI
# (no Secure Boot): a stick laid out like Make a DiskAnvil Live USB makes it (FAT32 DISKANVIL, then
# an exFAT ANVIL-FILES drive with an ISOs folder), and a tiny ISO of our own whose loopback.cfg
# says what it was given. The menu is the real grub.cfg, set to pick that ISO by itself.
# Needs grub (i386-pc and x86_64-efi), qemu, edk2-ovmf, mtools, exfatprogs, xorriso and udisks2
# (the exFAT drive is filled through a loop device, as the user). Run as the user, not root.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work=${1:-$(mktemp -d)}
mkdir -p "$work"
cd "$work"
id=menutest
ovmf=$(ls /usr/share/edk2/x64/OVMF_CODE.4m.fd /usr/share/edk2-ovmf/x64/OVMF_CODE.fd 2>/dev/null | head -1)

# The ISO: what a Linux ISO made for this has (most have one), printing what it got.
mkdir -p iso/boot/grub
cat > iso/boot/grub/loopback.cfg <<'CFG'
echo "ISOTEST iso_path=$iso_path"
echo "ISOTEST root=$root"
if [ -f /hello.txt ]; then echo "ISOTEST reads inside the ISO"; fi
halt
CFG
echo hello > iso/hello.txt
xorriso -as mkisofs -quiet -R -J -o tiny.iso iso

# The system partition: the menu set to open Other ISOs > tiny.iso, on the serial port too.
mkdir -p sys/.disk sys/boot/grub sys/EFI/BOOT
touch "sys/.disk/diskanvil-live-$id"
sed -e "s/@ID@/$id/g; s/@VERSION@/test/g; s/@MEMTEST@/test/g" \
    -e 's/^set default=0$/set default="Other ISOs on this stick...>tiny.iso"/' \
    -e 's/^set timeout=10$/set timeout=1\nserial --unit=0 --speed=115200\nterminal_output serial console\nterminal_input serial console/' \
    "$here/boot/grub.cfg" > sys/boot/grub/grub.cfg
grep -q 'set default="Other ISOs' sys/boot/grub/grub.cfg
cp -r /usr/lib/grub/i386-pc sys/boot/grub/
cp -r /usr/lib/grub/x86_64-efi sys/boot/grub/
cat > early.cfg <<CFG
search --no-floppy --set=root --file /.disk/diskanvil-live-$id
set prefix=(\$root)/boot/grub
CFG
grub-mkimage -O x86_64-efi -p /boot/grub -c early.cfg -o sys/EFI/BOOT/BOOTX64.EFI part_msdos fat search search_fs_file normal configfile
grub-mkimage -O i386-pc -p /boot/grub -c early.cfg -o core.img biosdisk part_msdos fat search search_fs_file
# 1 MiB in, 300 MiB of FAT32; the rest exFAT.
truncate -s 600M stick.img
printf 'label: dos\nstart=2048, size=614400, type=c, bootable\nstart=616448, type=7\n' | sfdisk -q stick.img
truncate -s 300M sys.img
mkfs.fat -F 32 -n DISKANVIL sys.img >/dev/null
mcopy -s -i sys.img sys/.disk sys/boot sys/EFI ::/
dd if=sys.img of=stick.img bs=1M seek=1 conv=notrunc status=none
truncate -s $((600 - 301))M files.img
mkfs.exfat -L ANVIL-FILES files.img >/dev/null
loop=$(udisksctl loop-setup --no-user-interaction -f files.img | sed -n 's/.* as \(\/dev\/loop[0-9]*\)\.$/\1/p')
mnt=$(udisksctl mount --no-user-interaction -b "$loop" | sed -n 's/^Mounted .* at \(.*\)$/\1/p')
mkdir -p "$mnt/ISOs"
cp tiny.iso "$mnt/ISOs/"
udisksctl unmount --no-user-interaction -b "$loop" >/dev/null
udisksctl loop-delete --no-user-interaction -b "$loop"
dd if=files.img of=stick.img bs=512 seek=616448 conv=notrunc status=none
# BIOS boot code in front of the partition, as Make a DiskAnvil Live USB writes it.
dd if=/usr/lib/grub/i386-pc/boot.img of=stick.img bs=440 count=1 conv=notrunc status=none
dd if=core.img of=stick.img bs=512 seek=1 conv=notrunc status=none

result=0
check() {
    local how=$1 out=$2
    if grep -q "ISOTEST iso_path=/ISOs/tiny.iso" "$out" && grep -q "ISOTEST root=loop" "$out" && grep -q "ISOTEST reads inside the ISO" "$out"; then
        echo "PASS  $how: Other ISOs found tiny.iso on ANVIL-FILES and started it with iso_path=/ISOs/tiny.iso"
    else
        echo "FAIL  $how: $(tr -d '\r' < "$out" | grep -a "ISOTEST\|error" | head -5)"
        result=1
    fi
}
timeout 90 qemu-system-x86_64 -m 512 -drive file=stick.img,format=raw -display none -serial file:bios.txt -no-reboot || true
check BIOS bios.txt
if [ -n "$ovmf" ]; then
    cp "${ovmf/CODE/VARS}" vars.fd 2>/dev/null || cp /usr/share/edk2/x64/OVMF_VARS.4m.fd vars.fd
    timeout 90 qemu-system-x86_64 -m 512 -machine q35 -drive if=pflash,format=raw,readonly=on,file="$ovmf" -drive if=pflash,format=raw,file=vars.fd \
        -drive file=stick.img,format=raw -display none -serial file:uefi.txt -no-reboot || true
    check UEFI uefi.txt
fi
exit $result
