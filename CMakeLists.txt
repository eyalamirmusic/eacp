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

if (APPLE)
    enable_language(OBJCXX)
endif ()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake")

option(EACP_UNITY_BUILD "Compile eacp libraries as CMake unity builds" OFF)
option(EACP_BUILD_GRAPHICS "Build the Graphics, SVG and WebView modules" ON)
option(EACP_BUILD_WEBVIEW "Build the WebView module" ON)
option(EACP_CI_BUILD "Mirror the exact flag set used by CI builds" OFF)

# EACP_CI_BUILD is the single switch CI passes so local builds can reproduce
# the CI configuration exactly. It forces on the unity-build flag of every
# project that exposes one: eacp's own (EACP_UNITY_BUILD) and Miro's
# (MIRO_UNITY_BUILD). These are set before the dependencies are pulled in below.
if (EACP_CI_BUILD)
    set(EACP_UNITY_BUILD ON CACHE BOOL
            "Compile eacp libraries as CMake unity builds" FORCE)
    set(MIRO_UNITY_BUILD ON CACHE BOOL
            "Enable CMake unity build for the Miro library" FORCE)
endif ()

if (NOT EACP_BUILD_GRAPHICS)
    set(EACP_BUILD_WEBVIEW OFF CACHE BOOL
            "Build the WebView module" FORCE)
endif ()

include(TargetSetup)
include(CPM)
include(EacpSchema)

option(EACP_ENABLE_TESTS "Enable Tests for EACP" ${PROJECT_IS_TOP_LEVEL})

if (PROJECT_IS_TOP_LEVEL)
    eacp_default_setup()
endif ()

set(CMAKE_CXX_STANDARD 20)
add_subdirectory(Lib)

if (PROJECT_IS_TOP_LEVEL)
    add_subdirectory(Apps)
endif ()

if (EACP_ENABLE_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif ()