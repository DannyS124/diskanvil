# SPDX-FileCopyrightText: 2026 Danny S
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.21)
project(diskforge
    VERSION 0.5.2
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
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib) # packed grains and clusters in virtual machines' disks
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
    src/drives/udisks.h src/drives/udisks_common.cpp
    src/drives/fsprobe.cpp src/drives/fsprobe.h
    src/common/format.cpp src/common/format.h
    src/drives/blockio.cpp src/drives/blockio.h
    src/tools/systemd.h
    src/usb/imagewriter.cpp src/usb/imagewriter.h
    src/usb/imagesource.cpp src/usb/imagesource.h
    src/lostfiles/lostsource.cpp src/lostfiles/lostsource.h
    src/lostfiles/lostscan.cpp src/lostfiles/lostscan.h
    src/lostfiles/lostfs.cpp src/lostfiles/lostfs.h src/lostfiles/lostntfs.cpp src/lostfiles/lostraw.h
    src/lostfiles/vshadow.cpp src/lostfiles/vshadow.h
    src/lostfiles/vdisk.cpp src/lostfiles/vdisk.h
    src/drives/fat32format.cpp src/drives/fat32format.h
    src/lostfiles/mediainfo.cpp src/lostfiles/mediainfo.h
    src/lostfiles/exif.cpp src/lostfiles/exif.h
    src/lostfiles/lookcopies.cpp src/lostfiles/lookcopies.h
    src/lostfiles/officetext.cpp src/lostfiles/officetext.h
    src/lostfiles/ole.cpp src/lostfiles/ole.h
    src/lostfiles/textkinds.cpp src/lostfiles/textkinds.h
    src/lostfiles/naming.cpp src/lostfiles/naming.h
    src/lostfiles/structure.cpp src/lostfiles/structure.h
    src/lostfiles/repair.cpp src/lostfiles/repair.h
    src/lostfiles/mp4fix.cpp src/lostfiles/mp4fix.h
    src/lostfiles/thumbcache.cpp src/lostfiles/thumbcache.h
    src/lostfiles/ransomware.cpp src/lostfiles/ransomware.h
    src/lostfiles/readthrough.cpp src/lostfiles/readthrough.h
    src/lostfiles/btrfsrestore.cpp src/lostfiles/btrfsrestore.h
    src/viruses/virusscan.cpp src/viruses/virusscan.h
    src/viruses/reghive.cpp src/viruses/reghive.h
    src/viruses/winstartup.cpp src/viruses/winstartup.h
    src/viruses/shelllink.cpp src/viruses/shelllink.h
    src/viruses/authenticode.cpp src/viruses/authenticode.h
    src/viruses/efivars.cpp src/viruses/efivars.h
    src/viruses/bootcheck.cpp src/viruses/bootcheck.h
    src/backup/newids.cpp src/backup/newids.h
    src/lostfiles/filetypes.cpp src/lostfiles/filetypes.h
    src/usb/checksums.cpp src/usb/checksums.h
    src/health/benchmark.cpp src/health/benchmark.h
    src/addons/addons.cpp src/addons/addons.h
    src/health/surfacescan.cpp src/health/surfacescan.h
    src/drives/gpt.cpp src/drives/gpt.h
    src/backup/blockcopy.cpp src/backup/blockcopy.h
    src/health/blockmapdata.cpp src/health/blockmapdata.h
    src/tools/fswalk.cpp src/tools/fswalk.h src/tools/squarify.h
    src/backup/clone.cpp src/backup/clone.h
    src/backup/imagebackup.cpp src/backup/imagebackup.h
    src/backup/rescuecopy.cpp src/backup/rescuecopy.h
    src/tools/cleanup.cpp src/tools/cleanup.h
    src/tools/snapper.h
    src/addons/signature.cpp src/addons/signature.h
    src/addons/outputfilter.cpp src/addons/outputfilter.h
    src/health/health.cpp src/health/health.h
    src/health/triage.cpp src/health/triage.h
    src/health/drivefacts.cpp src/health/drivefacts.h
    src/health/smartdata.cpp src/health/smartdata.h
    src/health/firmware.h
    src/health/btrfscheck.cpp src/health/btrfscheck.h
    src/drives/drivepower.cpp src/drives/drivepower.h
    src/drives/partrecover.cpp src/drives/partrecover.h
    src/drives/partscan.h
    src/common/applog.cpp src/common/applog.h
    src/usb/isofs.cpp src/usb/isofs.h
    src/usb/filecopy.cpp src/usb/filecopy.h
    src/usb/isomode.cpp src/usb/isomode.h
    src/usb/windowsusb.cpp src/usb/windowsusb.h
    src/common/hostprocess.cpp src/common/hostprocess.h
    src/usb/rescuestick.cpp src/usb/rescuestick.h
    src/usb/stickcheck.cpp src/usb/stickcheck.h
    src/drives/partscan.cpp src/drives/partscan.h
)
if(WIN32)
    target_sources(diskforge-core PRIVATE
        src/win/blockio_win.cpp
        src/win/udisks_win.cpp
        src/win/winutil.cpp src/win/winutil.h
        src/win/winstartup_win.cpp
        src/win/unavailable.cpp)
    target_link_libraries(diskforge-core PUBLIC Qt6::Core PRIVATE PkgConfig::ZSTD PkgConfig::ZLIB PkgConfig::ARCHIVE OpenSSL::Crypto
        setupapi cfgmgr32 ole32 oleaut32 wbemuuid uuid virtdisk)
    # Large files and the Windows 10 API (CM_Register_Notification, storage queries).
    target_compile_definitions(diskforge-core PUBLIC _FILE_OFFSET_BITS=64 _WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE NOMINMAX)
