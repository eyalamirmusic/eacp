include(AppleSetup)

# Published at include time rather than from eacp_default_setup(), which only
# runs when eacp is the top-level project: a project that fetches eacp gets
# the same bundle templates its own apps do.
set(EACP_MACOS_PLIST "${CMAKE_CURRENT_LIST_DIR}/macOSBundleInfo.plist.in"
        CACHE INTERNAL "eacp macOS bundle Info.plist template")
set(EACP_IOS_PLIST "${CMAKE_CURRENT_LIST_DIR}/iOSBundleInfo.plist.in"
        CACHE INTERNAL "eacp iOS bundle Info.plist template")

function(eacp_bundle_plist_template out_var)
    if (IOS)
        set(${out_var} "${EACP_IOS_PLIST}" PARENT_SCOPE)
    else ()
        set(${out_var} "${EACP_MACOS_PLIST}" PARENT_SCOPE)
    endif ()
endfunction()

if (ANDROID)
    include("${CMAKE_CURRENT_LIST_DIR}/Android.cmake")
endif ()

# eacp_add_app(<target> <sources>... [BUNDLE_ID <id>] [DISPLAY_NAME <name>]
#              [VERSION <x.y.z>] [VERSION_CODE <n>] [ICON <png>]
#              [ORIENTATION portrait|landscape])
#
# One app with one identity everywhere: com.eacp.<target>, named <target>, at the
# project's version, build 1, unless told otherwise. An executable whose bundle
# properties and at-rest icon these set, or on Android the shared library
# NativeActivity loads, with a module in the Android Studio project
# (eacp_add_android_app, which reads the rest). ORIENTATION locks a phone app
# to one orientation on iOS (UISupportedInterfaceOrientations) and Android
# (screenOrientation); landscape is either way up. Unset, the device rotates it.
function(eacp_add_app target)
    cmake_parse_arguments(APP ""
            "BUNDLE_ID;DISPLAY_NAME;VERSION;VERSION_CODE;ICON;ORIENTATION" ""
            ${ARGN})

    if (APP_ORIENTATION AND NOT APP_ORIENTATION MATCHES "^(portrait|landscape)$")
        message(FATAL_ERROR "eacp_add_app(${target}): ORIENTATION is portrait or "
                "landscape, not '${APP_ORIENTATION}'")
    endif ()

    string(TOLOWER "com.eacp.${target}" default_BUNDLE_ID)
    set(default_DISPLAY_NAME "${target}")
    set(default_VERSION "${PROJECT_VERSION}")
    set(default_VERSION_CODE 1)

    foreach (key BUNDLE_ID DISPLAY_NAME VERSION VERSION_CODE)
        if (NOT APP_${key})
            set(APP_${key} "${default_${key}}")
        endif ()
    endforeach ()

    if (NOT APP_VERSION)
        set(APP_VERSION 0.0.0)
    endif ()

    if (ANDROID)
        add_library(${target} SHARED ${APP_UNPARSED_ARGUMENTS})
    else ()
        add_executable(${target} ${APP_UNPARSED_ARGUMENTS})
    endif ()

    set_target_properties(${target} PROPERTIES
            MACOSX_BUNDLE_BUNDLE_NAME "${APP_DISPLAY_NAME}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${APP_BUNDLE_ID}"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${APP_BUNDLE_ID}"
            XCODE_ATTRIBUTE_MARKETING_VERSION "${APP_VERSION}"
            XCODE_ATTRIBUTE_CURRENT_PROJECT_VERSION "${APP_VERSION_CODE}"
            EACP_APP_VERSION "${APP_VERSION}"
            EACP_APP_VERSION_CODE "${APP_VERSION_CODE}")

    if (ANDROID)
        eacp_add_android_app(${target})
    elseif (APP_ICON)
        eacp_set_app_icon(${target} IMAGE "${APP_ICON}")
    endif ()

    if (IOS AND APP_ORIENTATION STREQUAL "portrait")
        eacp_add_plist_entries(${target} UISupportedInterfaceOrientations
                "UIInterfaceOrientationPortrait")
    elseif (IOS AND APP_ORIENTATION STREQUAL "landscape")
        eacp_add_plist_entries(${target} UISupportedInterfaceOrientations
                "UIInterfaceOrientationLandscapeLeft;UIInterfaceOrientationLandscapeRight")
    endif ()
