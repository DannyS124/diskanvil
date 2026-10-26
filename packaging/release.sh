#!/usr/bin/env bash

#
# The AUR repo is kept at ../diskanvil-aur, cloned the first time (ssh://aur@aur.archlinux.org/diskanvil.git,
# with the key in ~/.ssh/config's aur.archlinux.org entry on the AUR account).
#
# Never reuse a version number once it's been on GitHub.
set -euo pipefail

repo=DannyS124/diskanvil
root="$(cd "$(dirname "$0")/.." && pwd)"
pkgbuild="$root/packaging/arch/PKGBUILD"
cd "$root"

die() { echo "error: $*" >&2; exit 1; }
pkgver_of() { sed -n 's/^pkgver=//p' "$1"; }
on_github() { git ls-remote --exit-code --tags origin "refs/tags/v$1" >/dev/null 2>&1; }
release_hash() { gh release download "$1" -R "$repo" -p SHA256SUMS -O - | cut -d' ' -f1; }
aur="$(dirname "$root")/diskanvil-aur"
sync_aur() { # message
    if [[ ! -d "$aur/.git" ]]; then
        # The release is out by now, so the AUR not taking it is said loudly, not fatal.
        git clone -q ssh://aur@aur.archlinux.org/diskanvil.git "$aur" 2>/dev/null || {
            echo "!!! the AUR wasn't updated: it doesn't take the ssh key (is ~/.ssh/aur.pub on the AUR account?)."
            echo "!!! once it does: packaging/release.sh --aur $v"
            rm -rf "$aur"
            return 0
        }
        git -C "$aur" symbolic-ref HEAD refs/heads/master # a new package's repo is empty; the AUR only takes master
    fi
    cp "$pkgbuild" "$aur/PKGBUILD"
    (cd "$aur" && makepkg --printsrcinfo > .SRCINFO && git add PKGBUILD .SRCINFO \
        && { git diff --cached --quiet || { git commit -qm "$1" && git push -q origin master; }; })
    echo "==> AUR updated: https://aur.archlinux.org/packages/diskanvil"
}
set_pkgbuild() { # file version hash
    sed -i "s/^pkgver=.*/pkgver=$2/; s/^pkgrel=.*/pkgrel=1/; s/^sha256sums=.*/sha256sums=('$3')/" "$1"
}

cmd="${1:-}"
v="${2:-}"
[[ -n "$v" ]] || die "usage: release.sh --stage|--publish|--pull|--aur <version>"
stage="$root/packaging/staging/$v"
rescue_iso="diskanvil-live-$v.iso"
win_msi="DiskAnvil-$v.msi"
win_zip="DiskAnvil-$v-windows.zip"
tarball="diskanvil-$v.tar.gz"
pkg="diskanvil-$v-1-$(uname -m).pkg.tar.zst"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

