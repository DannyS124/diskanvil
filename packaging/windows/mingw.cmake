# Cross-compiling for Windows from Linux with MinGW-w64 (UCRT). The libraries come from the
# sysroot that fetch-msys2.py fills; moc, rcc and the rest from a Linux Qt of the same version.
#   -DCMAKE_TOOLCHAIN_FILE=packaging/windows/mingw.cmake -DDISKFORGE_SYSROOT=... -DQT_HOST_PATH=...
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

# Passed on to try_compile, which runs this file again.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES DISKFORGE_SYSROOT)

set(CMAKE_FIND_ROOT_PATH "${DISKFORGE_SYSROOT}/ucrt64" /usr/x86_64-w64-mingw32)
set(CMAKE_PREFIX_PATH "${DISKFORGE_SYSROOT}/ucrt64")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The .pc files say prefix=/ucrt64.
set(ENV{PKG_CONFIG_LIBDIR} "${DISKFORGE_SYSROOT}/ucrt64/lib/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${DISKFORGE_SYSROOT}")
