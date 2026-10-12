#!/bin/bash
# Builds the .deb in a clean Debian container and runs the test suites that don't need
# root or a running UDisks. Needs podman (no root). The package ends up in packaging/deb/out/.
#   packaging/deb/build.sh [debian release, default trixie]
set -euo pipefail
release=${1:-trixie}
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/packaging/deb/out"
mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# The source as it is now: committed or not, new files too, minus what .gitignore leaves out
# (and minus other git checkouts inside this one, which git lists as just a folder name).
git -C "$root" ls-files -z --cached --others --exclude-standard | grep -zv '/$' |
    tar -C "$root" --null -T - -cf "$work/src.tar"

cat > "$work/inside.sh" <<'INSIDE'
#!/bin/bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends build-essential cmake ninja-build pkg-config file dpkg-dev \
    qt6-base-dev qt6-base-dev-tools qt6-tools-dev qt6-tools-dev-tools qt6-l10n-tools qt6-svg-plugins \
    libzstd-dev zlib1g-dev libssl-dev libblkid-dev libarchive-dev libpoppler-qt6-dev qt6-multimedia-dev qt6-image-formats-plugins sqlite3 clamav \
    fdisk e2fsprogs dosfstools mtools btrfs-progs cryptsetup-bin systemd ffmpeg zstd xorriso >/dev/null
echo "Debian $(cat /etc/debian_version), Qt $(dpkg-query -W -f='${Version}' qt6-base-dev), gcc $(gcc -dumpversion)"
mkdir -p /src && tar -xf /work/src.tar -C /src
cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr >/build.log 2>&1 \
    || { tail -30 /build.log; exit 1; }
cmake --build /build >>/build.log 2>&1 || { grep -E "error" /build.log | head -30; exit 1; }
echo "build OK, $(grep -c 'warning:' /build.log) warnings"
failed=0
for suite in addons gpt copy usage backup rescuemap catalog fuzz health jobs recover rescueusb stickcheck images isomode windows lostfiles viruses registry startup boot newids fat32 luks savefiles clonezilla; do
    if QT_QPA_PLATFORM=offscreen /build/diskanvil-selftest --$suite >/tmp/$suite.log 2>&1; then
        echo "  $suite: passed"
    else
        echo "  $suite: FAILED"; grep -E "^FAIL" /tmp/$suite.log | head -5; failed=1
    fi
done
(cd /build && cpack -G DEB >/tmp/cpack.log 2>&1) || { cat /tmp/cpack.log; exit 1; }
# The one just made (out/ can still hold older ones).
deb=$(ls /build/*.deb | head -1)
cp "$deb" /out/
dpkg-deb -f "$deb" Package Version Depends | sed 's/^/  /'
exit $failed
INSIDE
chmod +x "$work/inside.sh"
podman run --rm -v "$work:/work:Z" -v "$out:/out:Z" "docker.io/library/debian:$release" /work/inside.sh
echo "==> $(ls -t "$out"/*.deb | head -1)"