case "$cmd" in
--stage)
    [[ -z "$(git status --porcelain)" ]] || die "commit or stash your changes first"
    grep -q "VERSION $v\$" CMakeLists.txt || die "set VERSION $v in CMakeLists.txt first"
    grep -q "release version=\"$v\"" data/io.github.DannyS124.DiskAnvil.metainfo.xml.in || die "add a <release version=\"$v\"> to the metainfo first"
    grep -q "^## $v\$" CHANGELOG.md || die "add a '## $v' section to CHANGELOG.md first"
    ! on_github "$v" || die "v$v is already on GitHub, use a new version"

    # DiskAnvil never takes orders from other programs: no sockets, no D-Bus service of its
    # own, nothing listening. Keep it that way. The one exception takes no orders either: Start
    # Other PCs from This One (src/tools/netboot*, DiskAnvil Live) hands out DiskAnvil Live's own
    # files, read-only, only while the person at the PC has it on.
    if grep -rnE --exclude='netboot.cpp' --exclude='netboot.h' --exclude='netbootdialog.cpp' \
        'QLocalServer|QTcpServer|QUdpSocket|QWebSocketServer|QSctpServer|registerService|registerObject|QDBusAbstractAdaptor|\blisten\(' src; then
        die "something in src/ listens for other programs (above); DiskAnvil mustn't"
    fi
    # Invisible characters (zero-width, text direction) can make code read differently than it runs.
    # A byte order mark is fine as a file's very first character (Windows PowerShell needs it to
    # read a script as UTF-8), and nowhere else.
    if grep -rnP '[\x{200B}-\x{200F}\x{202A}-\x{202E}\x{2066}-\x{2069}]' src tests data docs examples \
        || grep -rboP '\x{FEFF}' src tests data docs examples | grep -v ':0:'; then
        die "invisible characters in the files above; use escapes like \\u202E instead"
    fi

    echo "==> building and running the self-tests"
    cmake -S . -B build -DDISKANVIL_SHELL=ON >/dev/null
    cmake --build build -j"$(awk '/MemAvailable/ {j = int($2 / 1048576); print j < 1 ? 1 : j}' /proc/meminfo)" >/dev/null
    # New or changed strings must be in the translation template before a release.
    cmake --build build --target update_translations >/dev/null 2>&1
    python3 -I packaging/fill-english-plurals.py i18n/diskanvil_en.ts
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
    for suite in guard addons gpt copy usage backup rescuemap cleanup catalog fuzz health jobs recover rescueusb stickcheck images isomode windows lostfiles viruses registry startup boot newids fat32 luks savefiles clonezilla; do
        run_tests $suite ./build/diskanvil-selftest --$suite
    done
    run_tests window-user env QT_QPA_PLATFORM=offscreen ./build/diskanvil-uitest --user
    # DiskAnvil Live's screen, used with real DiskAnvil windows in it (a socket folder of its own).
    shell_run=$(mktemp -d); chmod 700 "$shell_run"
    run_tests shell env DISKANVIL_SHELL_POINTER=1 PATH="$PWD/build:$PATH" XDG_RUNTIME_DIR="$shell_run" QT_QPA_PLATFORM=offscreen ./build/diskanvil-shell --selftest
    rm -rf "$shell_run"
    # As root: test devices made with losetup and dmsetup.
    run_tests disks sudo ./build/diskanvil-selftest
    # Again as root, where DiskAnvil can open the test drive itself to unpack Windows' compressed files.
    run_tests viruses-root sudo ./build/diskanvil-selftest --viruses
    for suite in badsectors blockmap rescue clone btrfs optimize stop raid lvm; do
        run_tests $suite sudo ./build/diskanvil-selftest --$suite
    done
    run_tests window sudo env QT_QPA_PLATFORM=offscreen ./build/diskanvil-uitest
    run_tests window-newids sudo env QT_QPA_PLATFORM=offscreen ./build/diskanvil-uitest --user --only=newids
    # The windows that make test devices of their own, one at a time.
    for scenario in stickcheck burnin dirtyntfs synctimeout ext4journal sdcard refresh stopwipe luksheader nas tryrepair clonezilla testboot wipecert evidence writeblock; do
        run_tests window-$scenario sudo env QT_QPA_PLATFORM=offscreen ./build/diskanvil-uitest --only=$scenario
    done
    # The file-based suites again with AddressSanitizer and UBSan, which catch memory errors
    # that don't crash. Last, because building it takes a while and sudo's login runs out.
    cmake -S . -B build-asan -DDISKANVIL_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug >/dev/null
    cmake --build build-asan -j"$(awk '/MemAvailable/ {j = int($2 / 1048576); print j < 1 ? 1 : j}' /proc/meminfo)" --target diskanvil-selftest >/dev/null
    for suite in addons gpt copy usage backup rescuemap catalog fuzz health jobs recover rescueusb stickcheck images isomode windows lostfiles viruses registry startup boot newids fat32 luks savefiles clonezilla; do
        run_tests asan-$suite env ASAN_OPTIONS=detect_leaks=0 ./build-asan/diskanvil-selftest --$suite
    done

    # Local tag only. Restaging after a fix moves it to the new commit.
    git tag -d "v$v" >/dev/null 2>&1 || true
    git tag -s "v$v" -m "DiskAnvil $v"

    rm -rf "$stage"
    mkdir -p "$stage"
    git archive --format=tar.gz --prefix="diskanvil-$v/" -o "$stage/$tarball" "v$v"
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
        # The PKGBUILD people have now: GitHub's main branch, as the last publish left it (0.5.1's
        # is DiskForge's, under the name it had then).
        git show "origin/main:packaging/arch/PKGBUILD" > "$tmp/pkg/PKGBUILD" 2>/dev/null || cp "$pkgbuild" "$tmp/pkg/PKGBUILD"
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
    [[ "$(QT_QPA_PLATFORM=offscreen "$tmp/root/usr/bin/diskanvil" --version)" == "diskanvil $v" ]] || die "packaged program doesn't report $v"
    QT_QPA_PLATFORM=offscreen "$tmp/root/usr/bin/diskanvil" --dump >/dev/null || die "packaged program can't read the disks"
    desktop-file-validate "$tmp/root/usr/share/applications/"*.desktop
    appstreamcli validate --no-net "$tmp/root/usr/share/metainfo/"*.xml >/dev/null || die "metainfo doesn't validate"
    for f in usr/share/icons/hicolor/scalable/apps usr/share/man/man1/diskanvil.1.gz \
             usr/lib/systemd/system/diskanvil-journal-vacuum.service usr/lib/systemd/system/diskanvil-paccache-uninstalled.service; do
        [[ -e "$tmp/root/$f" ]] || die "package is missing $f"
    done

    # DiskAnvil Live, from the same source, so the stick always has the same DiskAnvil.
    echo "==> building DiskAnvil Live $v (podman, takes a while)"
    command -v podman >/dev/null || die "podman is needed to build DiskAnvil Live"
    rescue/build.sh >"$logs/rescue.log" 2>&1 || { tail -20 "$logs/rescue.log"; die "DiskAnvil Live didn't build (full log: $logs/rescue.log)"; }
    iso=$(ls -t rescue/out/diskanvil-live-"$v"-*.iso 2>/dev/null | head -1)
    [[ -n "$iso" ]] || die "the DiskAnvil Live build made no ISO for $v"
    cp "$iso" "$stage/$rescue_iso"
    (cd "$stage" && sha256sum "$rescue_iso" > "$rescue_iso.sha256")

    # The Windows version, from the same source: its installer, and a zip to run it from a
    # folder. Only where its cross-compiler is set up (see packaging/windows/build.sh); point
    # DISKANVIL_WINDOWS_WORK at the folder it downloads into.
    if command -v x86_64-w64-mingw32-g++ >/dev/null && command -v wixl >/dev/null; then
        echo "==> building the Windows version $v"
        win_work=$(realpath -m "${DISKANVIL_WINDOWS_WORK:-$HOME/.cache/diskanvil-windows}")
        packaging/windows/build.sh "$win_work" >"$logs/windows.log" 2>&1 || { tail -20 "$logs/windows.log"; die "the Windows version didn't build (full log: $logs/windows.log)"; }
        cp "$win_work/$win_msi" "$win_work/$win_zip" "$stage/"
        (cd "$stage" && sha256sum "$win_msi" "$win_zip" > SHA256SUMS-windows)
    else
        echo "==> no MinGW-w64 or msitools here, so no Windows version this time"
    fi

    cat <<EOF
