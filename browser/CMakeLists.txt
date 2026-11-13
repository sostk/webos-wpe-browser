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

add_executable(wpebrowser main.cpp chrome.cpp screens.cpp profile.cpp storage.cpp private_loader.cpp engine_client.cpp page_view.cpp keys.cpp direct_owner.cpp media_server.cpp lg_media_player.cpp wren_ui.cpp)
target_include_directories(wpebrowser PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../common")
target_link_libraries(wpebrowser PRIVATE wnk_app wnk_media_ndl ${CMAKE_DL_LIBS})
if (WNK_WEBOS)
    # LG's buffer-feed media pipeline (lg_media_player.cpp), from the NDK's starfish-media-pipeline header.
    target_include_directories(wpebrowser SYSTEM PRIVATE "${CMAKE_SYSROOT}/usr/include/starfish-media-pipeline")
    target_link_libraries(wpebrowser PRIVATE playerAPIs)
endif ()  # dlopen: libwpeb-owner.so (direct_owner.cpp)
target_include_directories(wpebrowser SYSTEM PRIVATE "${WNK_KIT_DIR}/third_party")  # nlohmann/json.hpp

# Fonts live in fonts/ next to the executable, on the host and in the ipk: the kit's Atkinson Hyperlegible and Wren's
# Manrope (both OFL; Manrope from Google Fonts, static weights 300/400/600/700).
add_custom_command(TARGET wpebrowser POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${WNK_FONT_DIR}" "$<TARGET_FILE_DIR:wpebrowser>/fonts"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/fonts" "$<TARGET_FILE_DIR:wpebrowser>/fonts"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/icons" "$<TARGET_FILE_DIR:wpebrowser>/icons"
        VERBATIM)

wnk_add_webos_package(
        TARGET wpebrowser
        APPID org.sostk.wpebrowser
        TITLE "Wren"
        VERSION 0.3.0
        VENDOR "sostk"
        ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.png"
        LARGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/largeIcon.png"
        ICON_COLOR "#0b1020"
        EXPECTED_NEEDED "${CMAKE_CURRENT_SOURCE_DIR}/expected-needed.txt"
        EXTRA_DIRS "${WNK_FONT_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/fonts" "${CMAKE_CURRENT_SOURCE_DIR}/icons"
        GLIBC_RUNTIMES ${WPEB_ENGINE_EXTRA_DIR}
        # Direct GPU presentation (engine/lib/libWPEBackend-webos.so, libwpeb-owner.so) must use the TV's own
        # Wayland client and wayland-egl, the ones its Mali EGL is built against.
        # Sound goes through the TV's libpulse: its PulseAudio 9.0 server rejects our newer client (tools/stage-engine.sh).
        SYSTEM_LIBS libwayland-client.so.0 libwayland-egl.so libpulse.so.0 libplayerAPIs.so.1)

# Host smoke test: OK on the focused button, then Back twice at the root quits.
wnk_add_app_test(NAME e2e.wpebrowser.starts_and_quits TARGET wpebrowser PREFIX APP_WPEBROWSER
        KEYS "Escape,Escape" PASS "browser ui ready")

if (NOT CMAKE_CROSSCOMPILING)
    add_executable(engine-environment-test tests/engine_environment.cpp engine_client.cpp private_loader.cpp)
    target_compile_features(engine-environment-test PRIVATE cxx_std_17)
    target_include_directories(engine-environment-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../common")
    target_link_libraries(engine-environment-test PRIVATE wnk_logging)
    add_test(NAME engine.environment COMMAND engine-environment-test)
    add_executable(storage-test tests/storage.cpp storage.cpp)
    target_compile_features(storage-test PRIVATE cxx_std_17)
    target_include_directories(storage-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME browser.storage COMMAND storage-test)
    set(LOADER_FIXTURE_INTERP "/tmp/wpe-loader-test/ld-test.so.1")
    add_executable(loader-fixture tests/loader_fixture.c)
    target_link_options(loader-fixture PRIVATE "-Wl,--dynamic-linker=${LOADER_FIXTURE_INTERP}")
    add_executable(private-loader-test tests/private_loader.cpp private_loader.cpp)
    target_compile_features(private-loader-test PRIVATE cxx_std_17)
    target_include_directories(private-loader-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(private-loader-test PRIVATE
        LOADER_FIXTURE="$<TARGET_FILE:loader-fixture>" LOADER_FIXTURE_INTERP="${LOADER_FIXTURE_INTERP}")
    add_dependencies(private-loader-test loader-fixture)
    add_test(NAME browser.private_loader COMMAND private-loader-test)
endif ()
