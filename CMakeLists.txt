# SPDX-FileCopyrightText: 2026 Danny S
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.21)
project(diskforge
    VERSION 0.5.1
    DESCRIPTION "Disk manager for Linux in the style of Windows Disk Management"
    HOMEPAGE_URL "https://github.com/DannyS124/diskforge"
    LANGUAGES CXX)

set(APP_ID "io.github.DannyS124.DiskForge")
set(APP_AUTHOR "Danny S")
set(APP_AUTHOR_URL "https://github.com/DannyS124")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Windows builds are cross-compiled with MinGW-w64; see packaging/windows/build.sh.
if(WIN32)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Network LinguistTools)
else()
    find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets DBus Network LinguistTools)
endif()
qt_standard_project_setup(I18N_SOURCE_LANGUAGE en)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
if(NOT WIN32)
    pkg_check_modules(BLKID REQUIRED IMPORTED_TARGET blkid) # util-linux, finds file systems for Recover Partitions
endif()
pkg_check_modules(ARCHIVE REQUIRED IMPORTED_TARGET libarchive) # unpacks compressed disk images (.xz, .gz, .zip...)
# Ed25519, for the signature on the online add-on list
find_package(OpenSSL 1.1.1 REQUIRED COMPONENTS Crypto)

# Memory and undefined-behaviour checks for the crash tests (release.sh builds this in build-asan).
option(DISKFORGE_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
if(DISKFORGE_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

set(APP_DEFINITIONS
    APP_VERSION="${PROJECT_VERSION}"
    APP_ID="${APP_ID}"
    APP_AUTHOR="${APP_AUTHOR}"
    APP_AUTHOR_URL="${APP_AUTHOR_URL}"
    APP_HOMEPAGE="${PROJECT_HOMEPAGE_URL}"
    APP_SUPPORT_URL="https://ko-fi.com/K0C528JI83")

# Everything that talks to UDisks2 (or Windows' disk API) or touches devices, shared with the
# test tools.
add_library(diskforge-core STATIC
    src/udisks.h src/udisks_common.cpp
    src/fsprobe.cpp src/fsprobe.h
    src/format.cpp src/format.h
    src/blockio.cpp src/blockio.h
    src/systemd.h
    src/imagewriter.cpp src/imagewriter.h
    src/imagesource.cpp src/imagesource.h
    src/lostsource.cpp src/lostsource.h
    src/lostscan.cpp src/lostscan.h
    src/lostfs.cpp src/lostfs.h src/lostntfs.cpp src/lostraw.h
    src/mediainfo.cpp src/mediainfo.h
    src/officetext.cpp src/officetext.h
    src/virusscan.cpp src/virusscan.h
    src/filetypes.cpp src/filetypes.h
    src/checksums.cpp src/checksums.h
    src/benchmark.cpp src/benchmark.h
    src/addons.cpp src/addons.h
    src/surfacescan.cpp src/surfacescan.h
    src/gpt.cpp src/gpt.h
    src/blockcopy.cpp src/blockcopy.h
    src/blockmapdata.cpp src/blockmapdata.h
    src/fswalk.cpp src/fswalk.h src/squarify.h
    src/clone.cpp src/clone.h
    src/imagebackup.cpp src/imagebackup.h
    src/rescuecopy.cpp src/rescuecopy.h
    src/cleanup.cpp src/cleanup.h
    src/snapper.h
    src/signature.cpp src/signature.h
    src/outputfilter.cpp src/outputfilter.h
    src/health.cpp src/health.h
    src/firmware.h
    src/btrfscheck.cpp src/btrfscheck.h
    src/drivepower.cpp src/drivepower.h
    src/partrecover.cpp src/partrecover.h
    src/partscan.h
    src/applog.cpp src/applog.h
    src/isofs.cpp src/isofs.h
    src/filecopy.cpp src/filecopy.h
    src/isomode.cpp src/isomode.h
    src/windowsusb.cpp src/windowsusb.h
    src/hostprocess.cpp src/hostprocess.h
    src/rescuestick.cpp src/rescuestick.h
    src/stickcheck.cpp src/stickcheck.h
)
if(WIN32)
    target_sources(diskforge-core PRIVATE
        src/win/blockio_win.cpp
        src/win/udisks_win.cpp
        src/win/winutil.cpp src/win/winutil.h
        src/win/unavailable.cpp)
    target_link_libraries(diskforge-core PUBLIC Qt6::Core PRIVATE PkgConfig::ZSTD PkgConfig::ARCHIVE OpenSSL::Crypto
        setupapi cfgmgr32 ole32 oleaut32 wbemuuid uuid virtdisk)
    # Large files and the Windows 10 API (CM_Register_Notification, storage queries).
    target_compile_definitions(diskforge-core PUBLIC _FILE_OFFSET_BITS=64 _WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE NOMINMAX)
else()
    target_sources(diskforge-core PRIVATE
        src/udisks.cpp src/operations.cpp src/dbusnames.h
        src/systemd.cpp src/snapper.cpp src/firmware.cpp src/partscan.cpp)
    target_link_libraries(diskforge-core PUBLIC Qt6::Core Qt6::DBus PRIVATE PkgConfig::ZSTD PkgConfig::BLKID PkgConfig::ARCHIVE OpenSSL::Crypto)
endif()
target_include_directories(diskforge-core PUBLIC src)
target_compile_definitions(diskforge-core PUBLIC ${APP_DEFINITIONS})
target_compile_options(diskforge-core PRIVATE -Wall -Wextra)

# Windows and dialogs, shared by the app, the dialog preview and the window test.
add_library(diskforge-ui STATIC
    src/diskmap.cpp src/diskmap.h
    src/homewindow.cpp src/homewindow.h
    src/lostfilesdialog.cpp src/lostfilesdialog.h
    src/preview.cpp src/preview.h
    src/previewloader.cpp src/previewloader.h
    src/mainwindow.cpp src/mainwindow.h
    src/dialogs.cpp src/dialogs.h
    src/tools.cpp src/tools.h
    src/rescueusbdialog.cpp src/rescueusbdialog.h
    src/udiskssteps.cpp src/udiskssteps.h
    src/usbprep.cpp src/usbprep.h
    src/isomount.cpp src/isomount.h
    src/windowsusbjob.cpp src/windowsusbjob.h
    src/windowsusbdialog.cpp src/windowsusbdialog.h
    src/isocopy.cpp src/isocopy.h
    src/stickcheckdialog.cpp src/stickcheckdialog.h
    src/blockmapwidget.cpp src/blockmapwidget.h
    src/jobui.cpp src/jobui.h
    src/copydialogs.cpp src/copydialogs.h
    src/usagedialog.cpp src/usagedialog.h
    src/virusdialog.cpp src/virusdialog.h
    src/systemtools.cpp src/systemtools.h
    src/erasedialog.cpp src/erasedialog.h
    src/addonsdialog.cpp src/addonsdialog.h
    src/addonprompt.cpp src/addonprompt.h
    src/addonform.cpp src/addonform.h
    src/addonmaker.cpp src/addonmaker.h
    src/addonoutput.cpp src/addonoutput.h
    src/noticebar.cpp src/noticebar.h
    src/typedialog.cpp src/typedialog.h
    src/powerbox.cpp src/powerbox.h
    src/inspectdialog.cpp src/inspectdialog.h
    src/recoverdialog.cpp src/recoverdialog.h
    src/catalogdialog.cpp src/catalogdialog.h
    src/about.cpp src/about.h
    src/updates.cpp src/updates.h
    src/translations.cpp src/translations.h
    src/theme.cpp src/theme.h
    src/thememaker.cpp src/thememaker.h
)
target_link_libraries(diskforge-ui PUBLIC diskforge-core Qt6::Widgets Qt6::Network)
# PDF pages in Find Lost Files' previews, when poppler's Qt 6 library is there (releases have it).
option(DISKFORGE_PDF "Show PDF pages in Find Lost Files (needs poppler-qt6)" ON)
if(DISKFORGE_PDF)
    pkg_check_modules(POPPLER IMPORTED_TARGET poppler-qt6)
    if(POPPLER_FOUND)
        target_link_libraries(diskforge-ui PRIVATE PkgConfig::POPPLER)
        target_compile_definitions(diskforge-ui PRIVATE DISKFORGE_PDF)
    else()
        message(STATUS "poppler-qt6 not found: Find Lost Files shows PDFs without their pages")
    endif()
endif()
if(WIN32)
    target_link_libraries(diskforge-ui PRIVATE powrprof)
endif()
target_compile_options(diskforge-ui PRIVATE -Wall -Wextra)

# Help page, icon and license text, built into each program that shows them.
function(diskforge_add_data target)
    qt_add_resources(${target} "${target}_data" PREFIX "/" FILES data/help.html data/${APP_ID}.svg LICENSE)
    qt_add_resources(${target} "${target}_themes" PREFIX "/themes" BASE data/themes
        FILES data/themes/classic.json data/themes/deadshadow.json data/themes/diskforge-live.json data/themes/high-contrast.json)
endfunction()

qt_add_executable(diskforge src/main.cpp)
diskforge_add_data(diskforge)
if(WIN32)
    # Its own window (no console), the icon and version details, and it asks to run as administrator.
    set_target_properties(diskforge PROPERTIES WIN32_EXECUTABLE ON)
    configure_file(data/windows/diskforge.rc.in diskforge.rc @ONLY)
    target_sources(diskforge PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/diskforge.rc)
endif()
target_link_libraries(diskforge PRIVATE diskforge-ui)
target_compile_options(diskforge PRIVATE -Wall -Wextra)


# dev tools, not installed
if(WIN32)
    option(DISKFORGE_SELFTEST "Build the loop-device self-test" OFF)
else()
    option(DISKFORGE_SELFTEST "Build the loop-device self-test" ON)
endif()
if(DISKFORGE_SELFTEST)
    qt_add_executable(diskforge-selftest tests/selftest.cpp tests/testkit.cpp tests/testkit.h
        tests/test_gpt.cpp tests/test_copy.cpp tests/test_blockmap.cpp
        tests/test_usage.cpp tests/test_backup.cpp
        tests/test_rescue.cpp tests/test_clone.cpp tests/test_system.cpp
        tests/test_catalog.cpp tests/test_fuzz.cpp tests/test_health.cpp tests/test_jobs.cpp tests/test_recover.cpp tests/test_rescueusb.cpp tests/test_stickcheck.cpp tests/test_images.cpp tests/test_isomode.cpp tests/test_windows.cpp tests/test_lostfiles.cpp tests/test_viruses.cpp tests/test_raid.cpp tests/test_lvm.cpp)
    target_link_libraries(diskforge-selftest PRIVATE diskforge-ui)
    target_compile_definitions(diskforge-selftest PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    qt_add_executable(diskforge-preview tests/preview.cpp)
    diskforge_add_data(diskforge-preview)
    target_link_libraries(diskforge-preview PRIVATE diskforge-ui)
    target_compile_definitions(diskforge-preview PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    qt_add_executable(diskforge-uitest tests/uitest.cpp)
    diskforge_add_data(diskforge-uitest)
    target_link_libraries(diskforge-uitest PRIVATE diskforge-ui)
    target_compile_definitions(diskforge-uitest PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

# English plural forms ("1 sector", "2 sectors"); other languages get their own
# i18n/diskforge_<lang>.ts (see CONTRIBUTING.md).
set(TRANSLATED_TARGETS diskforge)
if(DISKFORGE_SELFTEST)
    list(APPEND TRANSLATED_TARGETS diskforge-preview)
endif()
qt_add_translations(TARGETS ${TRANSLATED_TARGETS}
    SOURCE_TARGETS diskforge-core diskforge-ui diskforge
    TS_FILE_DIR i18n
    PLURALS_TS_FILE i18n/diskforge_en.ts
    RESOURCE_PREFIX /i18n)

include(GNUInstallDirs)
if(WIN32)
    # The rest (DLLs, Qt plugins) is put next to it by packaging/windows/deploy.sh.
    install(TARGETS diskforge RUNTIME DESTINATION .)
    return()
endif()
configure_file(data/${APP_ID}.desktop.in ${APP_ID}.desktop @ONLY)
configure_file(data/${APP_ID}.metainfo.xml.in ${APP_ID}.metainfo.xml @ONLY)
configure_file(data/diskforge.1.in diskforge.1 @ONLY)

install(TARGETS diskforge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES data/${APP_ID}.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/diskforge.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
# Disk Cleanup starts these to shrink the journal and the package cache; see the files for why they're units.
# Off for Flatpak and AppImage builds, which can't add system units.
option(DISKFORGE_SYSTEMD_UNIT "Install the journal cleanup unit" ON)
if(DISKFORGE_SYSTEMD_UNIT)
    pkg_get_variable(SYSTEMD_UNIT_DIR systemd systemd_system_unit_dir)
    if(NOT SYSTEMD_UNIT_DIR)
        set(SYSTEMD_UNIT_DIR ${CMAKE_INSTALL_PREFIX}/lib/systemd/system)
    endif()
    install(FILES data/diskforge-journal-vacuum.service data/diskforge-paccache-uninstalled.service DESTINATION ${SYSTEMD_UNIT_DIR})
endif()

# A .deb for Debian and Ubuntu: `cpack -G DEB` in the build folder (packaging/deb/build.sh does it in a
# clean Debian container). Library dependencies come from dpkg-shlibdeps; what it can't see is listed here.
set(CPACK_PACKAGE_NAME diskforge)
set(CPACK_PACKAGE_VENDOR "Danny S")
set(CPACK_PACKAGE_CONTACT "Danny S <48356907+DannyS124@users.noreply.github.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Disk manager and partition editor that works like Windows Disk Management")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/DannyS124/diskforge")
set(CPACK_DEBIAN_PACKAGE_SECTION admin)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "udisks2, polkitd, qt6-svg-plugins")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "dosfstools, exfatprogs, ntfs-3g, btrfs-progs")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "xfsprogs, bubblewrap, fwupd, udisks2-lvm2, snapper, wimtools, clamav")
set(CPACK_STRIP_FILES ON)
include(CPack)
