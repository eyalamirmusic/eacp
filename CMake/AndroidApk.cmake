# eacp_add_android_apk(<target> PACKAGE <id> [LABEL <name>] [ORIENTATION <o>]
#                      [VERSION_CODE <n>] [VERSION_NAME <s>] [RES_DIR <dir>]
#                      [ICON <@mipmap/name>] [MANIFEST_ELEMENTS <xml>]
#                      [APPLICATION_ATTRIBUTES <xml>] [ACTIVITY_ATTRIBUTES <xml>])
#
# Adds <target>-apk: the shared library <target> behind a NativeActivity,
# packaged and debug-signed by Scripts/android-apk.cmake (no Gradle). The APK
# lands at ${CMAKE_CURRENT_BINARY_DIR}/<target>.apk. The last three add to
# eacp's manifest: elements inside <manifest>, and attributes of <application>
# and of <activity>, each given as the text itself or a file holding it.
#
# And <target>-run, which builds the APK, then installs and launches it through
# Scripts/android-run.cmake on the device adb sees: a phone over USB, or an
# emulator it boots ($EACP_AVD, or the first AVD) where the host has one.

include("${CMAKE_CURRENT_LIST_DIR}/AndroidVersions.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/AndroidStudio.cmake")

# CMake scripts, so they run the same with no shell on any host.
set(EACP_ANDROID_APK_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../Scripts/android-apk.cmake")
set(EACP_ANDROID_RUN_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../Scripts/android-run.cmake")
set(EACP_ANDROID_COMMON_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../Scripts/android-common.cmake")
set(EACP_ANDROID_MANIFEST_TEMPLATE
        "${CMAKE_CURRENT_LIST_DIR}/AndroidManifest.xml.in")

# The NDK usually sits at <sdk>/ndk/<version>.
if (DEFINED ENV{ANDROID_HOME})
    set(eacp_android_sdk_default "$ENV{ANDROID_HOME}")
elseif (DEFINED ENV{ANDROID_SDK_ROOT})
    set(eacp_android_sdk_default "$ENV{ANDROID_SDK_ROOT}")
else ()
    get_filename_component(eacp_android_sdk_default "${ANDROID_NDK}/../.." ABSOLUTE)
endif ()

set(EACP_ANDROID_SDK "${eacp_android_sdk_default}" CACHE PATH
        "Android SDK that packages, installs and runs APKs")

function(eacp_add_android_apk target)
    cmake_parse_arguments(APK ""
            "PACKAGE;LABEL;ORIENTATION;VERSION_CODE;VERSION_NAME;RES_DIR;ICON;\
MANIFEST_ELEMENTS;APPLICATION_ATTRIBUTES;ACTIVITY_ATTRIBUTES" ""
            ${ARGN})

    if (NOT APK_PACKAGE)
        message(FATAL_ERROR "eacp_add_android_apk(${target}): PACKAGE is required")
    endif ()

    set(EACP_APK_PACKAGE "${APK_PACKAGE}")
    set(EACP_APK_LABEL "${target}")
    set(EACP_APK_ORIENTATION "unspecified")
    set(EACP_APK_VERSION_CODE "1")
    set(EACP_APK_VERSION_NAME "${PROJECT_VERSION}")
    set(EACP_APK_ICON_ATTRIBUTE "")

    if (APK_LABEL)
        set(EACP_APK_LABEL "${APK_LABEL}")
    endif ()

    if (APK_ORIENTATION)
        set(EACP_APK_ORIENTATION "${APK_ORIENTATION}")
    endif ()

    if (APK_VERSION_CODE)
        set(EACP_APK_VERSION_CODE "${APK_VERSION_CODE}")
    endif ()

    if (APK_VERSION_NAME)
        set(EACP_APK_VERSION_NAME "${APK_VERSION_NAME}")
    endif ()

    if (NOT EACP_APK_VERSION_NAME)
        set(EACP_APK_VERSION_NAME "0.1")
    endif ()

    if (APK_ICON)
        set(EACP_APK_ICON_ATTRIBUTE "android:icon=\"${APK_ICON}\"")
    endif ()

    foreach (slot MANIFEST_ELEMENTS APPLICATION_ATTRIBUTES ACTIVITY_ATTRIBUTES)
        set(EACP_APK_${slot} "${APK_${slot}}")
        get_filename_component(file "${APK_${slot}}" ABSOLUTE)

        if (EXISTS "${file}" AND NOT IS_DIRECTORY "${file}")
            file(READ "${file}" EACP_APK_${slot})
        endif ()
    endforeach ()

    set(EACP_APK_MIN_SDK "${ANDROID_PLATFORM_LEVEL}")
    set(EACP_APK_TARGET_SDK "${EACP_ANDROID_TARGET_SDK}")
    set(EACP_APK_LIB_NAME "${target}")

    set(manifest "${CMAKE_CURRENT_BINARY_DIR}/${target}-AndroidManifest.xml")
    configure_file("${EACP_ANDROID_MANIFEST_TEMPLATE}" "${manifest}" @ONLY)

    set(sdk "${EACP_ANDROID_SDK}")
    set(apk "${CMAKE_CURRENT_BINARY_DIR}/${target}.apk")

    add_custom_command(
            OUTPUT "${apk}"
            COMMAND "${CMAKE_COMMAND}"
                    "-DSDK=${sdk}"
                    "-DBUILD_TOOLS=${EACP_ANDROID_BUILD_TOOLS}"
                    "-DPLATFORM=android-${EACP_ANDROID_TARGET_SDK}"
                    "-DMANIFEST=${manifest}"
                    "-DLIBRARY=$<TARGET_FILE:${target}>"
                    "-DABI=${ANDROID_ABI}"
                    "-DSTRIP=${CMAKE_STRIP}"
                    "-DOUT=${apk}"
                    "-DRES_DIR=${APK_RES_DIR}"
                    "-DDEBUG=$<CONFIG:Debug>"
                    -P "${EACP_ANDROID_APK_SCRIPT}"
            DEPENDS ${target} "${manifest}" "${EACP_ANDROID_APK_SCRIPT}"
                    "${EACP_ANDROID_COMMON_SCRIPT}"
            COMMENT "Packaging ${target}.apk"
            VERBATIM)

    add_custom_target(${target}-apk ALL DEPENDS "${apk}")

    eacp_android_studio_add_app(${target}
            PACKAGE "${EACP_APK_PACKAGE}"
            VERSION_CODE "${EACP_APK_VERSION_CODE}"
            VERSION_NAME "${EACP_APK_VERSION_NAME}"
            RES_DIR "${APK_RES_DIR}"
            MIN_SDK "${EACP_APK_MIN_SDK}"
            MANIFEST "${manifest}")

    add_custom_target(${target}-run
            COMMAND "${CMAKE_COMMAND}" "-DSDK=${sdk}" "-DAPK=${apk}"
                    "-DPACKAGE=${APK_PACKAGE}" -P "${EACP_ANDROID_RUN_SCRIPT}"
            DEPENDS ${target}-apk
            USES_TERMINAL
            VERBATIM)
endfunction()
