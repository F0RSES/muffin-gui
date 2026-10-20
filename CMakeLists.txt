cmake_minimum_required(VERSION 3.21)
project(Muffin LANGUAGES CXX)

# Version stamped into the packages and into the root helper's --help output. Declared here, at the
# top, because two sections below need it: the executable target is built with it as a compile
# definition, and CPack uses it for the package version. A CACHE variable so a tag can override it
# without editing this file: the release job passes -DMUFFIN_VERSION="${tag without the leading v}",
# which is what keeps muffin_<ver>_amd64.deb, Muffin-Setup-<ver>.exe and the tag themselves in
# agreement. The default is the current release line, so local `cmake`/`cpack` runs still produce
# sensibly named files and a helper that reports a real version.
set(MUFFIN_VERSION "0.4.1" CACHE STRING "Version stamped into the generated packages")

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
    src/core/MuffinInstallHelper.cpp
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

# ---- the root helper --------------------------------------------------------
# The program zapret::MuffinInstallHelper describes and the GUI execs through pkexec: it takes a
# downloaded linux tree and installs it into ZapretPaths::systemInstallDir() as root, chown'ing the
# result to root:root and stripping the group/world write bits. Those permissions are the reason it
# exists — packaging/postinst's polkit rule grants passwordless execution to a payload under that
# tree, and the grant is only sound while an unprivileged account cannot replace the bytes.
#
# A compiled binary rather than a shell script, because `pkexec /bin/sh install_bin.sh` puts an
# interpreter on the authorised path: the interpreter runs whatever the script says, and the script
# sits in a directory the caller owns. This target has no degrees of freedom — one argument, a fixed
# destination read from ZapretPaths, and its own validation.
#
# POSIX-only: Windows elevates winws.exe through UAC from wherever it lives, and
# ZapretPaths::systemInstallDir() is empty there, so there would be nothing for it to do.
#
# The DESTINATION is part of the security contract, not a layout preference:
# MuffinInstallHelper::installedProgram() returns this exact path, the GUI execs it through pkexec,
# and packaging/check-polkit-rule.js reads `lib/muffin` back out of this file to assert the rule
# refuses that path — promoting user-written bytes into a passwordless tree is the one moment a human
# must approve. Renaming the destination without updating the rule check is a build failure, not a
# drift.
if (UNIX AND NOT APPLE)
    add_executable(muffin-install-helper src/tools/muffin-install-helper.cpp)
    target_link_libraries(muffin-install-helper PRIVATE zapretcore)
    target_compile_definitions(muffin-install-helper PRIVATE MUFFIN_VERSION="${MUFFIN_VERSION}")
    install(TARGETS muffin-install-helper RUNTIME DESTINATION lib/muffin)

    # The build tree has to present the same permissions as the package. install() ships a program
    # 0755, but the link step inherits the developer's umask — and under the common umask 002 that is
    # 0775, group-writable, which MuffinInstallHelper::findProgram() correctly refuses to exec as
    # root. Without this the helper would be invisible to a download run straight out of build/ while
    # being found by an apt user, and programCandidates()'s whole reason for existing (one code path
    # for both) would quietly not hold.
    if (NOT WIN32)
        add_custom_command(TARGET muffin-install-helper POST_BUILD
            COMMAND chmod 755 $<TARGET_FILE:muffin-install-helper>
            COMMENT "Matching the packaged 0755 mode on the build-tree helper")
    endif()
endif()

# ---- the GUI ----------------------------------------------------------------
set(ZAPRET_UI_SOURCES
    src/ui/Theme.cpp
    src/ui/LanguageManager.cpp
    src/ui/Settings.cpp
    src/ui/PingIndicator.cpp
    src/ui/PowerButton.cpp
    src/ui/ProfileCard.cpp
    src/ui/LogPanel.cpp
    src/ui/DownloadDialog.cpp
    src/ui/SettingsPage.cpp
    src/ui/MainWindow.cpp
)

