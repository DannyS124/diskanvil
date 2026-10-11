#!/bin/bash
# Makes the test disks for virtual machines' disk files (src/lostfiles/vdisk.cpp): one small disk of our
# own (an MBR, a FAT12 partition with two files, and numbered sectors near its end, so the last
# blocks are written to), then that same disk in every format and kind qemu-img writes. The tests
# read each one and expect the disk itself back, byte for byte; the two that only hold changes
# made to another disk are expected to be refused. Kept compressed; the tests unpack them.
#   tests/data/vdisk/make.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d -p "${XDG_CACHE_HOME:-$HOME/.cache}")
trap 'rm -rf "$work"' EXIT
cd "$work"

# 40 MiB: not a whole number of most formats' blocks.
truncate -s 40M disk.raw
echo 'start=2048, size=8192, type=6' | sfdisk -q disk.raw
truncate -s 4M part.img
mkfs.fat -F 12 -n VDISKFAT part.img >/dev/null
printf 'Notes for the virtual disk test.\n' > notes.txt
python3 -I -c 'for i in range(4096): print(f"line {i:06d} of the numbered file, for the virtual disk test.")' > numbers.txt
mcopy -i part.img notes.txt numbers.txt ::/
dd if=part.img of=disk.raw bs=512 seek=2048 conv=notrunc status=none
python3 -I - <<'PY'
with open("disk.raw", "r+b") as f:
    f.seek(39 * 1024 * 1024)
    for s in range(1024):
        f.write(f"sector {39 * 2048 + s:08d} ".encode().ljust(512, b"."))
PY

convert() { qemu-img convert -q -f raw -O "$@"; }
convert vdi disk.raw dynamic.vdi
convert vdi -o static=on disk.raw fixed.vdi
convert vpc -o force_size=on disk.raw dynamic.vhd
convert vpc -o subformat=fixed,force_size=on disk.raw fixed.vhd
convert vhdx disk.raw dynamic.vhdx
convert vhdx -o subformat=fixed disk.raw fixed.vhdx
convert vmdk disk.raw sparse.vmdk
convert vmdk -o subformat=streamOptimized disk.raw stream.vmdk
convert vmdk -o subformat=twoGbMaxExtentSparse disk.raw split.vmdk # and split-s001.vmdk
convert vmdk -o subformat=monolithicFlat disk.raw flat.vmdk        # and flat-flat.vmdk
convert qcow2 disk.raw plain.qcow2
convert qcow2 -o compat=0.10 disk.raw v2.qcow2
convert qcow2 -c disk.raw packed.qcow2
convert qcow2 -c -o compression_type=zstd disk.raw zstd.qcow2
convert qcow2 -o extended_l2=on disk.raw subclusters.qcow2
qemu-img create -q -f qcow2 -b plain.qcow2 -F qcow2 child.qcow2
qemu-img create -q -f vmdk -b sparse.vmdk -F vmdk child.vmdk

# (win-*: made by Windows itself, windows-job.ps1; left as they are.)
for f in disk.raw *.vdi *.vhd *.vhdx *.vmdk *.qcow2; do
    zstd -q -19 -f "$f" -o "$here/$f.zst"
done
sha256sum disk.raw | cut -d' ' -f1 > "$here/disk.sha256"
du -ch "$here"/*.zst | tail -1