else()
    target_sources(diskforge-core PRIVATE
        src/drives/udisks.cpp src/drives/operations.cpp src/drives/dbusnames.h
        src/tools/systemd.cpp src/tools/snapper.cpp src/health/firmware.cpp)
    target_link_libraries(diskforge-core PUBLIC Qt6::Core Qt6::DBus PRIVATE PkgConfig::ZSTD PkgConfig::ZLIB PkgConfig::BLKID PkgConfig::ARCHIVE OpenSSL::Crypto)
endif()
# The sources sit in folders by feature and include each other by file name.
target_include_directories(diskforge-core PUBLIC src src/addons src/app src/backup src/common src/drives src/health src/lostfiles src/tools src/usb src/viruses)
target_compile_definitions(diskforge-core PUBLIC ${APP_DEFINITIONS})
target_compile_options(diskforge-core PRIVATE -Wall -Wextra)

# Windows and dialogs, shared by the app, the dialog preview and the window test.
add_library(diskforge-ui STATIC
    src/app/diskmap.cpp src/app/diskmap.h
    src/app/homewindow.cpp src/app/homewindow.h
    src/app/whathappened.cpp src/app/whathappened.h
    src/lostfiles/lostfilesdialog.cpp src/lostfiles/lostfilesdialog.h
    src/lostfiles/preview.cpp src/lostfiles/preview.h
    src/lostfiles/pictures.cpp src/lostfiles/pictures.h
    src/lostfiles/previewloader.cpp src/lostfiles/previewloader.h
    src/lostfiles/viewer.cpp src/lostfiles/viewer.h
    src/lostfiles/hexview.cpp src/lostfiles/hexview.h
    src/lostfiles/btrfsdialog.cpp src/lostfiles/btrfsdialog.h
    src/app/mainwindow.cpp src/app/mainwindow.h
    src/app/dialogs.cpp src/app/dialogs.h
    src/app/tools.cpp src/app/tools.h
    src/usb/rescueusbdialog.cpp src/usb/rescueusbdialog.h
    src/drives/udiskssteps.cpp src/drives/udiskssteps.h
    src/usb/usbprep.cpp src/usb/usbprep.h
    src/drives/isomount.cpp src/drives/isomount.h
    src/usb/windowsusbjob.cpp src/usb/windowsusbjob.h
    src/usb/windowsusbdialog.cpp src/usb/windowsusbdialog.h
    src/usb/isocopy.cpp src/usb/isocopy.h
    src/usb/stickcheckdialog.cpp src/usb/stickcheckdialog.h
    src/health/blockmapwidget.cpp src/health/blockmapwidget.h
    src/common/jobui.cpp src/common/jobui.h
    src/common/flowlayout.cpp src/common/flowlayout.h
    src/backup/copydialogs.cpp src/backup/copydialogs.h
    src/tools/usagedialog.cpp src/tools/usagedialog.h
    src/viruses/virusdialog.cpp src/viruses/virusdialog.h
    src/viruses/hashlookup.cpp src/viruses/hashlookup.h
    src/backup/newidsdialog.cpp src/backup/newidsdialog.h
    src/tools/systemtools.cpp src/tools/systemtools.h
    src/drives/erasedialog.cpp src/drives/erasedialog.h
    src/addons/addonsdialog.cpp src/addons/addonsdialog.h
    src/addons/addonprompt.cpp src/addons/addonprompt.h
    src/addons/addonform.cpp src/addons/addonform.h
    src/addons/addonmaker.cpp src/addons/addonmaker.h
    src/addons/addonoutput.cpp src/addons/addonoutput.h
    src/app/noticebar.cpp src/app/noticebar.h
    src/drives/typedialog.cpp src/drives/typedialog.h
    src/drives/powerbox.cpp src/drives/powerbox.h
    src/drives/inspectdialog.cpp src/drives/inspectdialog.h
    src/drives/recoverdialog.cpp src/drives/recoverdialog.h
    src/addons/catalogdialog.cpp src/addons/catalogdialog.h
    src/app/about.cpp src/app/about.h
    src/app/updates.cpp src/app/updates.h
    src/app/translations.cpp src/app/translations.h
    src/app/theme.cpp src/app/theme.h
    src/app/thememaker.cpp src/app/thememaker.h
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
# Films and songs that play in Find Lost Files' viewer, when Qt Multimedia is there (releases have it).
option(DISKFORGE_MEDIA "Play films and songs in Find Lost Files' viewer (needs Qt Multimedia)" ON)
if(DISKFORGE_MEDIA)
    find_package(Qt6 6.8 QUIET COMPONENTS Multimedia MultimediaWidgets)
    if(Qt6Multimedia_FOUND AND Qt6MultimediaWidgets_FOUND)
        target_link_libraries(diskforge-ui PRIVATE Qt6::Multimedia Qt6::MultimediaWidgets)
        target_compile_definitions(diskforge-ui PRIVATE DISKFORGE_MEDIA)
    else()
        message(STATUS "Qt Multimedia not found: Find Lost Files' viewer shows films' details without playing them")
    endif()
endif()
if(WIN32)
    target_sources(diskforge-ui PRIVATE src/win/systemtools_win.cpp)
    target_link_libraries(diskforge-ui PRIVATE powrprof shell32)
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
    qt_add_executable(diskforge-selftest tests/selftest.cpp tests/testkit.cpp tests/testkit.h tests/test_fat32.cpp
        tests/test_gpt.cpp tests/test_copy.cpp tests/test_blockmap.cpp
        tests/test_usage.cpp tests/test_backup.cpp
        tests/test_rescue.cpp tests/test_clone.cpp tests/test_system.cpp
        tests/test_catalog.cpp tests/test_fuzz.cpp tests/test_health.cpp tests/test_jobs.cpp tests/test_recover.cpp tests/test_rescueusb.cpp tests/test_stickcheck.cpp tests/test_images.cpp tests/test_isomode.cpp tests/test_windows.cpp tests/test_lostfiles.cpp tests/test_viruses.cpp tests/test_registry.cpp tests/test_startup.cpp tests/test_boot.cpp tests/test_newids.cpp tests/testsign.cpp tests/testsign.h tests/test_raid.cpp tests/test_lvm.cpp)
    target_link_libraries(diskforge-selftest PRIVATE diskforge-ui OpenSSL::Crypto)
    target_compile_definitions(diskforge-selftest PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    qt_add_executable(diskforge-preview tests/preview.cpp)
    diskforge_add_data(diskforge-preview)
    target_link_libraries(diskforge-preview PRIVATE diskforge-ui)
    target_compile_definitions(diskforge-preview PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    qt_add_executable(diskforge-uitest tests/uitest.cpp tests/testsign.cpp tests/testsign.h)
    diskforge_add_data(diskforge-uitest)
    target_link_libraries(diskforge-uitest PRIVATE diskforge-ui OpenSSL::Crypto)
    target_compile_definitions(diskforge-uitest PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

# English plural forms ("1 sector", "2 sectors"); other languages get their own
# i18n/diskforge_<lang>.ts (see CONTRIBUTING.md).
set(TRANSLATED_TARGETS diskforge)
if(DISKFORGE_SELFTEST)
    # The window tests see the real English plurals too.
    list(APPEND TRANSLATED_TARGETS diskforge-preview diskforge-uitest)
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
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "xfsprogs, bubblewrap, fwupd, udisks2-lvm2, snapper, wimtools, clamav, qt6-image-formats-plugins, ffmpeg")
set(CPACK_STRIP_FILES ON)
include(CPack)
