# SPDX-License-Identifier: GPL-3.0-or-later
# wpe-engine: the WPE WebKit host process of the webOS browser.
# Cross-build with Buildroot's toolchain file (glibc 2.28 toolchain + staging sysroot):
#   cmake -S engine -B build -DCMAKE_TOOLCHAIN_FILE=<br-out>/host/share/buildroot/toolchainfile.cmake
cmake_minimum_required(VERSION 3.20)
project(wpe_engine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WPE REQUIRED IMPORTED_TARGET wpe-webkit-2.0 wpe-platform-2.0 wpe-1.0 glib-2.0 gio-unix-2.0)

add_executable(wpe-engine src/main.cpp src/platform.cpp src/direct.cpp)
target_include_directories(wpe-engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
# libWPEBackend-webos.so (engine/webos-backend, built with the webOS NDK) is dlopen()ed in direct mode.
target_link_libraries(wpe-engine PRIVATE PkgConfig::WPE ${CMAKE_DL_LIBS})
target_compile_options(wpe-engine PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
# Libraries are bundled next to the engine inside the app package (engine/lib).
set_target_properties(wpe-engine PROPERTIES
    BUILD_RPATH_USE_ORIGIN ON
    INSTALL_RPATH "$ORIGIN/lib")
install(TARGETS wpe-engine RUNTIME DESTINATION .)

# Build-time tool, not shipped: compiles the ad blocker's rule list with this WPE WebKit (run under QEMU by
# tools/stage-engine.sh; the store format changes between WebKit releases).
add_executable(compile-rules ../tools/adblock/compile-rules.c)
target_link_libraries(compile-rules PRIVATE PkgConfig::WPE)

# Hardware video (engine/gst/wpebmediasink.c): GStreamer sinks that hand the encoded stream to the UI process.
pkg_check_modules(GSTBASE IMPORTED_TARGET gstreamer-1.0 gstreamer-base-1.0 gstreamer-audio-1.0 gstreamer-video-1.0
                  gstreamer-pbutils-1.0)
if (GSTBASE_FOUND)
    add_library(gstwpebmedia MODULE gst/wpebmediasink.c gst/wpebpulse.c)
    # libpulse: headers only; wpebpulse.c loads the TV's libpulse.so.0 at run time.
    pkg_check_modules(PULSE_HEADERS REQUIRED libpulse)
    target_include_directories(gstwpebmedia PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common ${PULSE_HEADERS_INCLUDE_DIRS})
    target_link_libraries(gstwpebmedia PRIVATE PkgConfig::GSTBASE ${CMAKE_DL_LIBS})
    target_compile_options(gstwpebmedia PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif ()
