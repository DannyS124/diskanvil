#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Danny S
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Nothing goes on GitHub until you've installed and tried the exact package.
#
#   packaging/release.sh --stage 0.4.3     test and build 0.4.3 on this PC, ready to install and try
#   packaging/release.sh --publish 0.4.3   tried it and it's fine: upload exactly what was staged
#   packaging/release.sh --pull 0.4.3      a bad release got out: delete it, PKGBUILD goes back to the previous one
#   packaging/release.sh --aur 0.4.3       push the current PKGBUILD to the AUR (after a packaging-only fix)
#
# The AUR repo is expected at ../diskforge-aur (git clone ssh://aur@aur.archlinux.org/diskforge.git).
#
# Never reuse a version number once it's been on GitHub.
set -euo pipefail

repo=DannyS124/diskforge
root="$(cd "$(dirname "$0")/.." && pwd)"
pkgbuild="$root/packaging/arch/PKGBUILD"
cd "$root"

die() { echo "error: $*" >&2; exit 1; }
pkgver_of() { sed -n 's/^pkgver=//p' "$1"; }
on_github() { git ls-remote --exit-code --tags origin "refs/tags/v$1" >/dev/null 2>&1; }
release_hash() { gh release download "$1" -R "$repo" -p SHA256SUMS -O - | cut -d' ' -f1; }
aur="$(dirname "$root")/diskforge-aur"
sync_aur() { # message
    [[ -d "$aur/.git" ]] || { echo "(no AUR clone at $aur, skipping the AUR)"; return 0; }
    cp "$pkgbuild" "$aur/PKGBUILD"
    (cd "$aur" && makepkg --printsrcinfo > .SRCINFO && git add PKGBUILD .SRCINFO \
        && { git diff --cached --quiet || { git commit -qm "$1" && git push -q origin master; }; })
    echo "==> AUR updated: https://aur.archlinux.org/packages/diskforge"
}
set_pkgbuild() { # file version hash
    sed -i "s/^pkgver=.*/pkgver=$2/; s/^pkgrel=.*/pkgrel=1/; s/^sha256sums=.*/sha256sums=('$3')/" "$1"
}

cmd="${1:-}"
v="${2:-}"
[[ -n "$v" ]] || die "usage: release.sh --stage|--publish|--pull|--aur <version>"
stage="$root/packaging/staging/$v"
rescue_iso="diskforge-live-$v.iso"
tarball="diskforge-$v.tar.gz"
pkg="diskforge-$v-1-$(uname -m).pkg.tar.zst"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

