#!/bin/bash
# Builds DiskAnvil for Windows on Linux: MinGW-w64 (the mingw-w64-gcc package), Qt and the
# other libraries from MSYS2 (fetch-msys2.py), and moc and friends from a Linux Qt of the
# same version (aqtinstall, in a Python venv). Everything downloaded goes in one work folder.
#   packaging/windows/build.sh [work folder, default ~/.cache/diskanvil-windows]
# Ends with the program and everything it needs in <work>/DiskAnvil, zipped next to it, and with
# msitools there, DiskAnvil-<version>.msi to install it (msi.sh).
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(realpath -m "${1:-${DISKANVIL_WINDOWS_WORK:-$HOME/.cache/diskanvil-windows}}")
mkdir -p "$work"

python3 "$root/packaging/windows/fetch-msys2.py" "$work" qt6-base qt6-svg qt6-imageformats qt6-translations zstd libarchive openssl poppler-qt6
qt=$(sed -n 's/^set(PACKAGE_VERSION "\([0-9.]*\)")$/\1/p' "$work/sysroot/ucrt64/lib/cmake/Qt6Core/Qt6CoreConfigVersionImpl.cmake")
hostqt="$work/hostqt/$qt/gcc_64"
if [[ ! -x $hostqt/libexec/moc ]]; then
    [[ -x $work/venv/bin/aqt ]] || { python3 -m venv "$work/venv" && "$work/venv/bin/pip" install -q aqtinstall; }
    (cd "$work" && "$work/venv/bin/aqt" install-qt linux desktop "$qt" linux_gcc_64 --archives qtbase qttools qtdeclarative icu -O "$work/hostqt")
fi

cmake -S "$root" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$root/packaging/windows/mingw.cmake" \
    -DDISKANVIL_SYSROOT="$work/sysroot" -DQT_HOST_PATH="$hostqt" >/dev/null
cmake --build "$work/build"
x86_64-w64-mingw32-strip "$work/build/diskanvil.exe"

version=$(sed -n 's/^ *VERSION \([0-9.]*\)$/\1/p' "$root/CMakeLists.txt" | head -1)
rm -rf "$work/DiskAnvil"
"$root/packaging/windows/deploy.sh" "$work/build/diskanvil.exe" "$work/DiskAnvil" "$work/sysroot"
cp "$root/LICENSE" "$work/DiskAnvil/LICENSE.txt"
python3 -I "$root/packaging/windows/icons.py" "$root/rescue/icons/DiskAnvil-Live" "$work/DiskAnvil/icons"
(cd "$work" && rm -f "DiskAnvil-$version-windows.zip" && python3 -m zipfile -c "DiskAnvil-$version-windows.zip" DiskAnvil)
echo "==> $work/DiskAnvil-$version-windows.zip"
# The installer too, when msitools is here.
if command -v wixl >/dev/null; then
    "$root/packaging/windows/msi.sh" "$work"
fi
