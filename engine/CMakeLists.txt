# SPDX-License-Identifier: GPL-3.0-or-later
# wpe-engine: the WPE WebKit host process of the webOS browser.
# Cross-build with Buildroot's toolchain file (glibc 2.28 toolchain + staging sysroot):
#   cmake -S engine -B build -DCMAKE_TOOLCHAIN_FILE=<br-out>/host/share/buildroot/toolchainfile.cmake
cmake_minimum_required(VERSION 3.20)
project(wpe_engine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WPE REQUIRED IMPORTED_TARGET wpe-webkit-2.0 wpe-platform-2.0 glib-2.0 gio-unix-2.0)

add_executable(wpe-engine src/main.cpp src/platform.cpp)
target_include_directories(wpe-engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(wpe-engine PRIVATE PkgConfig::WPE)
target_compile_options(wpe-engine PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
# Libraries are bundled next to the engine inside the app package (engine/lib).
set_target_properties(wpe-engine PROPERTIES
    BUILD_RPATH_USE_ORIGIN ON
    INSTALL_RPATH "$ORIGIN/lib")
install(TARGETS wpe-engine RUNTIME DESTINATION .)
