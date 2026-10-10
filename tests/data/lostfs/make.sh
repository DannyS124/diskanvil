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

python3 -I "$here/files.py" "$work/files"

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
# ntfs-windows.img.zst is made by Windows itself (windows-job.cmd, then blank-boot-code.py), so
# it's kept as it is.
# ntfs-journal.img.zst too (journal-job.cmd, then journal-finish.sh).
grep -E '^ntfs-(windows|journal)\|' "$here/expected.txt" >> "$work/expected.txt" || true
cp "$work/expected.txt" "$here/expected.txt"
ls -la "$here"
