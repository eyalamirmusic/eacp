# The Android Studio project. An Android configure writes a Gradle project into
# EACP_ANDROID_STUDIO_DIR (<build>/AndroidStudio) with one module per
# eacp_add_app, as -G Xcode writes an Xcode project. Gradle compiles nothing of
# its own: each module's externalNativeBuild runs the top-level CMakeLists.txt
# for that one target, once per ABI in EACP_ANDROID_ABIS, and packages the
# library behind the manifest written here. Installing, signing and the App
# Bundle for Google Play are Gradle's; nothing in eacp packages an APK.
#
# eacp_add_android_app(<target>) is called by eacp_add_app with its APP_*
# arguments in scope: the identity and versions go to the module, ORIENTATION
# to the manifest, and ICON becomes the module's mipmap/ic_launcher.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/AndroidVersions.cmake")

option(EACP_ANDROID_STUDIO "Write an Android Studio (Gradle) project for every app"
        ON)
set(EACP_ANDROID_STUDIO_DIR "${CMAKE_BINARY_DIR}/AndroidStudio" CACHE PATH
        "Where the Android Studio project is written")
set(EACP_ANDROID_ABIS "arm64-v8a;x86_64" CACHE STRING
        "The ABIs the Android Studio project builds")
option(EACP_ANDROID_CLEARTEXT_TRAFFIC "Let the app's HTTP client reach http:// URLs"
        OFF)

# Gradle's sdk.dir: the SDK the toolchain looked in, unless that is no SDK and
# the NDK this configure uses sits in one, at <sdk>/ndk/<version>.
eacp_android_find_sdk(eacp_android_sdk_default)

if (ANDROID_NDK)
    set(eacp_android_ndk_used "${ANDROID_NDK}")
else ()
    set(eacp_android_ndk_used "${CMAKE_ANDROID_NDK}")
endif ()

get_filename_component(eacp_android_ndk_sdk "${eacp_android_ndk_used}/../.."
        ABSOLUTE)

if (NOT EXISTS "${eacp_android_sdk_default}/platforms"
        AND NOT EXISTS "${eacp_android_sdk_default}/ndk"
        AND (EXISTS "${eacp_android_ndk_sdk}/platforms"
        OR EXISTS "${eacp_android_ndk_sdk}/ndk"))
    set(eacp_android_sdk_default "${eacp_android_ndk_sdk}")
endif ()

set(EACP_ANDROID_SDK "${eacp_android_sdk_default}" CACHE PATH
        "The Android SDK the Studio project builds with")

# Gradle's compileSdk, resolved once for every module: EACP_ANDROID_COMPILE_SDK,
# else the newest platform in the SDK, else targetSdk for Gradle to download.
function(eacp_android_compile_sdk out)
    get_property(level GLOBAL PROPERTY EACP_ANDROID_COMPILE_SDK_LEVEL)

    if (level)
        set(${out} "${level}" PARENT_SCOPE)
        return()
    endif ()

    set(level "${EACP_ANDROID_COMPILE_SDK}")
    set(target "${EACP_ANDROID_TARGET_SDK}")

    if (NOT level)
        eacp_android_newest_platform("${EACP_ANDROID_SDK}" level)
    endif ()

    if (NOT level)
        message(WARNING
                "No Android platform is installed in ${EACP_ANDROID_SDK}: the "
                "Studio project compiles against android-${target}, which Gradle "
                "downloads on sync if the SDK's licenses are accepted. Install a "
                "platform with Android Studio's SDK Manager (SDK Platforms), or "
                "set EACP_ANDROID_COMPILE_SDK.")
        set(level "${target}")
    endif ()

    if (NOT level MATCHES "^[0-9]+(\\.[0-9]+)?$")
        message(FATAL_ERROR
                "EACP_ANDROID_COMPILE_SDK is ${level}: give an API level, such "
                "as 35 or 37.0.")
    endif ()

    if (level VERSION_LESS target)
        message(FATAL_ERROR
                "compileSdk ${level} is below EACP_ANDROID_TARGET_SDK ${target}: "
                "install android-${target} or newer with Android Studio's SDK "
                "Manager, set EACP_ANDROID_COMPILE_SDK to ${target} or higher, or "
                "lower EACP_ANDROID_TARGET_SDK.")
    endif ()

    set_property(GLOBAL PROPERTY EACP_ANDROID_COMPILE_SDK_LEVEL "${level}")
    set(${out} "${level}" PARENT_SCOPE)
endfunction()

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

# What Gradle's configures are given beyond its own: every EACP_* and CPM_*
# setting of this configure (a -DEACP_UNITY_BUILD=OFF or a
# -DCPM_Miro_SOURCE=... reaches them as it reached this one) and any other -D
# it was given on the command line, bar the CMAKE_* and ANDROID_* that Gradle
# sets itself and the Studio project's own, the source cache, and the script
# that hands them the sources this configure fetched.
function(eacp_android_cmake_arguments out)
    set(sources "${EACP_ANDROID_STUDIO_DIR}/eacp-sources.cmake")
    set(arguments -DEACP_ANDROID_STUDIO=OFF
            "-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=${sources}")
    get_cmake_property(names CACHE_VARIABLES)

    foreach (name IN LISTS names)
        get_property(type CACHE ${name} PROPERTY TYPE)
        get_property(help CACHE ${name} PROPERTY HELPSTRING)

        if (type STREQUAL "INTERNAL" OR type STREQUAL "STATIC"
                OR name MATCHES "^(CMAKE_|ANDROID_|CPM_SOURCE_CACHE$)"
                OR name MATCHES "^EACP_ANDROID_(STUDIO|ABIS$|SDK$)")
            continue()
        endif ()

        if (NOT name MATCHES "^(EACP|CPM)_" AND NOT help STREQUAL
                "No help, variable specified on the command line.")
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

