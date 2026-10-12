#!/bin/sh
# Makes tests/data/clonezilla: a Clonezilla-style folder of partclone images of our own small file
# systems (an ext4 split in gzip parts, a FAT32 in zstd, an ext4 without checksums), with the
# SHA-256 of each file system as it was. Run from here; uses podman (debian:trixie, partclone).
set -e
cd "$(dirname "$0")"
podman run --rm -v "$PWD":/work:Z docker.io/library/debian:trixie sh -c '
set -e
apt-get update -qq >/dev/null && apt-get install -y -qq partclone e2fsprogs dosfstools mtools zstd gzip >/dev/null 2>&1
w=$(mktemp -d) && cd "$w"
mkdir -p tree/docs && printf "partclone test\n" > tree/hello.txt && head -c 60000 /dev/urandom > tree/docs/random.bin
truncate -s 8M ext4.img && mkfs.ext4 -q -d tree -L PCTEST ext4.img
truncate -s 36M fat.img && mkfs.fat -F 32 -n PCFAT fat.img >/dev/null && mcopy -i fat.img tree/hello.txt ::/hello.txt && mcopy -s -i fat.img tree/docs ::/docs
partclone.ext4 -q -c -s ext4.img -O ext4.ptcl 2>/dev/null
partclone.fat32 -q -c -s fat.img -O fat.ptcl 2>/dev/null
partclone.ext4 -q -c -a0 -s ext4.img -O nocrc.ptcl 2>/dev/null
mkdir -p /work/image
gzip -c ext4.ptcl | split -b 40k - /work/image/sda1.ext4-ptcl-img.gz.
zstd -q -c fat.ptcl > /work/image/sda2.vfat-ptcl-img.zst
gzip -c nocrc.ptcl > /work/image/sdb1.ext4-ptcl-img.gz
printf "sda1 sda2 sdb1\n" > /work/image/parts
sha256sum ext4.img fat.img > /work/sha256.txt
'
