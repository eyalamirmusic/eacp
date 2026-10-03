# The Android Studio project. An Android configure writes a Gradle project into
# EACP_ANDROID_STUDIO_DIR (<build>/AndroidStudio) with one module per
# eacp_add_app, as -G Xcode writes an Xcode project. Gradle compiles nothing of
# its own: each module's externalNativeBuild runs the top-level CMakeLists.txt
# for that one target, once per ABI in EACP_ANDROID_ABIS, and packages the
# library behind the manifest written here. Installing, signing and the App
# Bundle for Google Play are Gradle's; nothing in eacp packages an APK.
#
# eacp_add_android_app(<target>) is called by eacp_add_app with its APP_*
# arguments in scope. The manifest is eacp's, plus MANIFEST_ELEMENTS inside
# <manifest> and APPLICATION_ATTRIBUTES and ACTIVITY_ATTRIBUTES on those two,
# each the text itself or a file holding it. The icon is RES_DIR's
# mipmap/ic_launcher where it has one, else ICON.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/AndroidVersions.cmake")

option(EACP_ANDROID_STUDIO "Write an Android Studio (Gradle) project for every app"
        ON)
set(EACP_ANDROID_STUDIO_DIR "${CMAKE_BINARY_DIR}/AndroidStudio" CACHE PATH
        "Where the Android Studio project is written")
set(EACP_ANDROID_ABIS "arm64-v8a;x86_64" CACHE STRING
        "The ABIs the Android Studio project builds")

# The SDK the NDK sits in, at <sdk>/ndk/<version>: Gradle's sdk.dir.
get_filename_component(eacp_android_sdk_default "${ANDROID_NDK}/../.." ABSOLUTE)
set(EACP_ANDROID_SDK "${eacp_android_sdk_default}" CACHE PATH
        "The Android SDK the Studio project builds with")

set(EACP_ANDROID_TEMPLATES "${CMAKE_CURRENT_LIST_DIR}/Android")

function(eacp_android_quoted_list out)
    set(quoted "")

    foreach (item IN LISTS ARGN)
        string(REPLACE "\\" "\\\\" item "${item}")
        string(REPLACE "\"" "\\\"" item "${item}")
        string(REPLACE "$" "\${'$'}" item "${item}")
        list(APPEND quoted "\"${item}\"")
    endforeach ()

    list(JOIN quoted ", " joined)
    set(${out} "${joined}" PARENT_SCOPE)
endfunction()

# What Gradle's configures are given beyond its own: every -D this configure
# was given on the command line (a -DEACP_UNITY_BUILD=OFF or a
# -DCPM_Miro_SOURCE=... reaches them as it reached this one), bar the CMAKE_*
# and ANDROID_* that Gradle sets itself, and the source cache, so no module
# fetches a package twice.
function(eacp_android_cmake_arguments out)
    set(arguments -DEACP_ANDROID_STUDIO=OFF)
    get_cmake_property(names CACHE_VARIABLES)

    foreach (name IN LISTS names)
        get_property(help CACHE ${name} PROPERTY HELPSTRING)

        if (NOT help STREQUAL "No help, variable specified on the command line."
                OR name MATCHES "^(CMAKE_|ANDROID_|EACP_ANDROID_STUDIO)")
            continue()
        endif ()

        get_property(value CACHE ${name} PROPERTY VALUE)
        list(APPEND arguments "-D${name}=${value}")
    endforeach ()

    if (CPM_SOURCE_CACHE)
        list(APPEND arguments "-DCPM_SOURCE_CACHE=${CPM_SOURCE_CACHE}")
    endif ()

    set(${out} "${arguments}" PARENT_SCOPE)
endfunction()

