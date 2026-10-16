cmake_minimum_required(VERSION 3.21)
project(zapret_gui LANGUAGES CXX)

# ---- Qt6 -------------------------------------------------------------------
# Core logic depends only on QtCore (QString/QDir/QFileInfo). Widgets come later.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core)

# ---- zapret core library (no GUI) ------------------------------------------
# Parsing/locating is QtCore-only. The execution backends + manager are also pure QtCore
# (QProcess), so the whole library builds headlessly on any platform; only the Windows UAC /
# CREATE_NO_WINDOW paths touch the WinAPI (guarded by #ifdef in the .cpp).
add_library(zapretcore STATIC
    src/core/StrategyParser.cpp
    src/core/ZapretPaths.cpp
    src/core/ZapretLocator.cpp
    src/core/PrivilegeHelper.cpp
    src/core/ZapretManager.cpp
    src/platform/WindowsBackend.cpp
    src/platform/LinuxBackend.cpp
)

# Headers carrying Q_OBJECT are listed so CMAKE_AUTOMOC generates their moc regardless of include
# order (IZapretBackend is an abstract interface only ever included, never a .cpp sibling).
target_sources(zapretcore PRIVATE
    src/core/Strategy.h
    src/core/IZapretBackend.h
    src/core/ZapretManager.h
    src/platform/WindowsBackend.h
    src/platform/LinuxBackend.h
)

target_include_directories(zapretcore PUBLIC src/core src/platform)
target_link_libraries(zapretcore PUBLIC Qt6::Core)

# Windows-only: ShellExecuteEx/ShellExecuteW (shell32) and token APIs (advapi32) used by
# PrivilegeHelper. They exist only on an MSVC/MinGW target and are omitted for host builds/tests.
if (WIN32)
    target_link_libraries(zapretcore PUBLIC shell32 advapi32)
endif()

# ---- core verification harness ---------------------------------------------
# Plain console program (no QTest dependency) that exercises the parser/locator
# against real on-disk zapret trees. See tests/coretest.cpp.
add_executable(coretest tests/coretest.cpp)
target_link_libraries(coretest PRIVATE zapretcore)

enable_testing()
add_test(NAME coretest COMMAND coretest)