endfunction()

function(set_default_warnings_level target)
    if (MSVC)
        target_compile_options(${target} PRIVATE /W4)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif ()

    # A 64-bit count assigned into an int is silent under -Wall -Wextra, and it
    # is exactly the bug the GPU buffer API's byte counts were widened to stop,
    # so the one warning that catches it is on wherever it exists. Clang only:
    # GCC has no equivalent short of -Wconversion, which is a far larger and
    # much noisier set, and MSVC already reports it as C4244 under /W4. A
    # narrowing that is genuinely intended is written as a cast with a comment.
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
        target_compile_options(${target} PRIVATE -Wshorten-64-to-32)
    endif ()
endfunction()

# For a vendored dependency, whose warnings are not ours to fix and whose noise is
# how eacp's own warnings get missed. The flag is appended after the dependency
# set its own, so it wins. Targets with no compile line of their own are skipped.
function(silence_target_warnings target)
    get_target_property(type ${target} TYPE)

    if (type STREQUAL "INTERFACE_LIBRARY" OR type STREQUAL "UTILITY")
        return()
    endif ()

    if (MSVC)
        target_compile_options(${target} PRIVATE /w)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -w)
    endif ()
endfunction()

# The bundle plist is eacp's template unless the target already has one, so
# an app's own plist, or eacp_add_plist_entries, may come before or after.
function(set_default_target_setting target)
    set_default_warnings_level(${target})
    set_target_properties(${target} PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

    if (APPLE)
        get_target_property(plist ${target} MACOSX_BUNDLE_INFO_PLIST)

        if (NOT plist)
            eacp_bundle_plist_template(template)
            set_target_properties(${target} PROPERTIES
                    MACOSX_BUNDLE_INFO_PLIST "${template}")
        endif ()
    endif ()
endfunction()

function(eacp_plist_string value out_var)
    string(REPLACE "&" "&amp;" value "${value}")
    string(REPLACE "<" "&lt;" value "${value}")
    string(REPLACE ">" "&gt;" value "${value}")
    set(${out_var} "<string>${value}</string>" PARENT_SCOPE)
endfunction()

function(eacp_plist_element value out_var)
    list(LENGTH value count)

    if (value STREQUAL "TRUE")
        set(${out_var} "<true/>" PARENT_SCOPE)
    elseif (value STREQUAL "FALSE")
        set(${out_var} "<false/>" PARENT_SCOPE)
    elseif (count GREATER 1)
        set(array "<array>\n")

        foreach (item IN LISTS value)
            eacp_plist_string("${item}" element)
            string(APPEND array "\t\t${element}\n")
        endforeach ()

        set(${out_var} "${array}\t</array>" PARENT_SCOPE)
    else ()
        eacp_plist_string("${value}" element)
        set(${out_var} "${element}" PARENT_SCOPE)
    endif ()
endfunction()

# Adds keys to an app's Info.plist on top of eacp's template for the platform,
# so an app that needs a usage description or LSUIElement does not carry a
# copy of the whole file. TRUE and FALSE become booleans, a list of more than
# one item an array of strings, anything else a string. Calls accumulate, and
# may come before or after set_default_target_setting. A no-op off Apple.
#
#   eacp_add_plist_entries(MyApp
#           NSCameraUsageDescription "Shows the camera in a GPU view."
#           LSUIElement TRUE)
function(eacp_add_plist_entries target)
    if (NOT APPLE)
        return()
    endif ()

    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "")
    list(LENGTH ARG_UNPARSED_ARGUMENTS count)
    math(EXPR remainder "${count} % 2")

    if (count EQUAL 0 OR remainder)
        message(FATAL_ERROR
                "eacp_add_plist_entries(${target}): expects key value pairs")
    endif ()

    get_target_property(entries ${target} EACP_PLIST_ENTRIES)

    if (NOT entries)
        set(entries "")
    endif ()

    math(EXPR last "${count} - 1")

    foreach (i RANGE 0 ${last} 2)
        math(EXPR j "${i} + 1")
        list(GET ARG_UNPARSED_ARGUMENTS ${i} key)
        list(GET ARG_UNPARSED_ARGUMENTS ${j} value)
        eacp_plist_element("${value}" element)
        string(APPEND entries "\t<key>${key}</key>\n\t${element}\n")
    endforeach ()

    set_target_properties(${target} PROPERTIES EACP_PLIST_ENTRIES "${entries}")

    eacp_bundle_plist_template(template)
    file(READ "${template}" plist)
    string(REPLACE "</dict>\n</plist>" "${entries}</dict>\n</plist>" plist
            "${plist}")

    set(generated "${CMAKE_CURRENT_BINARY_DIR}/${target}-Info.plist.in")
    file(WRITE "${generated}" "${plist}")
    set_target_properties(${target} PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${generated}")
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
    # The flags below are not the ones the shared PCH image was created under,
    # and MSVC rejects a mismatch outright.
    eacp_skip_pch(${target})

    if (MSVC)
        # The cl/clang-cl Debug defaults fight optimization: /RTC1 is a hard
        # error under any /O level (D8016) and /Od warns when overridden by /O2
        # (D9025). Strip both before forcing the level below.
        # Both languages: a vendored C library (ThirdParty/miniz) is forced
        # the same way, and cl's C Debug defaults carry the same two flags.
        foreach (lang C CXX)
            string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_${lang}_FLAGS_DEBUG
                    "${CMAKE_${lang}_FLAGS_DEBUG}")
            string(REGEX REPLACE "/Od" "" CMAKE_${lang}_FLAGS_DEBUG
                    "${CMAKE_${lang}_FLAGS_DEBUG}")
            set(CMAKE_${lang}_FLAGS_DEBUG "${CMAKE_${lang}_FLAGS_DEBUG}"
                    PARENT_SCOPE)
        endforeach ()

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

