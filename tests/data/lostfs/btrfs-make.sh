#!/bin/bash
# Makes btrfs.img.zst for the btrfs restore test: a btrfs file system with photos and notes,
# written in one commit, then some of them deleted in the next, so the older tree still lists
# them. Mounted, so needs sudo. Only needed when changing it:
#   tests/data/lostfs/btrfs-make.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d -p "$HOME/Projects/diskforge-work")
mnt="$work/mnt"
mkdir "$mnt"
trap 'mountpoint -q "$mnt" && sudo umount "$mnt"; rm -rf "$work"' EXIT
truncate -s 128M "$work/btrfs.img"
mkfs.btrfs -q -L Photos "$work/btrfs.img"
sudo mount -o loop,commit=1 "$work/btrfs.img" "$mnt"
sudo chown "$(id -u):$(id -g)" "$mnt"
mkdir -p "$mnt/Pictures/2024" "$mnt/Notes"
python3 -I - "$mnt" <<'PY'
import sys, random
random.seed(7)
root = sys.argv[1]
for i in range(1, 6):
    with open(f"{root}/Pictures/2024/beach{i}.jpg", "wb") as f:
        f.write(b"\xff\xd8\xff\xe0" + bytes(random.getrandbits(8) for _ in range(20000 + i * 1000)) + b"\xff\xd9")
for name, text in [("shopping.txt", "Milk, bread, tent pegs.\n"), ("ferry.txt", "Pier 3 at 07:30.\n")]:
    with open(f"{root}/Notes/{name}", "w") as f:
        f.write(text * 50)
PY
sync
sudo btrfs filesystem sync "$mnt"
sleep 2
rm "$mnt/Pictures/2024/beach2.jpg" "$mnt/Pictures/2024/beach4.jpg" "$mnt/Notes/ferry.txt"
sync
sudo btrfs filesystem sync "$mnt"
sleep 2
sudo umount "$mnt"
zstd -q -19 -f "$work/btrfs.img" -o "$here/btrfs.img.zst"
ls -la "$here/btrfs.img.zst"
