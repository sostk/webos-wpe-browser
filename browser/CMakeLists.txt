# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.22)
project(wpebrowser LANGUAGES C CXX)

# The kit: -DWNK_KIT_DIR=..., else $WNK_KIT_DIR, else where it was when tools/new-app.py made this.
# A relative path is taken from this folder (a git submodule at kit/ would be "kit").
if (DEFINED ENV{WNK_KIT_DIR})
    set(_kit_default "$ENV{WNK_KIT_DIR}")
else ()
    set(_kit_default "../third_party/webos-native-app-kit")
endif ()
set(WNK_KIT_DIR "${_kit_default}" CACHE PATH "The webOS native app kit (absolute, or relative to this folder)")
get_filename_component(_kit "${WNK_KIT_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if (NOT EXISTS "${_kit}/cmake/WnkApp.cmake")
    message(FATAL_ERROR "The webOS native app kit is not at ${_kit}: set WNK_KIT_DIR")
endif ()
# set(WNK_FFMPEG ON)  # media playback: see the kit's docs/AGENT-APP-GUIDE.md, "Media playback"
include("${_kit}/cmake/WnkApp.cmake")

# The WebKit engine (engine/ built with the glibc 2.28 toolchain, plus its bundled libraries) is staged by
# tools/stage-engine.sh into a folder named "engine"; when it exists the ipk carries it next to the app.
set(WPEB_ENGINE_STAGE "${CMAKE_CURRENT_SOURCE_DIR}/../build/engine-stage/engine" CACHE PATH "Staged engine folder")
if (EXISTS "${WPEB_ENGINE_STAGE}/wpe-engine")
    set(WPEB_ENGINE_EXTRA_DIR "${WPEB_ENGINE_STAGE}")
    message(STATUS "wpebrowser: packaging the engine from ${WPEB_ENGINE_STAGE}")
else ()
    set(WPEB_ENGINE_EXTRA_DIR "")
    message(STATUS "wpebrowser: no staged engine; the package will show 'engine not found'")
endif ()

add_executable(wpebrowser main.cpp chrome.cpp screens.cpp profile.cpp engine_client.cpp page_view.cpp keys.cpp)
target_include_directories(wpebrowser PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../common")
target_link_libraries(wpebrowser PRIVATE wnk_app)
target_include_directories(wpebrowser SYSTEM PRIVATE "${WNK_KIT_DIR}/third_party")  # nlohmann/json.hpp

# Fonts (Atkinson Hyperlegible, OFL) live in fonts/ next to the executable, on the host and in the ipk.
add_custom_command(TARGET wpebrowser POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${WNK_FONT_DIR}" "$<TARGET_FILE_DIR:wpebrowser>/fonts"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/icons" "$<TARGET_FILE_DIR:wpebrowser>/icons"
        VERBATIM)

wnk_add_webos_package(
        TARGET wpebrowser
        APPID org.sostk.wpebrowser
        TITLE "WPE Browser"
        VERSION 0.2.0
        VENDOR "sostk"
        ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.png"
        LARGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/largeIcon.png"
        ICON_COLOR "#1f2937"
        EXPECTED_NEEDED "${CMAKE_CURRENT_SOURCE_DIR}/expected-needed.txt"
        EXTRA_DIRS "${WNK_FONT_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/icons"
        GLIBC_RUNTIMES ${WPEB_ENGINE_EXTRA_DIR})

# Host smoke test: OK on the focused button, then Back twice at the root quits.
wnk_add_app_test(NAME e2e.wpebrowser.starts_and_quits TARGET wpebrowser PREFIX APP_WPEBROWSER
        KEYS "Escape,Escape" PASS "browser ui ready")
