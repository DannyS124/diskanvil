# SPDX-FileCopyrightText: 2026 Danny S
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.21)
project(diskanvil
    VERSION 0.5.2
    DESCRIPTION "Disk manager for Linux in the style of Windows Disk Management"
    HOMEPAGE_URL "https://github.com/DannyS124/diskanvil"
    LANGUAGES CXX)

set(APP_ID "io.github.DannyS124.DiskAnvil")
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
option(DISKANVIL_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
if(DISKANVIL_SANITIZE)
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
add_library(diskanvil-core STATIC
    src/drives/udisks.h src/drives/udisks_common.cpp
    src/drives/fsprobe.cpp src/drives/fsprobe.h
    src/common/format.cpp src/common/format.h
    src/drives/blockio.cpp src/drives/blockio.h
    src/tools/systemd.h
    src/usb/imagewriter.cpp src/usb/imagewriter.h
    src/usb/imagesource.cpp src/usb/imagesource.h
    src/lostfiles/lostsource.cpp src/lostfiles/lostsource.h
    src/lostfiles/lostscan.cpp src/lostfiles/lostscan.h
    src/lostfiles/timeline.cpp src/lostfiles/timeline.h
    src/lostfiles/lostfs.cpp src/lostfiles/lostfs.h src/lostfiles/lostntfs.cpp src/lostfiles/lostraw.h
    src/lostfiles/unpack.cpp src/lostfiles/unpack.h
    src/lostfiles/places.cpp src/lostfiles/places.h
    src/lostfiles/foundreport.cpp src/lostfiles/foundreport.h
    src/lostfiles/vshadow.cpp src/lostfiles/vshadow.h
    src/lostfiles/vdisk.cpp src/lostfiles/vdisk.h
    src/drives/fat32format.cpp src/drives/fat32format.h
    src/drives/exfatformat.cpp src/drives/exfatformat.h
    src/drives/sdcard.cpp src/drives/sdcard.h
    src/drives/luksheader.cpp src/drives/luksheader.h
    src/drives/closedsets.cpp src/drives/closedsets.h
    src/drives/erasure.cpp src/drives/erasure.h
    src/drives/usbinfo.cpp src/drives/usbinfo.h
    src/drives/writeblock.cpp src/drives/writeblock.h
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
    src/viruses/efiboot.cpp src/viruses/efiboot.h
    src/viruses/bootcheck.cpp src/viruses/bootcheck.h
    src/viruses/productkey.cpp src/viruses/productkey.h
    src/backup/savefiles.cpp src/backup/savefiles.h
    src/tools/linuxboot.cpp src/tools/linuxboot.h
    src/tools/tryrepair.cpp src/tools/tryrepair.h
    src/tools/winre.cpp src/tools/winre.h
    src/tools/testboot.cpp src/tools/testboot.h
    src/viruses/peinfo.cpp src/viruses/peinfo.h
    src/viruses/wof.cpp src/viruses/wof.h
    src/viruses/activity.cpp src/viruses/activity.h
    src/backup/newids.cpp src/backup/newids.h
    src/lostfiles/filetypes.cpp src/lostfiles/filetypes.h
    src/usb/checksums.cpp src/usb/checksums.h
    src/health/benchmark.cpp src/health/benchmark.h
    src/addons/addons.cpp src/addons/addons.h
    src/health/surfacescan.cpp src/health/surfacescan.h
    src/drives/gpt.cpp src/drives/gpt.h
    src/backup/blockcopy.cpp src/backup/blockcopy.h
    src/backup/zstdseek.cpp src/backup/zstdseek.h
    src/backup/partclone.cpp src/backup/partclone.h
    src/backup/ewf.cpp src/backup/ewf.h
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
    src/health/farm.cpp src/health/farm.h
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
    src/usb/liveonpc.cpp src/usb/liveonpc.h
    src/usb/stickcheck.cpp src/usb/stickcheck.h
    src/drives/partscan.cpp src/drives/partscan.h
)
if(WIN32)
    target_sources(diskanvil-core PRIVATE
        src/win/blockio_win.cpp
        src/win/udisks_win.cpp
        src/win/winutil.cpp src/win/winutil.h
        src/win/winstartup_win.cpp
        src/win/unavailable.cpp)
    target_link_libraries(diskanvil-core PUBLIC Qt6::Core PRIVATE PkgConfig::ZSTD PkgConfig::ZLIB PkgConfig::ARCHIVE OpenSSL::Crypto
        setupapi cfgmgr32 ole32 oleaut32 wbemuuid uuid virtdisk)
    # Large files and the Windows 10 API (CM_Register_Notification, storage queries).
    target_compile_definitions(diskanvil-core PUBLIC _FILE_OFFSET_BITS=64 _WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE NOMINMAX)
else()
    target_sources(diskanvil-core PRIVATE
        src/drives/udisks.cpp src/drives/operations.cpp src/drives/dbusnames.h
        src/tools/systemd.cpp src/tools/snapper.cpp src/health/firmware.cpp)
    target_link_libraries(diskanvil-core PUBLIC Qt6::Core Qt6::DBus PRIVATE PkgConfig::ZSTD PkgConfig::ZLIB PkgConfig::BLKID PkgConfig::ARCHIVE OpenSSL::Crypto)
endif()
# The sources sit in folders by feature and include each other by file name.
target_include_directories(diskanvil-core PUBLIC src src/addons src/app src/backup src/common src/drives src/health src/lostfiles src/tools src/usb src/viruses)
target_compile_definitions(diskanvil-core PUBLIC ${APP_DEFINITIONS})
target_compile_options(diskanvil-core PRIVATE -Wall -Wextra)

# Windows and dialogs, shared by the app, the dialog preview and the window test.
add_library(diskanvil-ui STATIC
    src/app/diskmap.cpp src/app/diskmap.h
    src/app/homewindow.cpp src/app/homewindow.h
    src/app/whathappened.cpp src/app/whathappened.h
    src/lostfiles/lostfilesdialog.cpp src/lostfiles/lostfilesdialog.h
    src/lostfiles/viewerhost.cpp src/lostfiles/viewerhost.h
    src/lostfiles/lookcloserdialog.cpp src/lostfiles/lookcloserdialog.h
    src/tools/filesdialog.cpp src/tools/filesdialog.h
    src/lostfiles/timelinedialog.cpp src/lostfiles/timelinedialog.h
    src/lostfiles/placesmap.cpp src/lostfiles/placesmap.h
    src/lostfiles/enhance.cpp src/lostfiles/enhance.h
    src/viruses/activitydialog.cpp src/viruses/activitydialog.h
    src/lostfiles/placesdialog.cpp src/lostfiles/placesdialog.h
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
    src/usb/liveonpcdialog.cpp src/usb/liveonpcdialog.h
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
    src/common/pdfreport.cpp src/common/pdfreport.h
    src/app/certificates.cpp src/app/certificates.h
    src/backup/copydialogs.cpp src/backup/copydialogs.h
    src/tools/usagedialog.cpp src/tools/usagedialog.h
    src/viruses/virusdialog.cpp src/viruses/virusdialog.h
    src/viruses/hashlookup.cpp src/viruses/hashlookup.h
    src/viruses/windowspicker.cpp src/viruses/windowspicker.h
    src/viruses/productkeydialog.cpp src/viruses/productkeydialog.h
    src/viruses/driversdialog.cpp src/viruses/driversdialog.h
    src/viruses/bootentriesdialog.cpp src/viruses/bootentriesdialog.h
    src/backup/savefilesdialog.cpp src/backup/savefilesdialog.h
    src/backup/clonezilladialog.cpp src/backup/clonezilladialog.h
    src/backup/evidencedialog.cpp src/backup/evidencedialog.h
    src/tools/linuxbootdialog.cpp src/tools/linuxbootdialog.h
    src/tools/tryrepairdialog.cpp src/tools/tryrepairdialog.h
    src/tools/winredialog.cpp src/tools/winredialog.h
    src/tools/testbootdialog.cpp src/tools/testbootdialog.h
    src/tools/remotehelpdialog.cpp src/tools/remotehelpdialog.h
    src/tools/remotehelp.cpp src/tools/remotehelp.h
    src/tools/netboot.cpp src/tools/netboot.h
    src/tools/netbootdialog.cpp src/tools/netbootdialog.h
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
    src/app/oldname.cpp src/app/oldname.h
    src/app/thememaker.cpp src/app/thememaker.h
)
target_link_libraries(diskanvil-ui PUBLIC diskanvil-core Qt6::Widgets Qt6::Network)
# PDF pages in Find Lost Files' previews, when poppler's Qt 6 library is there (releases have it).
option(DISKANVIL_PDF "Show PDF pages in Find Lost Files (needs poppler-qt6)" ON)
if(DISKANVIL_PDF)
    pkg_check_modules(POPPLER IMPORTED_TARGET poppler-qt6)
    if(POPPLER_FOUND)
        target_link_libraries(diskanvil-ui PRIVATE PkgConfig::POPPLER)
        target_compile_definitions(diskanvil-ui PRIVATE DISKANVIL_PDF)
    else()
        message(STATUS "poppler-qt6 not found: Find Lost Files shows PDFs without their pages")
    endif()
endif()
# Films and songs that play in Find Lost Files' viewer, when Qt Multimedia is there (releases have it).
option(DISKANVIL_MEDIA "Play films and songs in Find Lost Files' viewer (needs Qt Multimedia)" ON)
if(DISKANVIL_MEDIA)
    find_package(Qt6 6.8 QUIET COMPONENTS Multimedia MultimediaWidgets)
    if(Qt6Multimedia_FOUND AND Qt6MultimediaWidgets_FOUND)
        target_link_libraries(diskanvil-ui PRIVATE Qt6::Multimedia Qt6::MultimediaWidgets)
        target_compile_definitions(diskanvil-ui PUBLIC DISKANVIL_MEDIA)
    else()
        message(STATUS "Qt Multimedia not found: Find Lost Files' viewer shows films' details without playing them")
    endif()
endif()
if(WIN32)
    target_sources(diskanvil-ui PRIVATE src/win/systemtools_win.cpp)
    target_link_libraries(diskanvil-ui PRIVATE powrprof shell32)
endif()
target_compile_options(diskanvil-ui PRIVATE -Wall -Wextra)

# Help page, icon and license text, built into each program that shows them.
function(diskanvil_add_data target)
    qt_add_resources(${target} "${target}_data" PREFIX "/" FILES data/help.html data/${APP_ID}.svg LICENSE)
    qt_add_resources(${target} "${target}_themes" PREFIX "/themes" BASE data/themes
        FILES data/themes/classic.json data/themes/deadshadow.json data/themes/diskanvil-live.json data/themes/high-contrast.json)
    # Place names and a world map, for where photos were taken (data/places/ATTRIBUTION.txt).
    qt_add_resources(${target} "${target}_places" PREFIX "/places" BASE data/places
        FILES data/places/cities.tsv data/places/countries.tsv data/places/land.bin data/places/borders.bin)
endfunction()

qt_add_executable(diskanvil src/main.cpp)
diskanvil_add_data(diskanvil)
if(WIN32)
    # Its own window (no console), the icon and version details, and it asks to run as administrator.
    set_target_properties(diskanvil PROPERTIES WIN32_EXECUTABLE ON)
    configure_file(data/windows/diskanvil.rc.in diskanvil.rc @ONLY)
    target_sources(diskanvil PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/diskanvil.rc)
endif()
target_link_libraries(diskanvil PRIVATE diskanvil-ui)
target_compile_options(diskanvil PRIVATE -Wall -Wextra)

# DiskAnvil Live's own screen (a Wayland compositor): Linux, and only where Qt's compositor is.
if(NOT WIN32)
    # Only DiskAnvil Live needs it (the .deb it's made from turns it on): a PC has its own desktop.
    option(DISKANVIL_SHELL "Build diskanvil-shell, DiskAnvil Live's own screen (needs Qt Wayland Compositor)" OFF)
endif()
if(DISKANVIL_SHELL)
    find_package(Qt6 6.8 QUIET COMPONENTS Quick WaylandCompositor Test)
    if(Qt6WaylandCompositor_FOUND AND Qt6Quick_FOUND AND Qt6Test_FOUND)
        if(QT_KNOWN_POLICY_QTP0004)
            qt_policy(SET QTP0004 NEW)
        endif()
        qt_add_executable(diskanvil-shell src/shell/main.cpp src/shell/shell.cpp src/shell/shell.h src/shell/selftest.cpp src/shell/selftest.h
                          src/shell/network.cpp src/shell/network.h)
        # The QML at the module's top (qrc:/qt/qml/DiskAnvilShell/Main.qml), the same with every Qt 6 from 6.8.
        set(shell_qml Main ShellWindow TopBar Dock AppGrid PowerMenu)
        set(shell_qml_files)
        foreach(name ${shell_qml})
            set_source_files_properties(src/shell/qml/${name}.qml PROPERTIES QT_RESOURCE_ALIAS ${name}.qml)
            list(APPEND shell_qml_files src/shell/qml/${name}.qml)
        endforeach()
        qt_add_qml_module(diskanvil-shell URI DiskAnvilShell VERSION 1.0 RESOURCE_PREFIX /qt/qml QML_FILES ${shell_qml_files})
        file(GLOB shell_icons ${CMAKE_CURRENT_SOURCE_DIR}/rescue/icons/DiskAnvil-Live/scalable/apps/diskanvil-live-*.svg)
        foreach(icon ${shell_icons})
            get_filename_component(name ${icon} NAME)
            set_source_files_properties(${icon} PROPERTIES QT_RESOURCE_ALIAS icons/${name})
        endforeach()
        set_source_files_properties(rescue/art/desktop.svg PROPERTIES QT_RESOURCE_ALIAS art/desktop.svg)
        set_source_files_properties(rescue/art/diskanvil-live-icon.svg PROPERTIES QT_RESOURCE_ALIAS art/diskanvil-live-icon.svg)
        set_source_files_properties(rescue/art/pointer.svg PROPERTIES QT_RESOURCE_ALIAS art/pointer.svg)
        qt_add_resources(diskanvil-shell shell_art PREFIX /shell FILES ${shell_icons} rescue/art/desktop.svg rescue/art/diskanvil-live-icon.svg rescue/art/pointer.svg)
        target_compile_definitions(diskanvil-shell PRIVATE DISKANVIL_VERSION="${PROJECT_VERSION}")
        target_link_libraries(diskanvil-shell PRIVATE Qt6::Gui Qt6::Quick Qt6::WaylandCompositor Qt6::Test Qt6::DBus)
        target_compile_options(diskanvil-shell PRIVATE -Wall -Wextra)
        install(TARGETS diskanvil-shell RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    else()
        message(STATUS "diskanvil-shell isn't built: Qt Wayland Compositor wasn't found")
    endif()
endif()


# dev tools, not installed
if(WIN32)
    option(DISKANVIL_SELFTEST "Build the loop-device self-test" OFF)
else()
    option(DISKANVIL_SELFTEST "Build the loop-device self-test" ON)
endif()
if(DISKANVIL_SELFTEST)
    qt_add_executable(diskanvil-selftest tests/selftest.cpp tests/testkit.cpp tests/testkit.h tests/test_fat32.cpp tests/test_luks.cpp tests/test_savefiles.cpp
        tests/test_gpt.cpp tests/test_copy.cpp tests/test_blockmap.cpp
        tests/test_usage.cpp tests/test_backup.cpp
        tests/test_rescue.cpp tests/test_clone.cpp tests/test_system.cpp
        tests/test_catalog.cpp tests/test_fuzz.cpp tests/test_health.cpp tests/test_jobs.cpp tests/test_recover.cpp tests/test_rescueusb.cpp tests/test_stickcheck.cpp tests/test_images.cpp tests/test_isomode.cpp tests/test_windows.cpp tests/test_lostfiles.cpp tests/test_viruses.cpp tests/test_registry.cpp tests/test_startup.cpp tests/test_boot.cpp tests/test_newids.cpp tests/testsign.cpp tests/testsign.h tests/test_raid.cpp tests/test_lvm.cpp)
    diskanvil_add_data(diskanvil-selftest)
    target_link_libraries(diskanvil-selftest PRIVATE diskanvil-ui OpenSSL::Crypto)
    target_compile_definitions(diskanvil-selftest PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    qt_add_executable(diskanvil-preview tests/preview.cpp)
    diskanvil_add_data(diskanvil-preview)
    target_link_libraries(diskanvil-preview PRIVATE diskanvil-ui)
    target_compile_definitions(diskanvil-preview PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    qt_add_executable(diskanvil-uitest tests/uitest.cpp tests/testsign.cpp tests/testsign.h)
    diskanvil_add_data(diskanvil-uitest)
    target_link_libraries(diskanvil-uitest PRIVATE diskanvil-ui OpenSSL::Crypto)
    target_compile_definitions(diskanvil-uitest PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

# English plural forms ("1 sector", "2 sectors"); other languages get their own
# i18n/diskanvil_<lang>.ts (see CONTRIBUTING.md).
set(TRANSLATED_TARGETS diskanvil)
if(DISKANVIL_SELFTEST)
    # The window tests see the real English plurals too.
    list(APPEND TRANSLATED_TARGETS diskanvil-preview diskanvil-uitest)
endif()
qt_add_translations(TARGETS ${TRANSLATED_TARGETS}
    SOURCE_TARGETS diskanvil-core diskanvil-ui diskanvil
    TS_FILE_DIR i18n
    PLURALS_TS_FILE i18n/diskanvil_en.ts
    RESOURCE_PREFIX /i18n)

include(GNUInstallDirs)
if(WIN32)
    # The rest (DLLs, Qt plugins) is put next to it by packaging/windows/deploy.sh.
    install(TARGETS diskanvil RUNTIME DESTINATION .)
    return()
endif()
configure_file(data/${APP_ID}.desktop.in ${APP_ID}.desktop @ONLY)
configure_file(data/${APP_ID}.metainfo.xml.in ${APP_ID}.metainfo.xml @ONLY)
configure_file(data/diskanvil.1.in diskanvil.1 @ONLY)

install(TARGETS diskanvil RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES data/${APP_ID}.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/diskanvil.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
# Disk Cleanup starts these to shrink the journal and the package cache; see the files for why they're units.
# Off for Flatpak and AppImage builds, which can't add system units.
option(DISKANVIL_SYSTEMD_UNIT "Install the journal cleanup unit" ON)
if(DISKANVIL_SYSTEMD_UNIT)
    pkg_get_variable(SYSTEMD_UNIT_DIR systemd systemd_system_unit_dir)
    if(NOT SYSTEMD_UNIT_DIR)
        set(SYSTEMD_UNIT_DIR ${CMAKE_INSTALL_PREFIX}/lib/systemd/system)
    endif()
    install(FILES data/diskanvil-journal-vacuum.service data/diskanvil-paccache-uninstalled.service DESTINATION ${SYSTEMD_UNIT_DIR})
endif()

# A .deb for Debian and Ubuntu: `cpack -G DEB` in the build folder (packaging/deb/build.sh does it in a
# clean Debian container). Library dependencies come from dpkg-shlibdeps; what it can't see is listed here.
set(CPACK_PACKAGE_NAME diskanvil)
set(CPACK_PACKAGE_VENDOR "Danny S")
set(CPACK_PACKAGE_CONTACT "Danny S <48356907+DannyS124@users.noreply.github.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Disk manager and partition editor that works like Windows Disk Management")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/DannyS124/diskanvil")
set(CPACK_DEBIAN_PACKAGE_SECTION admin)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "udisks2, polkitd, qt6-svg-plugins")
# It was called DiskForge up to 0.5.1: this one takes its place.
set(CPACK_DEBIAN_PACKAGE_REPLACES "diskforge")
set(CPACK_DEBIAN_PACKAGE_BREAKS "diskforge")
set(CPACK_DEBIAN_PACKAGE_PROVIDES "diskforge")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "dosfstools, exfatprogs, ntfs-3g, btrfs-progs")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "xfsprogs, bubblewrap, fwupd, udisks2-lvm2, snapper, wimtools, clamav, qt6-image-formats-plugins, ffmpeg, smartmontools, sqlite3, mdadm, lvm2, efibootmgr, qemu-system-x86, ovmf, ldmtool")
set(CPACK_STRIP_FILES ON)
include(CPack)
