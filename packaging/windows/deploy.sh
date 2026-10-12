#!/bin/bash
# Puts a Windows program in a folder with every DLL and Qt plugin it needs, found by following
# each file's imports (what windeployqt does, without running Windows tools).
#   deploy.sh <program.exe> <folder> <sysroot>
set -euo pipefail
exe=$1 out=$2 sysroot=$3/ucrt64
mkdir -p "$out"
cp "$exe" "$out/"

# Where DLLs come from: the MSYS2 libraries first, then the compiler's own runtime.
search=("$sysroot/bin" /usr/x86_64-w64-mingw32/bin)
# offscreen: for --dump and --screenshot, which draw without a screen.
plugins=(platforms/qwindows.dll platforms/qoffscreen.dll styles/qmodernwindowsstyle.dll iconengines/qsvgicon.dll
    imageformats/qgif.dll imageformats/qico.dll imageformats/qjpeg.dll imageformats/qsvg.dll
    imageformats/qwebp.dll imageformats/qtiff.dll tls/qschannelbackend.dll
    multimedia/windowsmediaplugin.dll)
# Films and songs in Find Lost Files' viewer play through Windows' own Media Foundation (MSYS2's
# qt6-multimedia-wmf): Qt's FFmpeg one would bring over a hundred megabytes of codecs Windows has.
for p in "${plugins[@]}"; do
    if [[ -f $sysroot/share/qt6/plugins/$p ]]; then
        mkdir -p "$out/$(dirname "$p")"
        cp "$sysroot/share/qt6/plugins/$p" "$out/$p"
    fi
done

# Every file in the folder, and every DLL they pull in, until nothing new turns up.
declare -A done
todo=("$out/$(basename "$exe")")
while read -r p; do todo+=("$p"); done < <(find "$out" -mindepth 2 -name '*.dll')
while ((${#todo[@]})); do
    file=${todo[-1]}
    unset 'todo[-1]'
    while read -r dll; do
        key=${dll,,}
        [[ -n ${done[$key]:-} ]] && continue
        done[$key]=1
        for dir in "${search[@]}"; do
            found=$(find "$dir" -maxdepth 1 -iname "$dll" -print -quit 2>/dev/null)
            if [[ -n $found ]]; then
                cp "$found" "$out/"
                todo+=("$out/$(basename "$found")")
                break
            fi
        done
        # Not found means it comes with Windows (kernel32.dll, the api-ms-win-* sets...).
    done < <(x86_64-w64-mingw32-objdump -p "$file" | sed -n 's/^\s*DLL Name: //p')
done

# Qt's own translations for its buttons and dialogs.
if [[ -d $sysroot/share/qt6/translations ]]; then
    mkdir -p "$out/translations"
    cp "$sysroot"/share/qt6/translations/qtbase_*.qm "$out/translations/" 2>/dev/null || true
fi
