cmake_minimum_required(VERSION 3.21)
project(Muffin LANGUAGES CXX)

# ---- Qt6 -------------------------------------------------------------------
# Core logic depends on QtCore (QString/QDir/QProcess); the reachability prober adds QtNetwork, and
# the widget UI adds QtGui/QtWidgets. Everything is Widgets-only — no QML/Quick anywhere.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Widgets Test)

# ---- zapret core library (no GUI) ------------------------------------------
# Parsing/locating is QtCore-only. The execution backends + manager are pure QtCore (QProcess), and
# PingService adds QtCore/QtNetwork — so the whole library still builds and tests headlessly on any
# platform. Only the Windows UAC / CREATE_NO_WINDOW paths touch the WinAPI (guarded by #ifdef).
add_library(zapretcore STATIC
    src/core/StrategyParser.cpp
    src/core/ZapretPaths.cpp
    src/core/ZapretLocator.cpp
    src/core/ZapretSource.cpp
    src/core/ZapretDownloader.cpp
    src/core/PrivilegeHelper.cpp
    src/core/ZapretManager.cpp
    src/core/PingService.cpp
    src/platform/WindowsBackend.cpp
    src/platform/LinuxBackend.cpp
)

# Headers carrying Q_OBJECT are listed so CMAKE_AUTOMOC generates their moc regardless of include
# order (IZapretBackend is an abstract interface only ever included, never a .cpp sibling).
target_sources(zapretcore PRIVATE
    src/core/Strategy.h
    src/core/IZapretBackend.h
    src/core/ZapretDownloader.h
    src/core/ZapretManager.h
    src/core/PingService.h
    src/platform/WindowsBackend.h
    src/platform/LinuxBackend.h
)

target_include_directories(zapretcore PUBLIC src/core src/platform)
target_link_libraries(zapretcore PUBLIC Qt6::Core Qt6::Network)

# Windows-only: ShellExecuteEx/ShellExecuteW (shell32) and token APIs (advapi32) used by
# PrivilegeHelper. They exist only on an MSVC/MinGW target and are omitted for host builds/tests.
if (WIN32)
    target_link_libraries(zapretcore PUBLIC shell32 advapi32)
endif()

# ---- the GUI ----------------------------------------------------------------
set(ZAPRET_UI_SOURCES
    src/ui/Theme.cpp
    src/ui/Settings.cpp
    src/ui/PingIndicator.cpp
    src/ui/StrategyCard.cpp
    src/ui/LogPanel.cpp
    src/ui/DownloadDialog.cpp
    src/ui/SettingsDialog.cpp
    src/ui/MainWindow.cpp
)

# Q_OBJECT headers are listed explicitly so AUTOMOC sees them even though their .cpp siblings are the
# only translation units that include them.
set(ZAPRET_UI_HEADERS
    src/ui/Theme.h
    src/ui/Settings.h
    src/ui/PingIndicator.h
    src/ui/StrategyCard.h
    src/ui/LogPanel.h
    src/ui/DownloadDialog.h
    src/ui/SettingsDialog.h
    src/ui/MainWindow.h
)

qt_add_resources(ZAPRET_GUI_RESOURCES src/resources/resources.qrc)

qt_add_executable(Muffin WIN32
    src/main.cpp
    ${ZAPRET_UI_SOURCES}
    ${ZAPRET_UI_HEADERS}
    ${ZAPRET_GUI_RESOURCES}
)

target_include_directories(Muffin PRIVATE src/ui)
target_link_libraries(Muffin PRIVATE zapretcore Qt6::Widgets Qt6::Gui Qt6::Network)

# ---- tests ------------------------------------------------------------------
# coretest: QtCore-only console verifier for the parser/locator/manager/backends.
add_executable(coretest tests/coretest.cpp)
target_link_libraries(coretest PRIVATE zapretcore)

# uitest: headless widget tests (QtTest + QSignalSpy). Runs under the offscreen platform; see the
# QT_PLUGIN_PATH logic below for hosts whose Qt prefix carries no platform plugins.
add_executable(uitest
    tests/uitest.cpp
    ${ZAPRET_UI_SOURCES}
    ${ZAPRET_UI_HEADERS}
    ${ZAPRET_GUI_RESOURCES}
)
target_include_directories(uitest PRIVATE src/ui)
target_link_libraries(uitest PRIVATE zapretcore Qt6::Widgets Qt6::Gui Qt6::Network Qt6::Test)

