#!/bin/bash
# Makes the files for Find Lost Files' tests of the kinds added in 0.5.3, each by a program that
# makes such files for real: ffmpeg the films and sounds, ImageMagick the icon and the JPEG 2000,
# xz, bzip2, zstd and tar the archives, gcab the cabinet, the compilers a Windows and a Linux
# program, Python the MIDI tune and the JSON. All small, and all ours.
# Only needed when changing them:
#   tests/data/lostfs/types053/make.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"

ff() { ffmpeg -v error -y "$@"; }
ff -f lavfi -i testsrc2=size=160x120:rate=10 -f lavfi -i sine=frequency=440 -t 1 -c:v flv1 -shortest "$here/clip.flv"
ff -f lavfi -i testsrc2=size=160x120:rate=10 -f lavfi -i sine=frequency=440 -t 1 -c:v mpeg2video -c:a mp2 -shortest -f mpegts "$here/clip.ts"
ff -f lavfi -i testsrc2=size=160x120:rate=10 -f lavfi -i sine=frequency=440 -t 1 -c:v mpeg2video -c:a mp2 -shortest -f mpeg "$here/clip.mpg"
ff -f lavfi -i testsrc2=size=160x120:rate=10 -t 1 -c:v flv1 -f swf "$here/clip.swf"
ff -f lavfi -i testsrc2=size=160x112:rate=10 -f lavfi -i sine=frequency=440 -t 1 -c:v rv10 -c:a ac3 -shortest -f rm "$here/clip.rm"
ff -f lavfi -i sine=frequency=330:duration=1 "$here/tone.aiff"
ff -f lavfi -i sine=frequency=330:duration=1 -ar 8000 -ac 1 -c:a libopencore_amrnb "$here/voice.amr"
ff -f lavfi -i sine=frequency=330:duration=1 -c:a ac3 "$here/tone.ac3"
ff -f lavfi -i sine=frequency=330:duration=1 -c:a wavpack "$here/tone.wv"
ff -f lavfi -i sine=frequency=330:duration=1 "$here/tone.caf"
ff -f lavfi -i sine=frequency=330:duration=1 -c:a aac -f adts "$here/tone.aac"
magick -size 64x64 plasma:fractal -seed 3 -define icon:auto-resize=64,32,16 "$here/app.ico"
magick -size 96x64 plasma:fractal -seed 4 "$here/scan.jp2"

python3 -I - "$here" <<'PY'
import json, struct, sys
here = sys.argv[1]
# A tune: the header chunk, then one track of a few notes.
events = b''
for note in (60, 62, 64, 65, 67):
    events += bytes([0x00, 0x90, note, 0x60, 0x40, 0x80, note, 0x00])
events += bytes([0x00, 0xFF, 0x2F, 0x00])
midi = b'MThd' + struct.pack('>IHHH', 6, 0, 1, 96) + b'MTrk' + struct.pack('>I', len(events)) + events
open(f'{here}/tune.mid', 'wb').write(midi)
json.dump({'trip': 'ferry', 'days': [{'day': d, 'note': 'walk along the shore'} for d in range(40)]},
          open(f'{here}/plan.json', 'w'), indent=2)
PY

printf 'notes for the tar and the archives\n%.0s' {1..200} > notes.txt
cp "$here/app.ico" .
xz -c notes.txt > "$here/notes.txt.xz"
bzip2 -c notes.txt > "$here/notes.txt.bz2"
zstd -q -c notes.txt > "$here/notes.txt.zst"
tar --owner=0 --group=0 --mtime=2026-10-26 -cf "$here/bundle.tar" notes.txt app.ico
gcab -c "$here/bundle.cab" notes.txt app.ico

printf 'int main(void) { return 0; }\n' > hello.c
x86_64-w64-mingw32-gcc -Os -s hello.c -o "$here/hello.exe"
gcc -Os -s hello.c -o "$here/hello.elf"
echo "made: $(ls "$here" | grep -v make.sh | tr '\n' ' ')"
