#!/bin/sh
# Makes tests/data/pe/versioned.exe: a tiny Windows program of our own with a version resource,
# for peinfo's tests. Needs MinGW-w64 (x86_64-w64-mingw32-gcc and -windres).
set -e
cd "$(dirname "$0")"
work=$(mktemp -d)
cat > "$work/start.c" <<'C'
void start(void) {}
C
cat > "$work/version.rc" <<'RC'
#pragma code_page(65001)
1 VERSIONINFO
FILEVERSION 3,1,4,1
PRODUCTVERSION 3,1,4,1
FILETYPE 3
FILESUBTYPE 6
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904B0"
    BEGIN
      VALUE "CompanyName", "DiskForge Test Company"
      VALUE "FileDescription", "Grüße driver for tests"
      VALUE "FileVersion", "3.1.4.1"
      VALUE "ProductName", "Test Product"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
RC
x86_64-w64-mingw32-windres -c 65001 "$work/version.rc" -O coff -o "$work/version.o"
x86_64-w64-mingw32-gcc -Os -s -nostdlib -Wl,--entry=start -o versioned.exe "$work/start.c" "$work/version.o"
rm -r "$work"
