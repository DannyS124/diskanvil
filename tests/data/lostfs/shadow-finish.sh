#!/bin/bash
# The Linux half of ntfs-shadow.img.zst, after shadow-job.cmd: the volume off the disk with
# Windows' boot code blanked out, and shadow-expected.txt from what Windows wrote down about each
# copy: its ID and time, its files' SHA-256, and one digest of its volume's MiBs (the SHA-256 of
# Windows' "mib <n> <bytes> <sha256>" lines, joined). Left out of the digest: the first MiB
# (blanked here), and MiBs where Windows reads back whatever its buffers hold, given as
# copy:mib (blocks that were free when the copy was taken, or that VSS keeps for itself).
#   shadow-finish.sh disk.img job-output.txt 1:106 1:107 2:10
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# Not in /tmp: on most systems that's memory, and the volume is a gigabyte.
work=$(mktemp -d -p "${XDG_CACHE_HOME:-$HOME/.cache}")
trap 'rm -rf "$work"' EXIT

dd if="$1" of="$work/volume.img" bs=1M skip=1 conv=sparse status=none # the partition starts 1 MiB in
python3 -I "$here/blank-boot-code.py" "$work/volume.img"
if strings "$work/volume.img" | grep -qi 'bootmgr\|disk read error'; then
    echo "boot code left somewhere in the volume" >&2
    exit 1
fi
zstd -q -19 -f "$work/volume.img" -o "$here/ntfs-shadow.img.zst"
python3 -I - "$2" "${@:3}" > "$here/shadow-expected.txt" <<'PY'
import hashlib, sys
lines = [l.rstrip("\r\n") for l in open(sys.argv[1], encoding="utf-8", errors="replace")]
skip = {tuple(map(int, s.split(":"))) for s in sys.argv[2:]}
copies = []
for l in lines:
    w = l.split(" ")
    if w[0] == "copy":
        copies.append({"id": w[1], "time": w[2], "files": [], "mibs": []})
    elif w[0] == "file" and copies:
        copies[-1]["files"].append((w[1].replace("\\", "/"), w[2]))
    elif w[0] == "mib" and copies:
        copies[-1]["mibs"].append((int(w[1]), int(w[2]), w[3]))
print("# What Windows read from each shadow copy on ntfs-shadow.img (shadow-finish.sh made this).")
for i, c in enumerate(copies, 1):
    left = [m for m in c["mibs"] if m[0] != 0 and (i, m[0]) not in skip]
    digest = hashlib.sha256("".join(f"mib {m} {n} {h}\n" for m, n, h in left).encode()).hexdigest()
    sizes = c["mibs"][-1]
    out = ",".join(str(m) for m, _, _ in c["mibs"] if m == 0 or (i, m) in skip)
    print(f"copy|{i}|{c['id']}|{c['time']}|{(sizes[0] << 20) + sizes[1]}|{digest}|{out}")
    for path, sha in c["files"]:
        print(f"file|{i}|{path}|{sha}")
PY
ls -la "$here/ntfs-shadow.img.zst" "$here/shadow-expected.txt"