# ---- source encoding --------------------------------------------------------
# The UI strings are Cyrillic and are written as narrow literals inside QStringLiteral("...") (see
# src/ui/DownloadDialog.cpp, MainWindow.cpp, SettingsDialog.cpp, core/ZapretManager.cpp). Qt 6 reads such
# literals as UTF-8, so the bytes that reach the binary have to be UTF-8.
#
# MSVC does not assume that: without an explicit charset it decodes the source in the system ANSI
# codepage and re-encodes the execution charset the same way. Every Cyrillic literal then trips C4819
# and, worse, ships as mojibake — the dialog would read "ÑÐºÐ°ÑÑÑ" instead of "Скачать". The files are
# UTF-8 without a BOM, so /utf-8 (equivalent to /source-charset:utf-8 /execution-charset:utf-8) is the
# fix; adding a BOM to ~17 sources would work too but is far noisier and survives no editor by accident.
#
# Applied per target rather than add_compile_options() so it lands on the four targets that carry the
# literals and on nothing else. Generated sources inherit it from their owning target, which matters:
# AUTOMOC's moc_*.cpp and rcc's qrc_resources.cpp restate strings from the headers/resources and are
# compiled as part of these same targets. GCC/Clang already treat source and execution charsets as
# UTF-8 by default, so this block is deliberately MSVC-only and changes nothing on Linux.
if (MSVC)
    target_compile_options(Muffin PRIVATE /utf-8)
    target_compile_options(zapretcore PRIVATE /utf-8)
    target_compile_options(coretest PRIVATE /utf-8)
    target_compile_options(uitest PRIVATE /utf-8)
endif()

# ---- install rules ----------------------------------------------------------
# These exist only to give CPack something to package; a plain build (scripts/build.sh, CI configure +
# build + ctest) never touches them, and nothing here runs unless someone does `cmake --install` or
# `cpack`. The test targets are deliberately not installed.
install(TARGETS Muffin RUNTIME DESTINATION bin)

# Linux-only desktop integration. A Windows build tree has no use for an icon theme or a .desktop
# file, and the macOS case does not exist here, so the whole block is guarded — installing them on
# Windows would put a stray share/ next to Muffin.exe for Inno Setup to trip over.
#
# packaging/muffin.svg rather than src/resources/icons/app.svg: the in-app copy is a currentColor
# glyph that Theme::icon() recolours, which only works because Qt parses it — the desktop environment
# would get an unpainted shape. It is installed as muffin.svg to match the Icon= key of the entry.
if (UNIX AND NOT APPLE)
    install(FILES packaging/muffin.svg
            DESTINATION share/icons/hicolor/scalable/apps RENAME muffin.svg)
    install(FILES packaging/muffin.desktop
            DESTINATION share/applications)
endif()

enable_testing()

# The 0.1.1 identity migration reads and rewrites the per-user data and config locations, so both
# suites get those two locations redirected into the build tree: the alternative is a test run on a
# developer's machine moving their real ~/.local/share/Muffin tree and editing their real settings
# file. QStandardPaths takes both from the process environment and Qt caches that environment on first
# read, so the redirection has to exist before the process starts — hence here, not in the tests.
# Hosts that ignore XDG_* (Windows, macOS) cannot be redirected this way; each test checks whether the
# redirection took effect and skips the affected section rather than touch the real store.
set(ZAPRET_TEST_XDG
    "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home/share;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home/config")

add_test(NAME coretest COMMAND coretest)
set_tests_properties(coretest PROPERTIES ENVIRONMENT "${ZAPRET_TEST_XDG}")

add_test(NAME uitest COMMAND uitest)

