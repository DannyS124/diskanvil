#!/bin/bash
# Makes the files for Find Lost Files' tests of the kinds added in 0.5.2, each made by the
# program that makes such files for real, so the readers are checked against them:
# - LibreOffice turns office.py's flat OpenDocument files into Word, Excel and PowerPoint 97-2003
#   files. It runs in a throwaway Debian container (podman), so nothing is installed here. The
#   Outlook email is office.py's own (LibreOffice can't write those); olefile, an unrelated
#   reader, lists every compound file in the container, as a check.
# - ffmpeg makes the films and songs (Matroska, WebM, FLAC, Windows Media) with tags and covers.
# - Python's PIL makes the TIFF; gzip and tar the archives.
# Only needed when changing them:
#   tests/data/lostfs/types/make.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$here/office.py" "$work/"

podman run --rm -v "$work:/work:Z" docker.io/library/debian:trixie bash -c '
    set -e
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
        libreoffice-writer-nogui libreoffice-calc-nogui libreoffice-impress-nogui python3-olefile >/dev/null
    cd /work
    python3 -I office.py /work
    soffice --headless --convert-to "doc:MS Word 97" notes.fodt >/dev/null
    soffice --headless --convert-to "xls:MS Excel 97" budget.fods >/dev/null
    soffice --headless --convert-to "ppt:MS PowerPoint 97" slides.fodp >/dev/null
    python3 -I office.py /work macros
    for f in notes.doc budget.xls slides.ppt ferry.msg macros.doc; do
        python3 -I -c "import olefile, sys; o = olefile.OleFileIO(sys.argv[1]); print(sys.argv[1], sorted(\"/\".join(p) for p in o.listdir()))" "$f"
    done
    chown '"$(id -u):$(id -g)"' /work/*'
cp "$work"/{notes.doc,ferry.msg,macros.doc} "$here/"
# These two are big for what they hold (LibreOffice puts a picture in every PowerPoint file).
zstd -q -19 -f "$work/budget.xls" -o "$here/budget.xls.zst"
zstd -q -19 -f "$work/slides.ppt" -o "$here/slides.ppt.zst"

# A cover for the songs and films, and a TIFF.
python3 -I - "$work" <<'PY'
import sys
from PIL import Image, ImageDraw
img = Image.new("RGB", (96, 64), (6, 10, 22))
d = ImageDraw.Draw(img)
d.ellipse((10, 10, 50, 50), fill=(255, 51, 85))
d.rectangle((56, 16, 88, 48), fill=(43, 238, 138))
img.save(sys.argv[1] + "/cover.jpg", quality=85)
img.save(sys.argv[1] + "/picture.tif", compression="tiff_lzw")
PY
cp "$work/picture.tif" "$here/"

ff() { ffmpeg -nostdin -hide_banner -loglevel error -y "$@"; }
tags=(-metadata title="Lake trip" -metadata artist="Sam Tester" -metadata album="Summer" -metadata date=2024)
ff -f lavfi -i testsrc=size=64x48:rate=10:duration=2 -f lavfi -i sine=frequency=440:duration=2 -c:v mjpeg -c:a flac \
    "${tags[@]}" -metadata creation_time=2024-07-14T13:22:05Z -attach "$work/cover.jpg" -metadata:s:t mimetype=image/jpeg \
    "$here/trip.mkv"
ff -f lavfi -i testsrc=size=64x48:rate=10:duration=1 -f lavfi -i sine=frequency=440:duration=1 -c:v libvpx-vp9 -b:v 50k \
    -c:a libopus "${tags[@]}" "$here/trip.webm"
ff -f lavfi -i sine=frequency=440:duration=1.5 -i "$work/cover.jpg" -map 0 -map 1 -c:a flac -c:v copy -disposition:v attached_pic \
    "${tags[@]}" "$here/song.flac"
ff -f lavfi -i testsrc=size=64x48:rate=10:duration=2 -f lavfi -i sine=frequency=440:duration=2 -c:v wmv2 -c:a wmav2 \
    "${tags[@]}" "$here/trip.wmv"
ff -f lavfi -i sine=frequency=440:duration=1.5 -c:a wmav2 "${tags[@]}" "$here/song.wma"

printf 'Pack the blue tent.\n' > "$work/notes.txt"
gzip -9 -c "$work/notes.txt" > "$here/notes.txt.gz" # keeps the name: notes.txt
tar -C "$work" -czf "$here/pictures.tar.gz" cover.jpg picture.tif
ls -la "$here"
