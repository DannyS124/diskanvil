#!/bin/bash
# DiskForge's Windows installer (.msi), made from the folder build.sh puts together, with
# msitools' wixl (Arch: msitools). Windows Installer does the rest: Program Files, the Start
# menu, Settings > Apps, administrator rights, and replacing an older version.
#   packaging/windows/msi.sh [work folder build.sh used]
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(realpath -m "${1:-${DISKFORGE_WINDOWS_WORK:-$HOME/.cache/diskforge-windows}}")
[[ -f $work/DiskForge/diskforge.exe ]] || { echo "No $work/DiskForge: run build.sh first." >&2; exit 1; }
version=$(sed -n 's/^ *VERSION \([0-9.]*\)$/\1/p' "$root/CMakeLists.txt" | head -1)

msi=$work/msi
rm -rf "$msi"
mkdir -p "$msi/art" "$msi/ext"
# wixl's copy of WiX's installer pages, with DiskForge's pictures where WiX's were.
python3 -I "$root/packaging/windows/installer-art.py" "$root/data/io.github.DannyS124.DiskForge.svg" "$msi/art"
cp -r /usr/share/wixl-*/ext/ui "$msi/ext/"
cp "$msi/art/dialog.bmp" "$msi/ext/ui/bitmaps/dlgbmp.bmp"
cp "$msi/art/banner.bmp" "$msi/ext/ui/bitmaps/bannrbmp.bmp"
# Every file of the program, each a component of its own, with IDs new in each build: an
# upgrade removes the old version first, and components with the same IDs and files of the same
# version (Qt's DLLs don't change between DiskForges) would be skipped, then taken away with it.
(cd "$work" && find DiskForge -type f | LC_ALL=C sort |
    wixl-heat -p DiskForge/ --component-group Files --var var.Source --directory-ref INSTALLFOLDER --win64) |
    python3 -I -c 'import sys, uuid; sys.stdout.write("".join(l.replace("Guid=\"*\"", "Guid=\"%s\"" % uuid.uuid4()) for l in sys.stdin))' >"$msi/files.wxs"
wixl -a x64 --extdir "$msi/ext" --ext ui -D Version="$version" -D Win64=yes -D Source="$work/DiskForge" \
    -D Icon="$root/data/windows/diskforge.ico" \
    "$root/packaging/windows/diskforge.wxs" "$root/packaging/windows/installer-ui.wxs" "$msi/files.wxs" \
    -o "$work/DiskForge-$version.msi"
echo "==> $work/DiskForge-$version.msi"