function(eacp_add_android_app target)
    if (NOT EACP_ANDROID_STUDIO)
        return()
    endif ()

    set(EACP_APK_PACKAGE "${APP_BUNDLE_ID}")
    set(EACP_APK_LABEL "${APP_DISPLAY_NAME}")
    set(EACP_APK_VERSION_CODE "${APP_VERSION_CODE}")
    set(EACP_APK_VERSION_NAME "${APP_VERSION}")
    set(EACP_APK_MIN_SDK "${ANDROID_PLATFORM_LEVEL}")
    set(EACP_APK_LIB_NAME "${target}")
    set(EACP_APK_ORIENTATION "${APP_ORIENTATION}")
    set(EACP_APK_ICON_ATTRIBUTE "")

    if (NOT EACP_APK_ORIENTATION)
        set(EACP_APK_ORIENTATION unspecified)
    endif ()

    if (APP_RES_DIR)
        get_filename_component(APP_RES_DIR "${APP_RES_DIR}" ABSOLUTE)
        file(GLOB launcher "${APP_RES_DIR}/mipmap*/ic_launcher.*")
    elseif (APP_ICON)
        set(APP_RES_DIR "${CMAKE_CURRENT_BINARY_DIR}/${target}-res")
        configure_file("${APP_ICON}" "${APP_RES_DIR}/mipmap/ic_launcher.png" COPYONLY)
        set(launcher TRUE)
    endif ()

    if (launcher)
        set(EACP_APK_ICON_ATTRIBUTE "android:icon=\"@mipmap/ic_launcher\"")
    endif ()

    foreach (slot MANIFEST_ELEMENTS APPLICATION_ATTRIBUTES ACTIVITY_ATTRIBUTES)
        set(EACP_APK_${slot} "${APP_${slot}}")
        get_filename_component(file "${APP_${slot}}" ABSOLUTE)

        if (EXISTS "${file}" AND NOT IS_DIRECTORY "${file}")
            file(READ "${file}" EACP_APK_${slot})
        endif ()
    endforeach ()

    set(EACP_STUDIO_RES "")

    if (APP_RES_DIR)
        set(EACP_STUDIO_RES "
    sourceSets.getByName(\"main\").res.directories.add(\"${APP_RES_DIR}\")
")
    endif ()

    eacp_android_cmake_arguments(arguments)
    eacp_android_quoted_list(EACP_STUDIO_CMAKE_ARGUMENTS ${arguments})
    eacp_android_quoted_list(EACP_STUDIO_ABIS ${EACP_ANDROID_ABIS})
    set(EACP_STUDIO_CMAKE_LISTS "${CMAKE_SOURCE_DIR}/CMakeLists.txt")

    set(module "${EACP_ANDROID_STUDIO_DIR}/${target}")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/AndroidManifest.xml.in"
            "${module}/src/main/AndroidManifest.xml" @ONLY)
    configure_file("${EACP_ANDROID_TEMPLATES}/app.build.gradle.kts.in"
            "${module}/build.gradle.kts" @ONLY)

    set_property(GLOBAL APPEND PROPERTY EACP_ANDROID_APPS ${target})
endfunction()

# The project around the modules, once every app is declared.
function(eacp_write_android_studio_project)
    get_property(apps GLOBAL PROPERTY EACP_ANDROID_APPS)

    if (NOT apps)
        return()
    endif ()

    set(dir "${EACP_ANDROID_STUDIO_DIR}")
    set(EACP_STUDIO_NAME "${CMAKE_PROJECT_NAME}")
    set(EACP_STUDIO_SDK_DIR "${EACP_ANDROID_SDK}")
    set(EACP_STUDIO_INCLUDES "")

    foreach (app IN LISTS apps)
        string(APPEND EACP_STUDIO_INCLUDES "include(\":${app}\")\n")
    endforeach ()

    # Gradle runs the CMake that ran this configure, and looks for Ninja beside
    # it, in the SDK's CMake package, or on its PATH.
    get_filename_component(cmake_bin "${CMAKE_COMMAND}" DIRECTORY)
    get_filename_component(EACP_STUDIO_CMAKE_DIR "${cmake_bin}" DIRECTORY)

    foreach (file settings.gradle.kts build.gradle.kts gradle.properties
            local.properties)
        configure_file("${EACP_ANDROID_TEMPLATES}/${file}.in" "${dir}/${file}" @ONLY)
    endforeach ()

    configure_file("${EACP_ANDROID_TEMPLATES}/gradle-wrapper.properties.in"
            "${dir}/gradle/wrapper/gradle-wrapper.properties" @ONLY)
    file(COPY "${EACP_ANDROID_TEMPLATES}/wrapper/gradle-wrapper.jar"
            DESTINATION "${dir}/gradle/wrapper")
    file(COPY "${EACP_ANDROID_TEMPLATES}/wrapper/gradlew"
            "${EACP_ANDROID_TEMPLATES}/wrapper/gradlew.bat"
            DESTINATION "${dir}")

    list(LENGTH apps count)
    list(GET apps 0 first)
    message(STATUS "Android Studio project with ${count} app(s): ${dir}\n"
            "   Open that folder in Android Studio, or from a terminal in it: "
            "./gradlew :${first}:installDebug")
endfunction()

if (EACP_ANDROID_STUDIO)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
            CALL eacp_write_android_studio_project)
endif ()
