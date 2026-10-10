#!/bin/bash
# Builds DiskForge for Windows on Linux: MinGW-w64 (the mingw-w64-gcc package), Qt and the
# other libraries from MSYS2 (fetch-msys2.py), and moc and friends from a Linux Qt of the
# same version (aqtinstall, in a Python venv). Everything downloaded goes in one work folder.
#   packaging/windows/build.sh [work folder, default ~/.cache/diskforge-windows]
# Ends with the program and everything it needs in <work>/DiskForge, zipped next to it.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(realpath -m "${1:-${DISKFORGE_WINDOWS_WORK:-$HOME/.cache/diskforge-windows}}")
mkdir -p "$work"

python3 "$root/packaging/windows/fetch-msys2.py" "$work" qt6-base qt6-svg qt6-imageformats qt6-translations zstd libarchive openssl
qt=$(sed -n 's/^set(PACKAGE_VERSION "\([0-9.]*\)")$/\1/p' "$work/sysroot/ucrt64/lib/cmake/Qt6Core/Qt6CoreConfigVersionImpl.cmake")
hostqt="$work/hostqt/$qt/gcc_64"
if [[ ! -x $hostqt/libexec/moc ]]; then
    [[ -x $work/venv/bin/aqt ]] || { python3 -m venv "$work/venv" && "$work/venv/bin/pip" install -q aqtinstall; }
    "$work/venv/bin/aqt" install-qt linux desktop "$qt" linux_gcc_64 --archives qtbase qttools qtdeclarative icu -O "$work/hostqt"
fi

cmake -S "$root" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$root/packaging/windows/mingw.cmake" \
    -DDISKFORGE_SYSROOT="$work/sysroot" -DQT_HOST_PATH="$hostqt" >/dev/null
cmake --build "$work/build"
x86_64-w64-mingw32-strip "$work/build/diskforge.exe"

version=$(sed -n 's/^ *VERSION \([0-9.]*\)$/\1/p' "$root/CMakeLists.txt" | head -1)
rm -rf "$work/DiskForge"
"$root/packaging/windows/deploy.sh" "$work/build/diskforge.exe" "$work/DiskForge" "$work/sysroot"
cp "$root/LICENSE" "$work/DiskForge/LICENSE.txt"
(cd "$work" && rm -f "DiskForge-$version-windows.zip" && python3 -m zipfile -c "DiskForge-$version-windows.zip" DiskForge)
echo "==> $work/DiskForge-$version-windows.zip"
