#!/bin/bash
# Checks the boot menu's "Start this PC's own system" in QEMU, with this PC's GRUB, BIOS and UEFI
# (no Secure Boot): the stick first, then a PC drive whose own start-up (a tiny GRUB of our own,
# standing in for Windows Boot Manager on UEFI and for a drive's boot code on BIOS) says it was
# started. The stick's own start-up files must be passed over. The menu is the real grub.cfg, set
# to pick that entry by itself. Needs grub (i386-pc and x86_64-efi), qemu, edk2-ovmf, mtools.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work=${1:-$(mktemp -d)}
mkdir -p "$work"
cd "$work"
id=owntest
ovmf=$(ls /usr/share/edk2/x64/OVMF_CODE.4m.fd /usr/share/edk2-ovmf/x64/OVMF_CODE.fd 2>/dev/null | head -1)

# The stick, as in check-isos-menu.sh, with the menu set to start the PC's own system.
mkdir -p sys/.disk sys/boot/grub sys/EFI/BOOT
touch "sys/.disk/diskanvil-live-$id"
sed -e "s/@ID@/$id/g; s/@VERSION@/test/g; s/@MEMTEST@/test/g" \
    -e "s/^set default=0\$/set default=\"Start this PC's own system (Windows, Linux...)\"/" \
    -e 's/^set timeout=10$/set timeout=1\nserial --unit=0 --speed=115200\nterminal_output serial console\nterminal_input serial console/' \
    "$here/boot/grub.cfg" > sys/boot/grub/grub.cfg
grep -q "set default=\"Start this PC" sys/boot/grub/grub.cfg
cp -r /usr/lib/grub/i386-pc sys/boot/grub/
cp -r /usr/lib/grub/x86_64-efi sys/boot/grub/
cat > early.cfg <<CFG
search --no-floppy --set=root --file /.disk/diskanvil-live-$id
set prefix=(\$root)/boot/grub
CFG
grub-mkimage -O x86_64-efi -p /boot/grub -c early.cfg -o sys/EFI/BOOT/BOOTX64.EFI part_msdos fat search search_fs_file normal configfile
grub-mkimage -O i386-pc -p /boot/grub -c early.cfg -o core.img biosdisk part_msdos fat search search_fs_file
truncate -s 300M stick.img
printf 'label: dos\nstart=2048, type=c, bootable\n' | sfdisk -q stick.img
truncate -s 299M sys.img
mkfs.fat -F 32 -n DISKANVIL sys.img >/dev/null
mcopy -s -i sys.img sys/.disk sys/boot sys/EFI ::/
dd if=sys.img of=stick.img bs=1M seek=1 conv=notrunc status=none
dd if=/usr/lib/grub/i386-pc/boot.img of=stick.img bs=440 count=1 conv=notrunc status=none
dd if=core.img of=stick.img bs=512 seek=1 conv=notrunc status=none

# The PC's own drive: what starts says so on the serial port, and stops.
cat > own.cfg <<'CFG'
serial --unit=0 --speed=115200
terminal_output serial console
echo "OWNTEST the PC's own system started"
halt
CFG
# UEFI: GPT with a FAT system partition, "Windows Boot Manager" in its usual place.
truncate -s 64M pc-uefi.img
printf 'label: gpt\nstart=2048, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B\n' | sfdisk -q pc-uefi.img
truncate -s 63M esp.img
mkfs.fat -F 32 esp.img >/dev/null
mkdir -p esp/EFI/Microsoft/Boot
grub-mkimage -O x86_64-efi -p /none -c own.cfg -o esp/EFI/Microsoft/Boot/bootmgfw.efi normal serial terminal echo halt
mcopy -s -i esp.img esp/EFI ::/
dd if=esp.img of=pc-uefi.img bs=1M seek=1 conv=notrunc status=none
# BIOS: an MBR drive whose boot code starts its own.
truncate -s 16M pc-bios.img
printf 'label: dos\nstart=2048, type=83\n' | sfdisk -q pc-bios.img
grub-mkimage -O i386-pc -p /none -c own.cfg -o own-core.img biosdisk serial terminal echo halt
dd if=/usr/lib/grub/i386-pc/boot.img of=pc-bios.img bs=440 count=1 conv=notrunc status=none
dd if=own-core.img of=pc-bios.img bs=512 seek=1 conv=notrunc status=none

result=0
check() {
    if grep -q "OWNTEST the PC's own system started" "$2"; then
        echo "PASS  $1: the stick passed over, the PC's own system started"
    else
        echo "FAIL  $1: $(tr -d '\r' < "$2" | grep -a "OWNTEST\|error\|No other" | head -5)"
        result=1
    fi
}
timeout 90 qemu-system-x86_64 -m 512 -drive file=stick.img,format=raw -drive file=pc-bios.img,format=raw -display none -serial file:bios.txt -no-reboot || true
check BIOS bios.txt
if [ -n "$ovmf" ]; then
    cp /usr/share/edk2/x64/OVMF_VARS.4m.fd vars.fd
    timeout 90 qemu-system-x86_64 -m 512 -machine q35 -drive if=pflash,format=raw,readonly=on,file="$ovmf" -drive if=pflash,format=raw,file=vars.fd \
        -drive file=stick.img,format=raw -drive file=pc-uefi.img,format=raw -display none -serial file:uefi.txt -no-reboot || true
    check UEFI uefi.txt
fi
exit $result
