# libocdm: the OpenCDM C API used by WebKit's Thunder EME backend, implemented in-process on the TV's
# Widevine CE CDM (loaded at run time). See docs/widevine-cdm-abi.md.
cmake_minimum_required(VERSION 3.16)
project(ocdm-widevine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(OCDM_WIDEVINE_TESTS "Build the tests and the fake CDM" OFF)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST IMPORTED_TARGET gstreamer-1.0 gstreamer-base-1.0)

# Everything except the GStreamer entry points; tests link this directly.
add_library(ocdm_core STATIC src/ocdm.cpp src/cdm_library.cpp src/http.cpp src/host.cpp)
target_include_directories(ocdm_core PUBLIC include src)
target_compile_options(ocdm_core PRIVATE -Wall -Wextra)
target_link_libraries(ocdm_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

if (GST_FOUND)
    add_library(ocdm SHARED src/gst_adapter.cpp)
    target_compile_options(ocdm PRIVATE -Wall -Wextra)
    target_link_libraries(ocdm PRIVATE -Wl,--whole-archive ocdm_core -Wl,--no-whole-archive PkgConfig::GST)
    target_link_options(ocdm PRIVATE -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/libocdm.map
        -Wl,--no-undefined -static-libstdc++ -static-libgcc)
    set_target_properties(ocdm PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 1)
    install(TARGETS ocdm LIBRARY DESTINATION lib)
    # WebKit's FindThunder looks for open_cdm.h under WPEFramework/ocdm/.
    install(FILES include/ocdm/open_cdm.h include/ocdm/open_cdm_adapter.h DESTINATION include/WPEFramework/ocdm)
else ()
    message(STATUS "GStreamer not found: building only the core library and its tests")
endif ()

if (OCDM_WIDEVINE_TESTS)
    enable_testing()
    add_library(fake_cdm SHARED tests/fake_cdm.cpp)
    target_include_directories(fake_cdm PRIVATE include)

    add_executable(host_test tests/host_test.cpp)
    target_link_libraries(host_test PRIVATE ocdm_core)
    add_test(NAME host_test COMMAND host_test)

    add_executable(abi_check tests/abi_check.cpp)
    target_include_directories(abi_check PRIVATE include)
    add_test(NAME abi_check COMMAND abi_check)

    add_executable(ocdm_test tests/ocdm_test.cpp)
    target_link_libraries(ocdm_test PRIVATE ocdm_core)
    add_test(NAME ocdm_test COMMAND ocdm_test $<TARGET_FILE:fake_cdm>)

    if (GST_FOUND)
        add_executable(gst_adapter_test tests/gst_adapter_test.cpp)
        target_link_libraries(gst_adapter_test PRIVATE ocdm ${CMAKE_DL_LIBS} PkgConfig::GST)
        target_include_directories(gst_adapter_test PRIVATE include)
        add_test(NAME gst_adapter_test COMMAND gst_adapter_test $<TARGET_FILE:fake_cdm>)
    endif ()
endif ()