# A Qt installation can expose its libraries without the matching platform plugins (this project's
# user-local prefix does exactly that). Locate a plugin directory holding the offscreen platform and
# hand it to the test so ctest works without a display server.
find_file(ZAPRET_OFFSCREEN_PLUGIN NAMES libqoffscreen.so qoffscreen.dll
          PATHS "${Qt6_DIR}/../../../plugins/platforms"
                "$ENV{HOME}/.local/zapretdev/usr/plugins/platforms"
                "/usr/lib/x86_64-linux-gnu/qt6/plugins/platforms"
                "/usr/lib/qt6/plugins/platforms"
          NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if (ZAPRET_OFFSCREEN_PLUGIN)
    get_filename_component(ZAPRET_PLUGIN_PLATFORM_DIR "${ZAPRET_OFFSCREEN_PLUGIN}" DIRECTORY)
    get_filename_component(ZAPRET_PLUGIN_DIR "${ZAPRET_PLUGIN_PLATFORM_DIR}" DIRECTORY)
    set_tests_properties(uitest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${ZAPRET_PLUGIN_DIR};${ZAPRET_TEST_XDG}"
        TIMEOUT 120)
else()
    set_tests_properties(uitest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${ZAPRET_TEST_XDG}" TIMEOUT 120)
endif()

# ---- packaging (CPack) ------------------------------------------------------
# Version stamped into the packages. A cache variable so a tag can override it without editing this
# file: the release job passes -DMUFFIN_VERSION="${tag without the leading v}", which is what keeps
# muffin_<ver>_amd64.deb, Muffin-Setup-<ver>.exe and the tag themselves in agreement. The default is
# the current release line, so local `cpack` runs still produce a sensibly named file.
set(MUFFIN_VERSION "0.1.2" CACHE STRING "Version stamped into the generated packages")

set(CPACK_PACKAGE_NAME "muffin")
set(CPACK_PACKAGE_VENDOR "F0RSES")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Qt6 GUI for zapret")
set(CPACK_PACKAGE_VERSION "${MUFFIN_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Muffin")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "F0RSES")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/F0RSES/muffin-gui")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
# Minimum versions, not exact ones, and deliberately pinned above the distro archive: this project's
# real floor is Qt 6.6+ (QPalette::Accent, QStyleHints::colorScheme) and CI builds against 6.7.3. A
# plain "libqt6widgets6" would let apt resolve to the Ubuntu 22.04/24.04 Qt (6.2 / 6.4) and the binary
# would then die at startup with unresolved symbols — worse than an install-time dependency error.
# (An alternative that avoids the question entirely: ship an AppImage bundling Qt; not done yet.)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6widgets6 (>= 6.7), libqt6network6 (>= 6.7), libqt6svg6 (>= 6.7)")

# Debian wants the machine-readable architecture, and CMake's CMAKE_SYSTEM_PROCESSOR gives the kernel
# name. Mapped explicitly so the file name is deterministic (muffin_0.1.0_amd64.deb) instead of
# whatever the host's dpkg defaults happen to be.
if (NOT DEFINED CPACK_DEBIAN_PACKAGE_ARCHITECTURE)
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
        set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
    elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
    elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^i[3-6]86$")
        set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "i386")
    endif()
endif()

# Without this, CPack names the file after its own "<name>-<version>-<system>" default — locally that
# came out as muffin-0.1.0-Linux.deb, which is not a Debian file name and is not what the release job
# attaches. Underscored name_version_arch is what apt/dpkg and every other Debian package use.
if (DEFINED CPACK_DEBIAN_PACKAGE_ARCHITECTURE)
    set(CPACK_PACKAGE_FILE_NAME
        "${CPACK_PACKAGE_NAME}_${MUFFIN_VERSION}_${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}")
endif()

# Default generator per platform, but only when the caller did not choose one: the release job passes
# -DCPACK_GENERATOR=DEB, and `cpack -G DEB` also works. Guarded because CPack's own default (TGZ on
# Unix) is the right answer on a host without dpkg-deb, where a DEB default would only produce an error.
if (NOT CPACK_GENERATOR)
    find_program(CPACK_DPKG_DEB_PROGRAM dpkg-deb)
    if (UNIX AND NOT APPLE AND CPACK_DPKG_DEB_PROGRAM)
        set(CPACK_GENERATOR "DEB")
    endif()
endif()

include(CPack)