case "$cmd" in
--stage)
    [[ -z "$(git status --porcelain)" ]] || die "commit or stash your changes first"
    grep -q "VERSION $v\$" CMakeLists.txt || die "set VERSION $v in CMakeLists.txt first"
    grep -q "release version=\"$v\"" data/io.github.DannyS124.DiskForge.metainfo.xml.in || die "add a <release version=\"$v\"> to the metainfo first"
    grep -q "^## $v\$" CHANGELOG.md || die "add a '## $v' section to CHANGELOG.md first"
    ! on_github "$v" || die "v$v is already on GitHub, use a new version"

    # DiskForge never takes orders from other programs: no sockets, no D-Bus service of its
    # own, nothing listening. Keep it that way.
    if grep -rnE 'QLocalServer|QTcpServer|QUdpSocket|QWebSocketServer|QSctpServer|registerService|registerObject|QDBusAbstractAdaptor|\blisten\(' src; then
        die "something in src/ listens for other programs (above); DiskForge mustn't"
    fi
    # Invisible characters (zero-width, text direction) can make code read differently than it runs.
    if grep -rnP '[\x{200B}-\x{200F}\x{202A}-\x{202E}\x{2066}-\x{2069}\x{FEFF}]' src tests data docs examples; then
        die "invisible characters in the files above; use escapes like \\u202E instead"
    fi

    echo "==> building and running the self-tests"
    cmake -S . -B build >/dev/null
    cmake --build build -j"$(nproc)" >/dev/null
    # New or changed strings must be in the translation template before a release.
    cmake --build build --target update_translations >/dev/null 2>&1
    python3 -I packaging/fill-english-plurals.py i18n/diskforge_en.ts
    git diff --quiet -- i18n || die "translations are out of date: commit the changes in i18n/ and stage again"
    logs="$root/packaging/staging/test-logs"
    mkdir -p "$logs"
    run_tests() { # name command...
        local name=$1
        shift
        if "$@" >"$logs/$name.log" 2>&1; then
            echo "    $name: $(tail -1 "$logs/$name.log")"
        else
            grep -E '^FAIL' "$logs/$name.log" >&2
            die "$name tests failed, nothing staged (full log: $logs/$name.log)"
        fi
    }
    # As you: the system-disk guard and everything that works on plain files.
    for suite in guard addons gpt copy usage backup rescuemap cleanup catalog fuzz health jobs recover rescueusb stickcheck images isomode windows lostfiles viruses registry startup; do
        run_tests $suite ./build/diskforge-selftest --$suite
    done
    run_tests window-user env QT_QPA_PLATFORM=offscreen ./build/diskforge-uitest --user
    # As root: test devices made with losetup and dmsetup.
    run_tests disks sudo ./build/diskforge-selftest
    for suite in badsectors blockmap rescue clone btrfs optimize stop raid lvm; do
        run_tests $suite sudo ./build/diskforge-selftest --$suite
    done
    run_tests window sudo env QT_QPA_PLATFORM=offscreen ./build/diskforge-uitest
    # The file-based suites again with AddressSanitizer and UBSan, which catch memory errors
    # that don't crash. Last, because building it takes a while and sudo's login runs out.
    cmake -S . -B build-asan -DDISKFORGE_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug >/dev/null
    cmake --build build-asan -j"$(nproc)" --target diskforge-selftest >/dev/null
    for suite in addons gpt copy usage backup rescuemap catalog fuzz health jobs recover rescueusb stickcheck images isomode windows lostfiles viruses registry startup; do
        run_tests asan-$suite env ASAN_OPTIONS=detect_leaks=0 ./build-asan/diskforge-selftest --$suite
    done

    # Local tag only. Restaging after a fix moves it to the new commit.
    git tag -d "v$v" >/dev/null 2>&1 || true
    git tag -s "v$v" -m "DiskForge $v"

    rm -rf "$stage"
    mkdir -p "$stage"
    git archive --format=tar.gz --prefix="diskforge-$v/" -o "$stage/$tarball" "v$v"
    (cd "$stage" && sha256sum "$tarball" > SHA256SUMS)
    hash=$(cut -d' ' -f1 "$stage/SHA256SUMS")
    cp "$pkgbuild" "$stage/PKGBUILD"
    set_pkgbuild "$stage/PKGBUILD" "$v" "$hash"

    try_makepkg() { (cd "$tmp/pkg" && makepkg -f --noconfirm >"$tmp/makepkg.log" 2>&1) || { tail -20 "$tmp/makepkg.log"; die "$1"; }; }
    mkdir "$tmp/pkg"
    old=$(pkgver_of "$pkgbuild")
    # Build the current release first, then the new one in the same folder, the way
    # someone updating would. That's how the 0.4.1 -> 0.4.2 update broke.
    if gh release view "v$old" -R "$repo" >/dev/null 2>&1; then
        echo "==> building the current release ($old), then updating to $v in the same folder"
        cp "$pkgbuild" "$tmp/pkg/PKGBUILD"
        try_makepkg "the current release $old doesn't build"
    else
        echo "==> no release on GitHub to update from, building $v on its own"
    fi
    cp "$stage/PKGBUILD" "$stage/$tarball" "$tmp/pkg/"  # makepkg uses the local file and still checks the hash
    try_makepkg "$v doesn't build"
    cp "$tmp/pkg/$pkg" "$stage/"

    echo "==> checking the package"
    mkdir "$tmp/root"
    bsdtar -xf "$stage/$pkg" -C "$tmp/root"
    [[ "$(QT_QPA_PLATFORM=offscreen "$tmp/root/usr/bin/diskforge" --version)" == "diskforge $v" ]] || die "packaged program doesn't report $v"
    QT_QPA_PLATFORM=offscreen "$tmp/root/usr/bin/diskforge" --dump >/dev/null || die "packaged program can't read the disks"
    desktop-file-validate "$tmp/root/usr/share/applications/"*.desktop
    appstreamcli validate --no-net "$tmp/root/usr/share/metainfo/"*.xml >/dev/null || die "metainfo doesn't validate"
    for f in usr/share/icons/hicolor/scalable/apps usr/share/man/man1/diskforge.1.gz \
             usr/lib/systemd/system/diskforge-journal-vacuum.service usr/lib/systemd/system/diskforge-paccache-uninstalled.service; do
        [[ -e "$tmp/root/$f" ]] || die "package is missing $f"
    done

    # DiskForge Live, from the same source, so the stick always has the same DiskForge.
    echo "==> building DiskForge Live $v (podman, takes a while)"
    command -v podman >/dev/null || die "podman is needed to build DiskForge Live"
    rescue/build.sh >"$logs/rescue.log" 2>&1 || { tail -20 "$logs/rescue.log"; die "DiskForge Live didn't build (full log: $logs/rescue.log)"; }
    iso=$(ls -t rescue/out/diskforge-live-"$v"-*.iso 2>/dev/null | head -1)
    [[ -n "$iso" ]] || die "the DiskForge Live build made no ISO for $v"
    cp "$iso" "$stage/$rescue_iso"
    (cd "$stage" && sha256sum "$rescue_iso" > "$rescue_iso.sha256")

    cat <<EOF
==> $v is staged in packaging/staging/$v. Nothing is on GitHub yet.

Install it and try it:
  sudo pacman -U packaging/staging/$v/$pkg