# Every package this configure fetched, by the source CPM recorded for it, as
# the CPM_<name>_SOURCE of each Gradle configure, so none clones a package
# again. It is included at their first project() on every configure, so a
# source deleted since is fetched as usual rather than failing, an override a
# configure was given is kept, and so is a source a build fetched for itself
# while it still exists: ResEmbed's generator build refuses a moved source.
function(eacp_android_write_fetched_sources)
    set(content "")

    foreach (package IN LISTS CPM_PACKAGES)
        set(source "${CPM_PACKAGE_${package}_SOURCE_DIR}")

        if (source)
            set(recorded "CPM_PACKAGE_${package}_SOURCE_DIR")
            string(APPEND content
                    "if (NOT CPM_${package}_SOURCE AND EXISTS [==[${source}]==]\n"
                    "        AND (NOT EXISTS \"\${${recorded}}\"\n"
                    "        OR ${recorded} STREQUAL [==[${source}]==]))\n"
                    "    set(CPM_${package}_SOURCE [==[${source}]==])\n"
                    "endif ()\n")
        endif ()
    endforeach ()

    set(file "${EACP_ANDROID_STUDIO_DIR}/eacp-sources.cmake")
    file(WRITE "${file}.new" "${content}")
    file(COPY_FILE "${file}.new" "${file}" ONLY_IF_DIFFERENT)
    file(REMOVE "${file}.new")
endfunction()

# The NDK this configure compiles with: Gradle's ndkVersion.
function(eacp_android_ndk_version out)
    if (ANDROID_NDK_REVISION)
        set(${out} "${ANDROID_NDK_REVISION}" PARENT_SCOPE)
        return()
    endif ()

    if (CMAKE_ANDROID_NDK)
        set(ndk "${CMAKE_ANDROID_NDK}")
    else ()
        set(ndk "${ANDROID_NDK}")
    endif ()

    file(STRINGS "${ndk}/source.properties" revision REGEX "^Pkg\\.Revision")
    string(REGEX REPLACE "^Pkg\\.Revision *= *" "" revision "${revision}")
    set(${out} "${revision}" PARENT_SCOPE)
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
    set(EACP_APK_ICON_ATTRIBUTE "")
    set(EACP_STUDIO_RES "")

    if (EACP_ANDROID_CLEARTEXT_TRAFFIC)
        set(EACP_APK_CLEARTEXT_TRAFFIC true)
    else ()
        set(EACP_APK_CLEARTEXT_TRAFFIC false)
    endif ()

    if (APP_ORIENTATION STREQUAL "portrait")
        set(EACP_APK_ORIENTATION portrait)
    elseif (APP_ORIENTATION STREQUAL "landscape")
        set(EACP_APK_ORIENTATION sensorLandscape)
    else ()
        set(EACP_APK_ORIENTATION unspecified)
    endif ()

    set(EACP_APK_PERMISSIONS "")
    set(EACP_APK_FEATURES "")

    foreach (permission IN LISTS APP_PERMISSIONS)
        if (NOT permission MATCHES "\\.")
            set(permission "android.permission.${permission}")
        endif ()

        string(APPEND EACP_APK_PERMISSIONS
                "\n    <uses-permission android:name=\"${permission}\" />")

        if (permission STREQUAL "android.permission.CAMERA")
            string(APPEND EACP_APK_FEATURES "\n    <uses-feature "
                    "android:name=\"android.hardware.camera.any\" "
                    "android:required=\"false\" />")
        endif ()
    endforeach ()

    if (APP_ICON)
        set(res "${CMAKE_CURRENT_BINARY_DIR}/${target}-res")
        configure_file("${APP_ICON}" "${res}/mipmap/ic_launcher.png" COPYONLY)
        set(EACP_APK_ICON_ATTRIBUTE "android:icon=\"@mipmap/ic_launcher\"")
        set(EACP_STUDIO_RES "
    sourceSets.getByName(\"main\").res.directories.add(\"${res}\")
")
    endif ()

    eacp_android_cmake_arguments(arguments)
    eacp_android_quoted_list(EACP_STUDIO_CMAKE_ARGUMENTS ${arguments})
    eacp_android_quoted_list(EACP_STUDIO_ABIS ${EACP_ANDROID_ABIS})
    eacp_android_ndk_version(EACP_STUDIO_NDK_VERSION)
    eacp_android_compile_sdk(compile_sdk)
    string(REPLACE "." ";" compile_sdk "${compile_sdk}.0")
    list(GET compile_sdk 0 EACP_STUDIO_COMPILE_SDK)
    list(GET compile_sdk 1 EACP_STUDIO_COMPILE_SDK_MINOR)
    set(EACP_STUDIO_CMAKE_LISTS "${CMAKE_SOURCE_DIR}/CMakeLists.txt")

    set(module "${EACP_ANDROID_STUDIO_DIR}/${target}")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/AndroidManifest.xml.in"
            "${module}/src/main/AndroidManifest.xml" @ONLY)
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Android/app.build.gradle.kts.in"
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
    set(templates "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Android")
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
        configure_file("${templates}/${file}.in" "${dir}/${file}" @ONLY)
    endforeach ()

    configure_file("${templates}/gradle-wrapper.properties.in"
            "${dir}/gradle/wrapper/gradle-wrapper.properties" @ONLY)
    file(COPY "${templates}/wrapper/gradle-wrapper.jar"
            DESTINATION "${dir}/gradle/wrapper")
    file(COPY "${templates}/wrapper/gradlew"
            "${templates}/wrapper/gradlew.bat"
            DESTINATION "${dir}")
    eacp_android_write_fetched_sources()

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