==> $v is staged in packaging/staging/$v. Nothing is on GitHub yet.

Install it and try it:
  sudo pacman -U packaging/staging/$v/$pkg
On a USB stick you don't need: mount, unmount, format, new partition, resize, rename, delete,
Back Up and Restore, Clone Drive (onto another spare stick), Rescue Copy.
Then Disk Usage, Disk Cleanup, Optimize Drives, Btrfs Snapshots, Help, About and Check for Updates.
Add-ons: install examples/addons/folder-sizes from a file, run it on the system disk (look-only),
run your own add-on (it should say it was added outside DiskAnvil), and check what the
run question shows. Make one with Make an Add-on and Test it; pin it to the toolbar.
On the spare stick: Wipe Disk and Stop it halfway from the bar on top; delete a partition and
put it back with Recover Partitions; Partition Type and Flags; Inspect Partition Table.
Disk Health: the reasons, the firmware row, the Btrfs part; the warning bar and Dismiss.
View > Theme (each one), a hard drive's Properties > Power, Rescue Copy's "Go easy" options.
USB sticks: Check a USB Stick, Write Image to USB with a compressed image and with "copy the
files" (and persistence), Make a Windows USB, and Make a DiskAnvil Live USB with
packaging/staging/$v/$rescue_iso. Start a PC (or a VM) from the DiskAnvil Live stick, UEFI and an old
BIOS one if you have it, and look at its logs. Find Lost Files on a stick with a few deleted pictures,
saving them onto another drive.
Newer things: Find Lost Files on a FAT, exFAT and NTFS stick where you deleted some files and folders
(they should come back with their names), on a virtual machine's disk (File > Find Lost Files in a Disk
Image), and its previews of a PDF, a document, a song and a film. Scan for Viruses on a stick with the
EICAR test file on it (harmless, from eicar.org) and on your own drives. Give New IDs on a stick you
cloned. A SATA drive's settings and self-test history in Disk Health and Properties.
EOF
    if [[ -f "$stage/$win_msi" ]]; then
        cat <<EOF
Windows, on a PC or in a VM: install packaging/staging/$v/$win_msi. It isn't signed, so SmartScreen
warns (More info > Run anyway) and the admin prompt says the publisher is unknown. The zip next to it
runs without installing.
EOF
    fi
    cat <<EOF

All good:  packaging/release.sh --publish $v
Problem:   fix it, commit, and run --stage $v again
EOF
    ;;

--publish)
    [[ -f "$stage/$tarball" && -f "$stage/$pkg" && -f "$stage/$rescue_iso" ]] || die "nothing staged for $v, run --stage $v first"
    ! on_github "$v" || die "v$v is already on GitHub"
    git tag -v "v$v" >/dev/null 2>&1 || die "tag v$v is missing or not signed"
    # The staged file has to be exactly what the tag produces, so what you tested is what goes up.
    git archive --format=tar.gz --prefix="diskanvil-$v/" -o "$tmp/$tarball" "v$v"
    cmp -s "$tmp/$tarball" "$stage/$tarball" || die "the staged file doesn't match tag v$v, run --stage $v again"
    hash=$(cut -d' ' -f1 "$stage/SHA256SUMS")
    windows=()
    if [[ -f "$stage/$win_msi" ]]; then
        windows=("$stage/$win_msi" "$stage/$win_zip" "$stage/SHA256SUMS-windows")
    fi
    notes=$(awk -v h="## $v" '$0 == h {on=1; next} /^## / {on=0} on' CHANGELOG.md)

    git push -q
    git push -q origin "v$v"
    gh release create "v$v" "$stage/$tarball" "$stage/SHA256SUMS" "$stage/$rescue_iso" "$stage/$rescue_iso.sha256" "${windows[@]}" \
        -R "$repo" --verify-tag --latest \
        --title "DiskAnvil $v" --notes "$notes

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
    for f in DiskAnvil-x86_64.flatpak DiskAnvil-x86_64.AppImage; do
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
