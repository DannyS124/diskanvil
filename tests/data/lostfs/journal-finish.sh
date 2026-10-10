#!/bin/bash
# The Linux half of ntfs-journal.img.zst, after journal-job.cmd: the volume off the stick with
# Windows' boot code blanked out, then keep.txt deleted by Linux's own NTFS driver, which takes
# the names away (the change journal still knows it). Mounts, so needs sudo.
#   journal-finish.sh stick.img
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
mnt="$work/mnt"
mkdir "$mnt"
trap 'mountpoint -q "$mnt" && sudo umount "$mnt"; rm -rf "$work"' EXIT

dd if="$1" of="$work/volume.img" bs=1M skip=1 status=none # the partition starts 1 MiB in
python3 -I "$here/blank-boot-code.py" "$work/volume.img"
sudo mount -t ntfs3 -o loop "$work/volume.img" "$mnt"
sudo rm "$mnt/keep.txt"
sudo umount "$mnt"
zstd -q -19 -f "$work/volume.img" -o "$here/ntfs-journal.img.zst"
ls -la "$here/ntfs-journal.img.zst"
