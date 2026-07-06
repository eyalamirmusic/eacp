include(AppleSetup)

function(set_default_warnings_level target)
    if (MSVC)
        target_compile_options(${target} PRIVATE /W4)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif ()
endfunction()

function(set_default_target_setting target)
    set_default_warnings_level(${target})
    set_target_properties(${target} PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    if (IOS)
        set_target_properties(${target} PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${EACP_IOS_PLIST}")
    elseif (APPLE)
        set_target_properties(${target} PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${EACP_MACOS_PLIST}")
    endif ()
endfunction()

function(eacp_enable_unity_build target)
    if (EACP_UNITY_BUILD)
        set_target_properties(${target} PROPERTIES UNITY_BUILD ON)
    endif ()
endfunction()

# Force a target to compile at the platform's maximum *speed* optimization in
# EVERY configuration -- including Debug -- without enabling fast-math: IEEE FP
# semantics and bit-exact results are preserved (no multiply-add contraction).
# Intended for hot numeric / SIMD kernels that are pointless at -O0 / -Od. Debug
# info (-g / /Zi) is left untouched, so the target stays debuggable, just fast.
#
# Removing cl's Debug-only /RTC1 and /Od edits the *directory-scoped* Debug flags
# (hence PARENT_SCOPE), so keep each force-optimized target in its own
# subdirectory -- the usual case. The rest of the project keeps the defaults.
function(eacp_force_optimization target)
    if (MSVC)
        # The cl/clang-cl Debug defaults fight optimization: /RTC1 is a hard
        # error under any /O level (D8016) and /Od warns when overridden by /O2
        # (D9025). Strip both before forcing the level below.
        string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_CXX_FLAGS_DEBUG
                "${CMAKE_CXX_FLAGS_DEBUG}")
        string(REGEX REPLACE "/Od" "" CMAKE_CXX_FLAGS_DEBUG
                "${CMAKE_CXX_FLAGS_DEBUG}")
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}" PARENT_SCOPE)

        # clang-cl reports CXX_COMPILER_ID==Clang but parses the MSVC-style
        # driver, so its GCC-style flags must tunnel through /clang: or they are
        # silently dropped -- and a dropped -ffp-contract=off lets clang-cl fuse
        # FMA, breaking bit-exactness. Real cl has no /O3 (tops out at /O2) and
        # never contracts FP by default (/fp:precise).
        if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE
                    /clang:-O3 /clang:-ffp-contract=off)
        else ()
            target_compile_options(${target} PRIVATE /O2)
        endif ()
    else ()
        target_compile_options(${target} PRIVATE -O3 -ffp-contract=off)
    endif ()
endfunction()

# Mark an executable as a windowed GUI app so launching it never pops a console
# window on Windows — the cross-platform analogue of MACOSX_BUNDLE on Apple. A
# no-op everywhere but Windows. Call this on any eacp app that owns a window
# (graphics/tray/webview apps) rather than a console.
function(eacp_set_gui_subsystem target)
    set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE TRUE)
    # WIN32_EXECUTABLE selects /SUBSYSTEM:WINDOWS, whose default MSVC entry point
    # is WinMainCRTStartup (it expects WinMain). eacp apps use a standard
    # int main(), so redirect the entry to the console CRT startup — it still
    # parses argv and calls main(), just without a console attached.
    if (MSVC)
        target_link_options(${target} PRIVATE "/ENTRY:mainCRTStartup")
    endif ()
endfunction()

# Gives an app its at-rest icon — the one Finder, Explorer and a
# not-yet-running Dock/taskbar tile draw. Those are rendered by processes
# that never execute the binary, so a runtime WindowOptions::applicationIcon
# cannot supply them: macOS reads the bundle's .icns (named by
# CFBundleIconFile), Windows reads an ICON resource compiled into the .exe.
# Apps supply the platform-native files directly:
#
#   eacp_set_app_icon(MyApp
#           ICNS "${CMAKE_CURRENT_SOURCE_DIR}/Icon.icns"
#           ICO  "${CMAKE_CURRENT_SOURCE_DIR}/Icon.ico")
#
# Either argument may be omitted for a platform the app doesn't ship on.
# Explorer picks the ICON resource with the lowest ID for the at-rest icon,
# hence resource id 1 in the generated .rc.
function(eacp_set_app_icon target)
    cmake_parse_arguments(ARG "" "ICNS;ICO" "" ${ARGN})

    if (APPLE AND NOT IOS AND ARG_ICNS)
        get_filename_component(icns "${ARG_ICNS}" ABSOLUTE)
        get_filename_component(icns_name "${icns}" NAME)

        target_sources(${target} PRIVATE "${icns}")
        set_source_files_properties("${icns}" PROPERTIES
                MACOSX_PACKAGE_LOCATION Resources)
        set_target_properties(${target} PROPERTIES
                MACOSX_BUNDLE_ICON_FILE "${icns_name}")
    endif ()

    if (WIN32 AND ARG_ICO)
        # get_filename_component normalises to forward slashes, which rc.exe
        # accepts and which don't act as escape characters in the .rc string.
        get_filename_component(ico "${ARG_ICO}" ABSOLUTE)
        set(rc_file "${CMAKE_CURRENT_BINARY_DIR}/${target}-icon.rc")

        file(CONFIGURE OUTPUT "${rc_file}" CONTENT "1 ICON \"${ico}\"\n")
        target_sources(${target} PRIVATE "${rc_file}")
        set_source_files_properties("${rc_file}" PROPERTIES
                OBJECT_DEPENDS "${ico}")
    endif ()
endfunction()

function(add_ide_sources target)
    file(GLOB_RECURSE ALL_HEADERS
            "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    )

    if (ALL_HEADERS)
        target_sources(${target} PRIVATE ${ALL_HEADERS})
        set_source_files_properties(${ALL_HEADERS} PROPERTIES HEADER_FILE_ONLY TRUE)
    endif ()
endfunction()

function(eacp_default_setup)
    eacp_setup_apple()

    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(CMAKE_CXX_COMPILE_OPTIONS_IPO "-flto=full")
    endif ()
endfunction()