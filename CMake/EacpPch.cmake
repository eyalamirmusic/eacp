# One precompiled header, built once and reused by every eacp target.
#
# The standard library, not eacp, dominates compile time here: on MSVC the
# <string>/<vector>/<memory>/<thread> set is ~1.8s of a typical 2.0s
# translation unit, and the project has several hundred of them. Precompiling
# it once takes an app's Main.cpp from ~2.0s to ~0.6s.

# Two toolchain setups cannot express a shared PCH at all:
#
# Xcode has no way to represent a target-wide setting per language -- CMake
# evaluates $<COMPILE_LANGUAGE:C> as CXX for any target that also holds C++
# sources -- so the generated C resource blobs would be told to precompile the
# C++ standard library.
#
# An Apple universal build compiles each TU once with both -arch flags, while
# clang bakes a single target architecture into a .pch. There is no one PCH to
# emit, and none the compile could load.
function(eacp_shared_pch_supported result)
    if (CMAKE_GENERATOR MATCHES "Xcode")
        set(${result} OFF PARENT_SCOPE)
        return()
    endif ()

    list(LENGTH CMAKE_OSX_ARCHITECTURES architectures)

    if (architectures GREATER 1)
        set(${result} OFF PARENT_SCOPE)
        return()
    endif ()

    set(${result} ON PARENT_SCOPE)
endfunction()

# CMake emits a separate PCH per language a target compiles, and a consumer can
# only reuse the languages the producer itself speaks. Consumers compile C (the
# generated resource blobs) and, on Apple, Objective-C++ -- so the producer has
# to carry a source of each, and select the matching header per language.
function(eacp_add_shared_pch)
    eacp_shared_pch_supported(supported)

    if (NOT supported)
        message(STATUS "eacp: shared PCH unavailable for this toolchain setup")
        return()
    endif ()

    set(dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    set(sources "${dir}/Pch.c" "${dir}/Pch.cpp")
    set(headers
            "$<$<COMPILE_LANGUAGE:C>:${dir}/Pch-C.h>"
            "$<$<COMPILE_LANGUAGE:CXX>:${dir}/Pch.h>")

    if (APPLE)
        list(APPEND sources "${dir}/Pch.mm")
        list(APPEND headers "$<$<COMPILE_LANGUAGE:OBJCXX>:${dir}/Pch.h>")
    endif ()

    add_library(eacp-pch STATIC ${sources})
    target_precompile_headers(eacp-pch PRIVATE ${headers})

    # cl compares the consumer's warning level against the one baked into the
    # PCH, so the producer has to be built at the project's level too.
    set_default_warnings_level(eacp-pch)
endfunction()

# Reusing a PCH requires the consumer to compile with the flags it was built
# with, so a target that rewrites its own optimization level opts out via
# eacp_no_shared_pch below.
function(eacp_use_shared_pch target)
    if (NOT EACP_SHARED_PCH OR NOT TARGET eacp-pch OR target STREQUAL eacp-pch)
        return()
    endif ()

    get_target_property(type ${target} TYPE)

    if (type STREQUAL "INTERFACE_LIBRARY" OR type STREQUAL "UTILITY")
        return()
    endif ()

    # An object library's objects are spliced into whatever links it, but the
    # object cl emits alongside the PCH is not -- and MSVC needs it present in
    # every image holding /Yu-compiled code (LNK2011).
    if (type STREQUAL "OBJECT_LIBRARY")
        return()
    endif ()

    target_precompile_headers(${target} REUSE_FROM eacp-pch)
endfunction()

# Opt a target out, whether or not it has already been given the shared PCH.
#
# This declares the target PCH-free rather than trying to strip the reuse
# property back off it: removing a property is not portable across CMake
# versions -- 4.x drops it, older ones keep an empty target name and then
# reject it as a target that does not exist.
function(eacp_no_shared_pch target)
    set_target_properties(${target} PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)
endfunction()
