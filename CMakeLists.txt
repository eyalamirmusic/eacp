cmake_minimum_required(VERSION 3.31)
project(eacp LANGUAGES C CXX)

if (CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
            "eacp requires a 64-bit toolchain (x64 or ARM64), but the "
            "selected compiler targets 32-bit "
            "(CMAKE_SIZEOF_VOID_P=${CMAKE_SIZEOF_VOID_P}, "
            "arch='${CMAKE_SYSTEM_PROCESSOR}'). On Windows, configure from an "
            "x64 or ARM64 Developer prompt, or pass an explicit 64-bit "
            "target, e.g. -DCMAKE_CXX_FLAGS=--target=x86_64-pc-windows-msvc.")
endif ()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(APPLE)
    enable_language(OBJCXX)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake")

option(EACP_UNITY_BUILD "Compile eacp libraries as CMake unity builds" OFF)
option(EACP_BUILD_GRAPHICS "Build the Graphics, SVG and WebView modules" ON)
option(EACP_BUILD_WEBVIEW "Build the WebView module" ON)

if (NOT EACP_BUILD_GRAPHICS)
    set(EACP_BUILD_WEBVIEW OFF CACHE BOOL
            "Build the WebView module" FORCE)
endif ()

include(TargetSetup)
include(CPM)
include(EacpSchema)

if (PROJECT_IS_TOP_LEVEL)
    eacp_default_setup()
endif ()

set(CMAKE_CXX_STANDARD 20)
add_subdirectory(Lib)

if (PROJECT_IS_TOP_LEVEL)
    # enable_testing() must precede add_subdirectory(Apps) so per-app
    # test targets (registered via eacp_add_webview_test) get picked
    # up by CTest. The Tests/ subtree is downstream, unaffected.
    enable_testing()
    add_subdirectory(Apps)
    add_subdirectory(Tests)
endif ()