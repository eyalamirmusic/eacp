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

# Stamps an app's name and the enclosing project's version into the binary two
# ways: the native metadata the OS reads (macOS bundle plist keys / a Windows
# VERSIONINFO resource), and an AppInfo.json embedded via ResEmbed that
# Platform::getAppName()/getAppVersion() read back at runtime. The name comes
# from MACOSX_BUNDLE_BUNDLE_NAME (apps set it before calling us) or the target
# name; the version from ${PROJECT_VERSION}, defaulting to 0.0.0.
function(eacp_embed_app_info target)
    get_target_property(app_name ${target} MACOSX_BUNDLE_BUNDLE_NAME)
    if (NOT app_name)
        set(app_name "${target}")
    endif ()

    set(app_version "${PROJECT_VERSION}")
    if (NOT app_version)
        set(app_version "0.0.0")
    endif ()

    if (APPLE)
        set_target_properties(${target} PROPERTIES
                MACOSX_BUNDLE_SHORT_VERSION_STRING "${app_version}"
                MACOSX_BUNDLE_BUNDLE_VERSION "${app_version}"
                MACOSX_BUNDLE_LONG_VERSION_STRING "${app_version}")
    elseif (WIN32)
        # VERSIONINFO's FILEVERSION/PRODUCTVERSION need four numeric fields; the
        # project may set fewer, so pad the missing components with 0.
        set(v_major "${PROJECT_VERSION_MAJOR}")
        set(v_minor "${PROJECT_VERSION_MINOR}")
        set(v_patch "${PROJECT_VERSION_PATCH}")
        set(v_tweak "${PROJECT_VERSION_TWEAK}")
        foreach (comp v_major v_minor v_patch v_tweak)
            if (NOT ${comp})
                set(${comp} 0)
            endif ()
        endforeach ()

        # Resource ids are per-type, so id 1 here does not clash with the
        # id-1 ICON that eacp_set_app_icon emits.
        set(version_rc "${CMAKE_CURRENT_BINARY_DIR}/${target}-version.rc")
        file(CONFIGURE OUTPUT "${version_rc}" @ONLY CONTENT [==[
1 VERSIONINFO
FILEVERSION @v_major@,@v_minor@,@v_patch@,@v_tweak@
PRODUCTVERSION @v_major@,@v_minor@,@v_patch@,@v_tweak@
FILEOS 0x40004L
FILETYPE 0x1L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "ProductName", "@app_name@"
            VALUE "FileVersion", "@app_version@"
            VALUE "ProductVersion", "@app_version@"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
]==])
        target_sources(${target} PRIVATE "${version_rc}")
    endif ()

    # The ResEmbed runtime key is the file basename, so the file must be named
    # exactly AppInfo.json; a per-target subdir keeps sibling targets isolated.
    set(app_info_json "${CMAKE_CURRENT_BINARY_DIR}/${target}-AppInfo/AppInfo.json")
    file(CONFIGURE OUTPUT "${app_info_json}" @ONLY CONTENT [==[
{
    "name": "@app_name@",
    "version": "@app_version@"
}
]==])

    find_package(ResEmbed REQUIRED)
    res_embed_add(${target}
            FILES     "${app_info_json}"
            NAMESPACE AppInfo
            CATEGORY  AppInfo)
endfunction()

# Mark an executable as a windowed GUI app so launching it never pops a console
# window on Windows — the cross-platform analogue of MACOSX_BUNDLE on Apple. It
# also stamps the app name and project version into the binary via
# eacp_embed_app_info. Call this on any eacp app that owns a window
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

    eacp_embed_app_info(${target})
endfunction()

# Gives an app its at-rest icon — the one Finder, Explorer and a
# not-yet-running Dock/taskbar tile draw. Those are rendered by processes
# that never execute the binary, so a runtime WindowOptions::applicationIcon
# cannot supply them: macOS reads the bundle's .icns (named by
# CFBundleIconFile), Windows reads an ICON resource compiled into the .exe.
# Apps either point IMAGE at one source PNG/JPEG and let the build generate the
# right container for the platform being built, or supply the platform-native
# files directly:
#
#   eacp_set_app_icon(MyApp IMAGE "${CMAKE_CURRENT_SOURCE_DIR}/Icon.png")
#
#   eacp_set_app_icon(MyApp
#           ICNS "${CMAKE_CURRENT_SOURCE_DIR}/Icon.icns"
#           ICO  "${CMAKE_CURRENT_SOURCE_DIR}/Icon.ico")
#
# IMAGE runs eacp-icon-tool at build time to emit .icns on macOS / .ico on
# Windows, and wins over a prebuilt ICNS/ICO for that platform. Any argument
# may be omitted for a platform the app doesn't ship on. Explorer picks the
# ICON resource with the lowest ID for the at-rest icon, hence resource id 1 in
# the generated .rc.
function(eacp_set_app_icon target)
    cmake_parse_arguments(ARG "" "ICNS;ICO;IMAGE" "" ${ARGN})

    # A source image generates the current platform's container into the build
    # tree; feed it into the same wiring the prebuilt paths use below.
    if (ARG_IMAGE)
        get_filename_component(image "${ARG_IMAGE}" ABSOLUTE)

        if (APPLE AND NOT IOS)
            set(generated_icns "${CMAKE_CURRENT_BINARY_DIR}/${target}.icns")
            add_custom_command(
                    OUTPUT "${generated_icns}"
                    COMMAND eacp-icon-tool --format icns
                            --in "${image}" --out "${generated_icns}"
                    DEPENDS eacp-icon-tool "${image}"
                    VERBATIM)
            set(ARG_ICNS "${generated_icns}")
        elseif (WIN32)
            set(generated_ico "${CMAKE_CURRENT_BINARY_DIR}/${target}.ico")
            add_custom_command(
                    OUTPUT "${generated_ico}"
                    COMMAND eacp-icon-tool --format ico
                            --in "${image}" --out "${generated_ico}"
                    DEPENDS eacp-icon-tool "${image}"
                    VERBATIM)
            # The .rc references the .ico via OBJECT_DEPENDS rather than
            # compiling it, so list the generated file as a non-compiled source
            # to make CMake schedule its custom command.
            set_source_files_properties("${generated_ico}" PROPERTIES
                    GENERATED TRUE HEADER_FILE_ONLY TRUE)
            target_sources(${target} PRIVATE "${generated_ico}")
            set(ARG_ICO "${generated_ico}")
        endif ()
    endif ()

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
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
    add_compile_definitions(_LIBCPP_REMOVE_TRANSITIVE_INCLUDES)
    eacp_setup_apple()

    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(CMAKE_CXX_COMPILE_OPTIONS_IPO "-flto=full")
    endif ()
endfunction()