# Q_OBJECT headers are listed explicitly so AUTOMOC sees them even though their .cpp siblings are the
# only translation units that include them.
set(ZAPRET_UI_HEADERS
    src/ui/Theme.h
    src/ui/LanguageManager.h
    src/ui/Settings.h
    src/ui/PingIndicator.h
    src/ui/PowerButton.h
    src/ui/ProfileCard.h
    src/ui/LogPanel.h
    src/ui/DownloadDialog.h
    src/ui/SettingsPage.h
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

# ---- Windows resources: icon + elevation manifest ---------------------------
# Everything a Windows executable has to expose before its code runs lives in
# packaging/windows/app.rc: the icon group the shell reads for the file (Start menu,
# desktop, taskbar and every shortcut that does not name an icon of its own), and the
# RT_MANIFEST that carries requireAdministrator. Neither can come from the .qrc — Qt
# resources are invisible to Explorer and to CreateProcess, which is why the icon was
# missing in exactly those three places and why "run as administrator" was still a
# thing the user had to remember: WindowsBackend::start() refuses an elevated-required
# strategy unless the token it already holds is elevated, and up to now nothing but the
# user's right-click guaranteed that.
#
# app.rc names its two payloads by relative path and the tools disagree about what
# that means. Measured here: windres resolves them against the directory holding the
# .rc itself, so it compiles from any working directory; rc.exe documents the current
# directory instead, which for a CMake build is the binary dir rather than the source
# dir. Staging copies there is two lines and makes the same script compile under
# either rule — from an IDE, from `cmake --build`, or by hand inside
# packaging/windows/. It is also self-correcting: configure_file() re-stages whenever
# CMake re-runs, so a build-tree copy that drifts is repaired before anything reads
# it (verified by hand-corrupting build/app.manifest — the exe still embedded the
# source's requireAdministrator). The payload is then named twice on purpose, because
# the two copies answer different questions: the configure_file() copy is what the
# compiler may read, and OBJECT_DEPENDS on the SOURCE copies is what makes editing the
# artwork or the manifest in the checkout rebuild the resource.
#
# packaging/check-win-resources.py asserts all of the above from the other side, on every push
# and PR: that both payloads app.rc names are staged here and guarded here, that the RC language
# is enabled, that OBJECT_DEPENDS covers them, and that /MANIFEST:NO stays confined to MSVC. It
# reads the file with comments stripped, so mentioning a flag here does not count as setting it.
if (WIN32)
    enable_language(RC)
    set(MUFFIN_RC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows")
    foreach (muffin_rc_file IN ITEMS app.rc app.manifest muffin.ico)
        # Missing here means shipping an exe that either has no brand or runs unelevated,
        # and both look like a working build until a user hits it. Fail at configure.
        if (NOT EXISTS "${MUFFIN_RC_DIR}/${muffin_rc_file}")
            message(FATAL_ERROR "packaging/windows/${muffin_rc_file} is missing")
        endif()
    endforeach()

    configure_file("${MUFFIN_RC_DIR}/muffin.ico"   "${CMAKE_CURRENT_BINARY_DIR}/muffin.ico"   COPYONLY)
    configure_file("${MUFFIN_RC_DIR}/app.manifest" "${CMAKE_CURRENT_BINARY_DIR}/app.manifest" COPYONLY)
    set_source_files_properties("${MUFFIN_RC_DIR}/app.rc" PROPERTIES
        OBJECT_DEPENDS "${MUFFIN_RC_DIR}/muffin.ico;${MUFFIN_RC_DIR}/app.manifest")

    target_sources(Muffin PRIVATE "${MUFFIN_RC_DIR}/app.rc")

    # MSVC only. CMake's VS generator maps "/MANIFEST:NO" to <GenerateManifest>false</>
    # (Templates/MSBuild/FlagTables/v143_Link.json), which stops link.exe from emitting its
    # own manifest and mt.exe from embedding it over ours — an mt embed replaces resource
    # #1/RT_MANIFEST and would quietly put the level back to asInvoker. MinGW has no
    # manifest step to suppress and does not understand the flag, so it stays out of there.
    if (MSVC)
        set_target_properties(Muffin PROPERTIES LINK_FLAGS "/MANIFEST:NO")
    endif()
endif()

# ---- tests ------------------------------------------------------------------
# coretest: QtCore-only console verifier for the parser/locator/manager/backends.
add_executable(coretest tests/coretest.cpp)
target_link_libraries(coretest PRIVATE zapretcore)
# The root helper stamps MUFFIN_VERSION into its --help and --version output, and coretest asserts the
# stamp actually reached the binary. It needs the same value the helper target was given to compare
# against, or "the version is stamped" would be checking a string against nothing.
target_compile_definitions(coretest PRIVATE MUFFIN_VERSION="${MUFFIN_VERSION}")

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
# src/ui/DownloadDialog.cpp, MainWindow.cpp, SettingsPage.cpp, core/ZapretManager.cpp). Qt 6 reads such
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
# glyph that Theme::icon() recolours, which only works because Qt parses it — a hicolor icon is read by
# the desktop environment's own SVG loader, which has no recolouring step, so installing app.svg under
# the name "muffin" would put an unpainted (or theme-tinted) outline in the launcher. The packaged copy
# carries the same artwork with explicit paints, and it is installed under the name the .desktop entry's
# Icon=muffin key resolves to. Verified with `DESTDIR=/tmp/stage cmake --install build`: the two files
# land as share/icons/hicolor/scalable/apps/muffin.svg and share/applications/muffin.desktop.
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
# MUFFIN_VERSION — the version stamped into the packages — is set once, at the top of this file, so the
# root helper target and CPack can never be told two different things. The release job overrides it
# with -DMUFFIN_VERSION="${tag without the leading v}", which is what keeps muffin_<ver>_amd64.deb,
# Muffin-Setup-<ver>.exe and the tag themselves in agreement.

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

# Everything privileged in LinuxBackend goes through pkexec, and a desktop polkit agent (Muffin's
# included) authenticates every one of those calls — so a start/stop cycle asked for the password on
# several phases at once. packaging/postinst installs a rules.d entry so that authorisation happens
# once, at `apt install`, where dpkg is already root and the admin has just approved the install.
# CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA is how a DEB package gets maintainer scripts, and packaging/postrm
# is its counterpart: the rule lives in /etc, which is not in the package's file list, so dpkg would
# otherwise leave a passwordless iptables entry behind after the application was removed.
#
# The rule authorises only programs whose bytes a non-root user cannot replace (exact /usr/sbin
# binaries, and a payload under the root-owned /usr/local/zapret or /opt/zapret trees). It is NOT a
# name match: `program` is the binary pkexec execs, so exempting "anything ending in /nfqws" would
# exempt a file the user wrote himself in his own home — a local root. See the header in postinst.
#
# Kept UNIX-only for the same reason the install() rules and the DEB default above are: a Windows tree
# has no dpkg to receive the script, and the script is /bin/sh. The path must be absolute.
#
# dpkg will not configure a package whose postinst is not executable, and CPack copies the file
# verbatim — permissions included — so the exec bit is re-asserted at configure time instead of being
# trusted to whatever the checkout happened to preserve. An unguarded file(CHMOD) is deliberate: if
# a script goes missing, configuring should fail loudly rather than quietly ship a package that
# asks for a password again, or one that leaves its rule installed forever.
if (UNIX AND NOT APPLE)
    file(CHMOD "${CMAKE_SOURCE_DIR}/packaging/postinst" "${CMAKE_SOURCE_DIR}/packaging/postrm"
         PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                     GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_SOURCE_DIR}/packaging/postinst;${CMAKE_SOURCE_DIR}/packaging/postrm")
endif()

include(CPack)

