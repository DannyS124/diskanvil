#!/bin/sh
# Makes tests/data/ewf/ref.E01: an evidence file made by libewf's own ewfacquire (podman,
# debian:trixie, ewf-tools) from data of our own: some text, then zeros. SHA-256 of that data in
# ref.sha256.
set -e
cd "$(dirname "$0")"
podman run --rm -v "$PWD":/w:Z docker.io/library/debian:trixie sh -c '
set -e
apt-get update -qq >/dev/null && apt-get install -y -qq ewf-tools >/dev/null 2>&1
w=$(mktemp -d) && cd "$w"
python3 -c "import sys; sys.stdout.buffer.write((b\"DiskForge evidence test \" * 4000).ljust(256 * 1024, b\"\\0\"))" > raw.img 2>/dev/null \
  || (yes "DiskForge evidence test" | head -c 96000; head -c 166144 /dev/zero) > raw.img
truncate -s 256K raw.img
ewfacquire -u -t /w/ref -f encase6 -c best -C REF-CASE -D "Made by ewfacquire" -E REF-1 -e libewf -N "test data" -m fixed -M physical -S 0 raw.img >/dev/null
sha256sum raw.img | cut -c1-64 > /w/ref.sha256
'
