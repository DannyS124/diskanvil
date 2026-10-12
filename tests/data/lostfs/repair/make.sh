#!/bin/bash
# Films for the test of rebuilding a lost index (mp4fix.h), made by ffmpeg the way cameras store
# them: each kind twice, a longer one whose index the test cuts off and a short one to borrow
# from, with the same settings (as one camera would use) but other pictures.
# Only needed when changing them:
#   tests/data/lostfs/repair/make.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
ff() { ffmpeg -nostdin -hide_banner -loglevel error -y "$@"; }
video() { # name, duration, source, codec options..., then the container's options
    local name=$1 seconds=$2 source=$3
    shift 3
    ff -f lavfi -i "$source=size=128x96:rate=15:duration=$seconds" -f lavfi -i "sine=frequency=440:duration=$seconds" -pix_fmt yuv420p "$@" "$here/$name"
}
# H.264 with PCM sound, in a MOV (as many cameras write them), no reordered frames.
video h264-pcm.mov 4 testsrc -c:v libx264 -preset veryfast -bf 0 -g 15 -c:a pcm_s16le -ar 8000
video h264-pcm-ref.mov 2 testsrc2 -c:v libx264 -preset veryfast -bf 0 -g 15 -c:a pcm_s16le -ar 8000
# H.264 with AAC sound in an MP4 (as phones write them), with reordered frames.
video h264-aac.mp4 4 testsrc -c:v libx264 -preset veryfast -g 15 -c:a aac -b:a 64k
video h264-aac-ref.mp4 2 testsrc2 -c:v libx264 -preset veryfast -g 15 -c:a aac -b:a 64k
# Motion JPEG with PCM sound (older cameras).
video mjpeg.mov 3 testsrc -c:v mjpeg -q:v 5 -c:a pcm_s16le -ar 8000
video mjpeg-ref.mov 2 testsrc2 -c:v mjpeg -q:v 5 -c:a pcm_s16le -ar 8000
# H.265, when this ffmpeg has it.
encoders=$(ffmpeg -hide_banner -encoders 2>/dev/null)
if grep -q ' libx265 ' <<<"$encoders"; then
    video h265.mp4 4 testsrc -c:v libx265 -preset ultrafast -x265-params bframes=0:log-level=error -tag:v hvc1 -an
    video h265-ref.mp4 2 testsrc2 -c:v libx265 -preset ultrafast -x265-params bframes=0:log-level=error -tag:v hvc1 -an
fi
ls -la "$here"
# A PDF whose objects are packed in object streams, with a cross-reference stream (as Word and
# newer programs write them), made by Ghostscript.
printf '%%!PS\n/Helvetica findfont 24 scalefont setfont\n72 720 moveto (Packed objects, found again.) show\nshowpage\n72 720 moveto (Page two.) show\nshowpage\n' |
    gs -q -dNOPAUSE -dBATCH -sDEVICE=pdfwrite -dCompatibilityLevel=1.7 -sOutputFile="$here/packed.pdf" -
ls -la "$here/packed.pdf"