# Stamps an app's name, company and the enclosing project's version into the
# binary two ways: the native metadata the OS reads (macOS bundle plist keys /
# a Windows VERSIONINFO resource), and an AppInfo.json embedded via ResEmbed
# that Platform::getAppName()/getCompanyName()/getAppVersion() read back at
# runtime. The name comes from MACOSX_BUNDLE_BUNDLE_NAME (apps set it before
# calling us) or the target name; the company from the target's
# EACP_COMPANY_NAME property, else the EACP_COMPANY_NAME variable, else empty;
# the version from eacp_add_app's VERSION, else ${PROJECT_VERSION}, else 0.0.0,
# and the build from its VERSION_CODE, else the version. Name and company
# are what FilePath::appSupportDirectory() puts the app's own folder under.
function(eacp_embed_app_info target)
    get_target_property(app_name ${target} MACOSX_BUNDLE_BUNDLE_NAME)
    if (NOT app_name)
        set(app_name "${target}")
    endif ()

    get_target_property(company_name ${target} EACP_COMPANY_NAME)
    if (NOT company_name)
        set(company_name "${EACP_COMPANY_NAME}")
    endif ()

    get_target_property(app_version ${target} EACP_APP_VERSION)
    if (NOT app_version)
        set(app_version "${PROJECT_VERSION}")
    endif ()
    if (NOT app_version)
        set(app_version "0.0.0")
    endif ()

    get_target_property(build ${target} EACP_APP_VERSION_CODE)
    if (NOT build)
        set(build "${app_version}")
    endif ()

    if (APPLE)
        set_target_properties(${target} PROPERTIES
                MACOSX_BUNDLE_SHORT_VERSION_STRING "${app_version}"
                MACOSX_BUNDLE_BUNDLE_VERSION "${build}"
                MACOSX_BUNDLE_LONG_VERSION_STRING "${app_version}")
    elseif (WIN32)
        # VERSIONINFO's FILEVERSION/PRODUCTVERSION need four numeric fields; the
        # version may have fewer, so the missing ones are 0.
        string(REGEX MATCHALL "[0-9]+" fields "${app_version}")
        list(APPEND fields 0 0 0 0)
        list(SUBLIST fields 0 4 fields)
        list(JOIN fields "," numeric_version)

        # Resource ids are per-type, so id 1 here does not clash with the
        # id-1 ICON that eacp_set_app_icon emits.
        set(version_rc "${CMAKE_CURRENT_BINARY_DIR}/${target}-version.rc")
        file(CONFIGURE OUTPUT "${version_rc}" @ONLY CONTENT [==[
1 VERSIONINFO
FILEVERSION @numeric_version@
PRODUCTVERSION @numeric_version@
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
    "company": "@company_name@",
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
