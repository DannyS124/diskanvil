#!/bin/bash
# The Linux half of wof.img.zst, after job.cmd: the volume off the disk, with Windows' boot code
# blanked out.
#   wof-finish.sh disk.img
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d -p "${XDG_CACHE_HOME:-$HOME/.cache}")
trap 'rm -rf "$work"' EXIT

start=$(sfdisk -d "$1" | sed -n 's/.*start= *\([0-9]*\),.*/\1/p' | head -1)
size=$(sfdisk -d "$1" | sed -n 's/.*size= *\([0-9]*\),.*/\1/p' | head -1)
dd if="$1" of="$work/volume.img" bs=512 skip="$start" count="$size" status=none
python3 -I "$here/../lostfs/blank-boot-code.py" "$work/volume.img"
if strings "$work/volume.img" | grep -qi 'bootmgr\|disk read error'; then
    echo "boot code left somewhere in the volume" >&2
    exit 1
fi
zstd -q -19 -f "$work/volume.img" -o "$here/wof.img.zst"
ls -la "$here/wof.img.zst"
