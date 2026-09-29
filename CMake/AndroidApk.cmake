# eacp_add_android_apk(<target> PACKAGE <id> [LABEL <name>] [ORIENTATION <o>]
#                      [VERSION_CODE <n>] [VERSION_NAME <s>] [RES_DIR <dir>]
#                      [ICON <@mipmap/name>])
#
# Adds <target>-apk: the shared library <target> behind a NativeActivity,
# packaged and debug-signed by Scripts/android-apk (no Gradle). The APK lands
# at ${CMAKE_CURRENT_BINARY_DIR}/<target>.apk.
#
# And <target>-run, which builds the APK, then installs and launches it through
# Scripts/android-run on the device adb sees, booting an emulator ($EACP_AVD,
# or the first AVD) when none is attached.

set(EACP_ANDROID_APK_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../Scripts/android-apk")
set(EACP_ANDROID_RUN_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../Scripts/android-run")
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

set(EACP_ANDROID_BUILD_TOOLS "35.0.0" CACHE STRING
        "Android SDK build-tools version that packages APKs")
set(EACP_ANDROID_TARGET_SDK "35" CACHE STRING
        "targetSdkVersion, and the android.jar the manifest links against")

function(eacp_add_android_apk target)
    cmake_parse_arguments(APK ""
            "PACKAGE;LABEL;ORIENTATION;VERSION_CODE;VERSION_NAME;RES_DIR;ICON" ""
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

    set(EACP_APK_MIN_SDK "${ANDROID_PLATFORM_LEVEL}")
    set(EACP_APK_TARGET_SDK "${EACP_ANDROID_TARGET_SDK}")
    set(EACP_APK_LIB_NAME "${target}")

    set(manifest "${CMAKE_CURRENT_BINARY_DIR}/${target}-AndroidManifest.xml")
    configure_file("${EACP_ANDROID_MANIFEST_TEMPLATE}" "${manifest}" @ONLY)

    set(sdk "${EACP_ANDROID_SDK}")
    set(apk "${CMAKE_CURRENT_BINARY_DIR}/${target}.apk")

    add_custom_command(
            OUTPUT "${apk}"
            COMMAND "${EACP_ANDROID_APK_SCRIPT}"
                    "${sdk}"
                    "${EACP_ANDROID_BUILD_TOOLS}"
                    "android-${EACP_ANDROID_TARGET_SDK}"
                    "${manifest}"
                    "$<TARGET_FILE:${target}>"
                    "${ANDROID_ABI}"
                    "${apk}"
                    "${APK_RES_DIR}"
                    "$<$<CONFIG:Debug>:debug>"
            DEPENDS ${target} "${manifest}" "${EACP_ANDROID_APK_SCRIPT}"
            COMMENT "Packaging ${target}.apk"
            VERBATIM)

    add_custom_target(${target}-apk ALL DEPENDS "${apk}")

    add_custom_target(${target}-run
            COMMAND "${EACP_ANDROID_RUN_SCRIPT}" "${sdk}" "${apk}" "${APK_PACKAGE}"
            DEPENDS ${target}-apk
            USES_TERMINAL
            VERBATIM)
endfunction()
