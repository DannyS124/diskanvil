#!/bin/bash
# Builds the DiskAnvil Live ISO. Needs podman (no root) and about 10 GB free.
# Everything happens in a Debian container; nothing gets installed on this PC.
#
#   rescue/build.sh            build DiskAnvil's .deb, then the ISO, into rescue/out/
#   rescue/build.sh --no-app   reuse the .deb from the last build (faster when only the
#                              rescue files changed)
#
# Downloaded packages are kept in ~/.cache/diskanvil-live, so the next build is quicker.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
version=$(sed -n 's/^ *VERSION \([0-9][0-9.]*\)$/\1/p' "$root/CMakeLists.txt" | head -1)
cache=${XDG_CACHE_HOME:-$HOME/.cache}/diskanvil-live
out=$here/out
mkdir -p "$cache" "$out"

if [ "${1:-}" != --no-app ] || ! ls "$root"/packaging/deb/out/diskanvil_"$version"_amd64.deb >/dev/null 2>&1; then
    echo "==> Building DiskAnvil $version for Debian"
    "$root/packaging/deb/build.sh"
fi

echo "==> Getting the builder ready"
podman build -q -t diskanvil-live-builder -f "$here/Containerfile" "$here" >/dev/null

echo "==> Building the rescue image (the first time downloads about 1.5 GB)"
# --privileged: mmdebstrap needs to mount /proc and /sys inside the new system. It's still
# rootless podman, so "root" in there is just this user.
podman run --rm --privileged \
    -v "$root:/src:ro" -v "$out:/out:Z" -v "$cache:/cache:Z" \
    diskanvil-live-builder bash /src/rescue/make-image.sh "$version"
