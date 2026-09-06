cmake_minimum_required(VERSION 3.31)
project(eacp VERSION 0.1.0 LANGUAGES C CXX)

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

# eacp builds runtime-loaded plugins and webview helpers as shared libraries,
# and those link eacp-core together with its static dependencies (ea_data_structures,
# Miro, ResEmbed). Every such static lib must be position-independent or the
# shared-object link fails on Linux (ld: "final link failed: bad value"). Force
# PIC for every target, including the CPM-fetched dependencies configured below.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (APPLE)
    enable_language(OBJCXX)

    # CMake already puts -lobjc on the link line of everything that pulls in
    # Objective-C. Xcode adds -fobjc-link-runtime on top for any target with its
    # own .mm sources, and ld warns about the duplicate.
    set(CMAKE_XCODE_ATTRIBUTE_CLANG_LINK_OBJC_RUNTIME NO)
endif ()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake")

option(EACP_UNITY_BUILD "Compile eacp libraries as CMake unity builds" OFF)
option(EACP_BUILD_GRAPHICS "Build the Graphics, SVG and WebView modules" ON)
option(EACP_BUILD_WEBVIEW "Build the WebView module" ON)
option(EACP_CI_BUILD "Mirror the exact flag set used by CI builds" OFF)

# Partly implemented. What it turns on today is a headless eacp-graphics -
# views, windows with no surface, and the portable model around them - and the
# compute half of the Vulkan backend under it: buffers, kernels, command
# buffers and timestamps, with the render half still stubbed. The Wayland and
# FreeType halves arrive over the later stages of the plan, so EACP_HAS_TEXT
# below stays off on Linux whatever this says.
option(EACP_LINUX_GRAPHICS
        "Build the Linux graphics backend (headless Graphics, Vulkan compute)"
        OFF)

if (EACP_CI_BUILD)
    set(EACP_UNITY_BUILD ON CACHE BOOL
            "Compile eacp libraries as CMake unity builds" FORCE)
    set(MIRO_UNITY_BUILD ON CACHE BOOL
            "Enable CMake unity build for the Miro library" FORCE)
    # Worth roughly half the compile time of a cold Windows build. Off by
    # default because it is a whole-project rebuild whenever Pch.h changes,
    # which is the wrong trade for an edit-compile loop.
    set(EACP_PCH ON CACHE BOOL
            "Precompile the system headers every eacp target parses" FORCE)
endif ()

if (NOT EACP_BUILD_GRAPHICS)
    set(EACP_BUILD_WEBVIEW OFF CACHE BOOL
            "Build the WebView module" FORCE)
endif ()

# What this host can do, decided once and read by Lib, Apps and Tests instead of
# each of them restating the platform predicate. set() does not evaluate boolean
# expressions, hence the if()/set() shape.
#
#   EACP_HAS_DRAW    a windowing system to put views in: Graphics.
#   EACP_HAS_GPU     a GPU backend under eacp-gpu, and so also GPUWidgets and
#                    Sprites, which are portable C++ over nothing else.
#   EACP_HAS_TEXT    a glyph rasterizer, and so also UI and SVG, which draw
#                    their text through it.
#   EACP_HAS_CAPTURE cameras and screen/video capture, which no Linux backend
#                    exists for even once drawing does: Camera, CameraView,
#                    Video, VideoView.
#   EACP_HAS_WEBVIEW a native web runtime (WKWebView, WebView2). The page
#                    bridge is portable and deliberately outside this.
#
# The three drawing variables are nested rather than one predicate because Linux
# reaches them one at a time: headless Graphics first, then Vulkan, then
# FreeType/HarfBuzz. Apple and Windows have all three, so their target sets are
# whatever EACP_HAS_DRAW was already. Linux is now at the second: the Vulkan
# backend's compute and buffer half is there, so EACP_HAS_GPU follows
# EACP_LINUX_GRAPHICS, and EACP_HAS_TEXT is the one still waiting.
#
# Everything device-free stays out of them: the shader codegen library and the
# WebView page bridge build on every platform.
set(EACP_HAS_DRAW OFF)

if (EACP_BUILD_GRAPHICS AND (APPLE OR WIN32 OR (LINUX AND EACP_LINUX_GRAPHICS)))
    set(EACP_HAS_DRAW ON)
endif ()

set(EACP_HAS_GPU OFF)

if (EACP_HAS_DRAW AND (APPLE OR WIN32 OR (LINUX AND EACP_LINUX_GRAPHICS)))
    set(EACP_HAS_GPU ON)
endif ()

set(EACP_HAS_TEXT OFF)

if (EACP_HAS_GPU AND (APPLE OR WIN32))
    set(EACP_HAS_TEXT ON)
endif ()

set(EACP_HAS_CAPTURE OFF)

if (EACP_HAS_DRAW AND (APPLE OR WIN32))
    set(EACP_HAS_CAPTURE ON)
endif ()

set(EACP_HAS_WEBVIEW OFF)

if (EACP_HAS_DRAW AND EACP_BUILD_WEBVIEW AND (APPLE OR WIN32))
    set(EACP_HAS_WEBVIEW ON)
endif ()

include(TargetSetup)
include(EacpSchema)
include(EacpPlugin)
include(EacpPch)

option(EACP_HIDDEN_VISIBILITY "Compile eacp with hidden symbol visibility" ON)

if (EACP_HIDDEN_VISIBILITY)
    set(CMAKE_C_VISIBILITY_PRESET hidden)
    set(CMAKE_CXX_VISIBILITY_PRESET hidden)
    set(CMAKE_OBJC_VISIBILITY_PRESET hidden)
    set(CMAKE_OBJCXX_VISIBILITY_PRESET hidden)
    set(CMAKE_VISIBILITY_INLINES_HIDDEN TRUE)
endif ()

# GCC gives some STL statics STB_GNU_UNIQUE binding, and glibc refuses to unmap
# any library exporting one. eacp libraries link statically into runtime-loaded
# plugins, so a unique symbol anywhere in them pins the plugin resident and
# defeats the unload contract DynamicLibrary is built on (eacp_add_plugin sets
# the same flag on a plugin's own sources). Clang emits no such symbols.
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-fno-gnu-unique)
endif ()


option(EACP_ENABLE_TESTS "Enable Tests for EACP" ${PROJECT_IS_TOP_LEVEL})

if (PROJECT_IS_TOP_LEVEL)
    eacp_default_setup()
endif ()

# Deliberately outside that guard: a consumer building eacp as a subproject is
# exactly who gets no deployment target and needs telling.
#eacp_check_deployment_target()

set(CMAKE_CXX_STANDARD 20)

add_subdirectory(Lib)

option(EACP_ENABLE_EXAMPLES "Enable Examples for EACP" ${PROJECT_IS_TOP_LEVEL})

if (EACP_ENABLE_EXAMPLES)
    add_subdirectory(Apps)
endif ()

if (EACP_ENABLE_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif ()

# Builds the shared precompiled header and points every target above at it.
# Has to run after the last add_subdirectory: it walks the targets they created.
eacp_apply_pch()
