#!/bin/bash
# The film for Find Lost Files' tests of putting fragmented files back together: H.264 in MP4,
# with its index after its frames (ffmpeg's way), made by ffmpeg. Only needed when changing it:
#   tests/data/lostfs/fragments/make.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
ffmpeg -v error -y -f lavfi -i mandelbrot=size=320x240:rate=25 -t 6 -c:v libx264 -pix_fmt yuv420p -b:v 400k "$here/film.mp4"
