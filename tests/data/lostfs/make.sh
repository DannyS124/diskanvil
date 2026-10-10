#!/bin/bash
# Makes the test images for Find Lost Files' FAT, exFAT and NTFS readers: each file system gets
# pictures, a document and text, in folders and with long names, put there by Linux's own
# drivers (mounted, so needs sudo), and then some of them deleted, a whole folder too.
# expected.txt lists every file: where it was, whether it was deleted, its size and SHA-256.
# The images are kept compressed; the tests unpack them. Only needed when changing them:
#   tests/data/lostfs/make.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
mnt="$work/mnt"
mkdir -p "$mnt" "$work/files"
trap 'mountpoint -q "$mnt" && sudo umount "$mnt"; rm -rf "$work"' EXIT

# Pictures of random dots (they don't pack small, so they span several clusters), always the same.
python3 -I - "$work/files" <<'PY'
import random, sys
from PIL import Image
out = sys.argv[1]
def dots(path, w, h, seed, fmt):
    rnd = random.Random(seed)
    img = Image.new("RGB", (w, h))
    img.putdata([(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(w * h)])
    img.save(path, fmt, **({"quality": 90} if fmt == "JPEG" else {}))
dots(f"{out}/photo.jpg", 64, 48, 1, "JPEG")
dots(f"{out}/holiday.jpg", 96, 64, 2, "JPEG")
dots(f"{out}/pic1.png", 48, 48, 3, "PNG")
dots(f"{out}/pic2.jpg", 80, 60, 4, "JPEG")
dots(f"{out}/scan.png", 40, 40, 5, "PNG")
dots(f"{out}/kitten.jpg", 72, 72, 6, "JPEG")
PY
# A small PDF, the same one the carving tests make.
printf '%s\n' '%PDF-1.4' '1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj' \
    '2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj' \
    '3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >> endobj' \
    'trailer << /Root 1 0 R >>' '%%EOF' > "$work/files/letter.pdf"
printf 'Things to remember.\nThe stick is in the drawer.\n' > "$work/files/notes.txt"
printf 'An old-style 8.3 name: FAT keeps no long name for it.\n' > "$work/files/readme.txt"

make() { # name, size in MiB, mount type, mkfs command...
    local name=$1 size=$2 type=$3
    shift 3
    local img="$work/$name.img"
    truncate -s "${size}M" "$img"
    "$@" "$img" >/dev/null
    sudo mount -t "$type" -o loop,uid="$(id -u)",gid="$(id -g)" "$img" "$mnt"
    # What's put on it, as "path|file" pairs.
    local -a put=(
        "photo.jpg|photo.jpg"
        "Holiday photo with a long name.jpg|holiday.jpg"
        "notes.txt|notes.txt"
        "README.TXT|readme.txt"
        "Pictures/2024/pic1.png|pic1.png"
        "Pictures/2024/pic2.jpg|pic2.jpg"
        "Old stuff/letter.pdf|letter.pdf"
        "Old stuff/scan.png|scan.png"
    )
    for p in "${put[@]}"; do
        mkdir -p "$mnt/$(dirname "${p%%|*}")"
        cp "$work/files/${p##*|}" "$mnt/${p%%|*}"
    done
    if [[ $name == ntfs* ]]; then
        # Windows' Recycle Bin: the file as $R..., and $I... saying where it was (version 2).
        bin="$mnt/\$Recycle.Bin/S-1-5-21-1000"
        mkdir -p "$bin"
        cp "$work/files/kitten.jpg" "$bin/\$RK4T7Q2.jpg"
        python3 -I - "$bin/\$IK4T7Q2.jpg" "$(stat -c %s "$work/files/kitten.jpg")" <<'PY'
import struct, sys
path = "C:\\Users\\Test\\Pictures\\kitten.jpg\0"
when = 133700000000000000  # 2024-09-03
data = struct.pack("<QQQI", 2, int(sys.argv[2]), when, len(path)) + path.encode("utf-16-le")
open(sys.argv[1], "wb").write(data)
PY
    fi
    sync
    rm "$mnt/Holiday photo with a long name.jpg" "$mnt/notes.txt" "$mnt/README.TXT" "$mnt/Pictures/2024/pic1.png"
    rm -r "$mnt/Old stuff"
    sync
    sudo umount "$mnt"
    {
        for p in "${put[@]}"; do
            local path=${p%%|*} file=$work/files/${p##*|} gone=no
            case $path in "Holiday photo"*|notes.txt|README.TXT|Pictures/2024/pic1.png|"Old stuff/"*) gone=yes ;; esac
            printf '%s|%s|%s|%s|%s\n' "$name" "$path" "$gone" "$(stat -c %s "$file")" "$(sha256sum < "$file" | cut -d' ' -f1)"
        done
        if [[ $name == ntfs* ]]; then
            file=$work/files/kitten.jpg
            printf '%s|%s|%s|%s|%s\n' "$name" "Recycle Bin/kitten.jpg" yes "$(stat -c %s "$file")" "$(sha256sum < "$file" | cut -d' ' -f1)"
        fi
    } >> "$work/expected.txt"
    zstd -q -19 -f "$img" -o "$here/$name.img.zst"
}

make fat12 4 vfat mkfs.vfat -F 12 -s 4 -n TESTFAT12
make fat16 16 vfat mkfs.vfat -F 16 -s 2 -n TESTFAT16
make fat32 40 vfat mkfs.vfat -F 32 -s 1 -n TESTFAT32
make exfat 16 exfat mkfs.exfat -L TESTEXFAT
make ntfs 16 ntfs-3g mkntfs -q -F -Q -L TESTNTFS
# Linux's own NTFS driver deletes differently: it takes the names away.
make ntfs-linux 16 ntfs3 mkntfs -q -F -Q -L TESTNTFS3
cp "$work/expected.txt" "$here/expected.txt"
ls -la "$here"
