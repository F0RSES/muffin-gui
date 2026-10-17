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

enable_testing()
add_test(NAME coretest COMMAND coretest)
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
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${ZAPRET_PLUGIN_DIR}"
        TIMEOUT 120)
else()
    set_tests_properties(uitest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
endif()