On a USB stick you don't need: mount, unmount, format, new partition, resize, rename, delete,
Back Up and Restore, Clone Drive (onto another spare stick), Rescue Copy.
Then Disk Usage, Disk Cleanup, Optimize Drives, Btrfs Snapshots, Help, About and Check for Updates.
Add-ons: install examples/addons/folder-sizes from a file, run it on the system disk (look-only),
run your own add-on (it should say it was added outside DiskForge), and check what the
run question shows. Make one with Make an Add-on and Test it; pin it to the toolbar.
On the spare stick: Wipe Disk and Stop it halfway from the bar on top; delete a partition and
put it back with Recover Partitions; Partition Type and Flags; Inspect Partition Table.
Disk Health: the reasons, the firmware row, the Btrfs part; the warning bar and Dismiss.
View > Theme (each one), a hard drive's Properties > Power, Rescue Copy's "Go easy" options.
USB sticks: Check a USB Stick, Write Image to USB with a compressed image and with "copy the
files" (and persistence), Make a Windows USB, and Make a DiskForge Live USB with
packaging/staging/$v/$rescue_iso. Start a PC (or a VM) from the DiskForge Live stick, UEFI and an old
BIOS one if you have it, and look at its logs. Find Lost Files on a stick with a few deleted pictures,
saving them onto another drive.

All good:  packaging/release.sh --publish $v
Problem:   fix it, commit, and run --stage $v again
EOF
    ;;

--publish)
    [[ -f "$stage/$tarball" && -f "$stage/$pkg" && -f "$stage/$rescue_iso" ]] || die "nothing staged for $v, run --stage $v first"
    ! on_github "$v" || die "v$v is already on GitHub"
    git tag -v "v$v" >/dev/null 2>&1 || die "tag v$v is missing or not signed"
    # The staged file has to be exactly what the tag produces, so what you tested is what goes up.
    git archive --format=tar.gz --prefix="diskforge-$v/" -o "$tmp/$tarball" "v$v"
    cmp -s "$tmp/$tarball" "$stage/$tarball" || die "the staged file doesn't match tag v$v, run --stage $v again"
    hash=$(cut -d' ' -f1 "$stage/SHA256SUMS")
    notes=$(awk -v h="## $v" '$0 == h {on=1; next} /^## / {on=0} on' CHANGELOG.md)

    git push -q
    git push -q origin "v$v"
    gh release create "v$v" "$stage/$tarball" "$stage/SHA256SUMS" "$stage/$rescue_iso" "$stage/$rescue_iso.sha256" \
        -R "$repo" --verify-tag --latest \
        --title "DiskForge $v" --notes "$notes

sha256: \`$hash\`" >/dev/null
    set_pkgbuild "$pkgbuild" "$v" "$hash"
    git commit -qm "release $v" "$pkgbuild"
    git push -q
    echo "==> $v is out: https://github.com/$repo/releases/tag/v$v"
    sync_aur "update to $v"

    # GitHub builds the Flatpak and AppImage for the release; wait for them.
    echo "==> waiting for the Flatpak and AppImage to be built"
    run_id=""
    for _ in $(seq 30); do
        run_id=$(gh run list -R "$repo" --workflow bundles.yml --event release --limit 5 \
            --json databaseId,headBranch --jq ".[] | select(.headBranch == \"v$v\") | .databaseId" | head -1)
        [[ -n "$run_id" ]] && break
        sleep 10
    done
    [[ -n "$run_id" ]] || die "the bundles workflow didn't start; the release is out without them (check the Actions tab)"
    gh run watch "$run_id" -R "$repo" --exit-status >/dev/null || die "building the bundles failed; the release is out without them (gh run view $run_id -R $repo)"
    assets=$(gh release view "v$v" -R "$repo" --json assets --jq '.assets[].name')
    for f in DiskForge-x86_64.flatpak DiskForge-x86_64.AppImage; do
        grep -qx "$f" <<<"$assets" || die "$f wasn't attached to the release"
    done
    echo "==> the Flatpak and AppImage are attached"
    ;;

--pull)
    gh release view "v$v" -R "$repo" >/dev/null 2>&1 || die "no release v$v on GitHub"
    gh release delete "v$v" -R "$repo" --cleanup-tag --yes
    git tag -d "v$v" >/dev/null 2>&1 || true
    prev=$(gh release list -R "$repo" --exclude-drafts --exclude-pre-releases --limit 1 --json tagName --jq '.[0].tagName')
    if [[ -z "$prev" ]]; then
        echo "v$v is gone. There's no other release, so the PKGBUILD can't build until the next one is published."
        exit 0
    fi
    set_pkgbuild "$pkgbuild" "${prev#v}" "$(release_hash "$prev")"
    git commit -qm "pull $v" "$pkgbuild"
    git push -q
    echo "v$v is gone and the PKGBUILD builds ${prev#v} again. Fix it and release a new version number."
    sync_aur "back to ${prev#v}, $v was pulled"
    ;;

--aur)
    [[ "$(pkgver_of "$pkgbuild")" == "$v" ]] || die "the PKGBUILD is at $(pkgver_of "$pkgbuild"), not $v"
    gh release view "v$v" -R "$repo" >/dev/null 2>&1 || die "v$v isn't released on GitHub"
    sync_aur "$v-$(sed -n 's/^pkgrel=//p' "$pkgbuild")"
    ;;

*)
    die "usage: release.sh --stage|--publish|--pull|--aur <version>"
    ;;
esac
