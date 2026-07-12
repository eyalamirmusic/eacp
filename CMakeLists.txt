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
include(EacpPch)
include(CPM)
include(EacpSchema)
include(EacpPlugin)

option(EACP_HIDDEN_VISIBILITY "Compile eacp with hidden symbol visibility" ON)

if (EACP_HIDDEN_VISIBILITY)
    set(CMAKE_C_VISIBILITY_PRESET hidden)
    set(CMAKE_CXX_VISIBILITY_PRESET hidden)
    set(CMAKE_OBJC_VISIBILITY_PRESET hidden)
    set(CMAKE_OBJCXX_VISIBILITY_PRESET hidden)
    set(CMAKE_VISIBILITY_INLINES_HIDDEN TRUE)
endif ()

option(EACP_ENABLE_TESTS "Enable Tests for EACP" ${PROJECT_IS_TOP_LEVEL})

if (PROJECT_IS_TOP_LEVEL)
    eacp_default_setup()
endif ()

set(CMAKE_CXX_STANDARD 20)

# C++20 turns on Ninja's module dependency scanning, which preprocesses every
# translation unit a second time before it can be compiled, and makes each
# target's objects wait on a collation step for the whole target. eacp uses no
# modules, so that entire phase buys nothing.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Already a PUBLIC requirement of eacp-gpu and of Miro, so most of the tree
# compiles with it and the rest does not. Hoisting it to the whole project makes
# the flag set uniform -- which reusing one precompiled header across targets
# requires (cl rejects a PCH built under a different conformance mode).
add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)

option(EACP_EMBED_DEBUG_INFO
        "Put MSVC debug info in the objects (/Z7) instead of a PDB (/Zi)" OFF)

# /Z7 is what lets a compiler cache work at all -- a /Zi object references its
# .pdb by absolute path and so cannot be replayed from a cache -- but it copies
# the shared PCH's debug info into every object, which roughly doubles link
# time. Worth it with sccache in front, not worth it without.
if (EACP_EMBED_DEBUG_INFO)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
            "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
endif ()

option(EACP_SHARED_PCH "Precompile the standard library once for every target" ON)

if (EACP_SHARED_PCH)
    eacp_add_shared_pch()
endif ()

add_subdirectory(Lib)

if (PROJECT_IS_TOP_LEVEL)
    add_subdirectory(Apps)
endif ()

if (EACP_ENABLE_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif ()