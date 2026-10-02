# eacp_add_android_apk(<target>), called by eacp_add_app, whose APP_* arguments
# it reads, for the shared library <target> behind a NativeActivity:
#
# <target>-apk, packaged and debug-signed by Scripts/android-apk.cmake (no
# Gradle) at ${CMAKE_CURRENT_BINARY_DIR}/<target>.apk. Its manifest is eacp's,
# plus MANIFEST_ELEMENTS inside <manifest> and APPLICATION_ATTRIBUTES and
# ACTIVITY_ATTRIBUTES on those two, each the text itself or a file holding it.
# The icon is RES_DIR's mipmap/ic_launcher where it has one, else ICON.
#
# <target>-run, which builds the APK, then installs and launches it through
# Scripts/android-run.cmake on the device adb sees: a phone over USB, or an
# emulator it boots ($EACP_AVD, or the first AVD) where the host has one.
#
# And <target>-aab, the App Bundle for Google Play that Scripts/android-bundle.cmake
# builds from a Release library per ABI in EACP_ANDROID_ABIS, at
# ${CMAKE_CURRENT_BINARY_DIR}/<target>.aab, signed with the upload key in
# EACP_ANDROID_KEYSTORE, EACP_ANDROID_KEY_ALIAS and EACP_ANDROID_KEYSTORE_PASSWORD.

include("${CMAKE_CURRENT_LIST_DIR}/AndroidVersions.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/AndroidStudio.cmake")

# CMake scripts, so they run the same with no shell on any host.
set(EACP_ANDROID_APK_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../Scripts/android-apk.cmake")
set(EACP_ANDROID_RUN_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../Scripts/android-run.cmake")
set(EACP_ANDROID_BUNDLE_SCRIPT
        "${CMAKE_CURRENT_LIST_DIR}/../Scripts/android-bundle.cmake")
set(EACP_ANDROID_COMMON_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../Scripts/android-common.cmake")
set(EACP_ANDROID_MANIFEST_TEMPLATE
        "${CMAKE_CURRENT_LIST_DIR}/AndroidManifest.xml.in")

# The SDK the NDK sits in, at <sdk>/ndk/<version>.
get_filename_component(eacp_android_sdk_default "${ANDROID_NDK}/../.." ABSOLUTE)
set(EACP_ANDROID_SDK "${eacp_android_sdk_default}" CACHE PATH
        "Android SDK that packages, installs and runs APKs")

function(eacp_add_android_apk target)
    set(EACP_APK_PACKAGE "${APP_BUNDLE_ID}")
    set(EACP_APK_LABEL "${APP_DISPLAY_NAME}")
    set(EACP_APK_ORIENTATION "${APP_ORIENTATION}")
    set(EACP_APK_VERSION_CODE "${APP_VERSION_CODE}")
    set(EACP_APK_VERSION_NAME "${APP_VERSION}")
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
                    "-DRES_DIR=${APP_RES_DIR}"
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
            RES_DIR "${APP_RES_DIR}"
            MIN_SDK "${EACP_APK_MIN_SDK}"
            MANIFEST "${manifest}")

    add_custom_target(${target}-run
            COMMAND "${CMAKE_COMMAND}" "-DSDK=${sdk}" "-DAPK=${apk}"
                    "-DPACKAGE=${EACP_APK_PACKAGE}" -P "${EACP_ANDROID_RUN_SCRIPT}"
            DEPENDS ${target}-apk
            USES_TERMINAL
            VERBATIM)

    file(RELATIVE_PATH library_dir "${CMAKE_BINARY_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
    set(config "${CMAKE_CURRENT_BINARY_DIR}/${target}-aab.cmake")
    set_property(GLOBAL APPEND PROPERTY EACP_ANDROID_BUNDLES "${config}")
    set_property(GLOBAL PROPERTY EACP_ANDROID_BUNDLE_${config} "\
set(TARGET [==[${target}]==])
set(LIBRARY [==[${library_dir}/lib${target}.so]==])
set(MANIFEST [==[${manifest}]==])
set(RES_DIR [==[${APP_RES_DIR}]==])
set(OUT [==[${CMAKE_CURRENT_BINARY_DIR}/${target}.aab]==])
")

    add_custom_target(${target}-aab
            COMMAND "${CMAKE_COMMAND}" "-DCONFIG=${config}"
                    -P "${EACP_ANDROID_BUNDLE_SCRIPT}"
            USES_TERMINAL
            VERBATIM)
endfunction()

# Each <target>-aab's settings, written once every package is fetched, for the
# Release configure per ABI to build the same sources.
function(eacp_android_write_bundles)
    eacp_android_nested_init_cache(includes)
    get_property(configs GLOBAL PROPERTY EACP_ANDROID_BUNDLES)

    foreach (config IN LISTS configs)
        get_property(app GLOBAL PROPERTY EACP_ANDROID_BUNDLE_${config})
        file(WRITE "${config}" "${app}\
set(SDK [==[${EACP_ANDROID_SDK}]==])
set(SOURCE [==[${CMAKE_SOURCE_DIR}]==])
set(BUILD_ROOT [==[${CMAKE_BINARY_DIR}/aab]==])
set(ABIS [==[${EACP_ANDROID_ABIS}]==])
set(STRIP [==[${CMAKE_STRIP}]==])
set(OBJCOPY [==[${CMAKE_OBJCOPY}]==])
set(CONFIGURE_ARGUMENTS [==[-G;${CMAKE_GENERATOR};\
-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM};\
-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE};\
-DANDROID_PLATFORM=${ANDROID_PLATFORM};-DCMAKE_BUILD_TYPE=Release;-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=${includes}]==])
")
    endforeach ()
endfunction()

get_property(eacp_android_bundles_deferred GLOBAL PROPERTY EACP_ANDROID_BUNDLES_DEFERRED)

if (NOT eacp_android_bundles_deferred)
    set_property(GLOBAL PROPERTY EACP_ANDROID_BUNDLES_DEFERRED TRUE)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL eacp_android_write_bundles)
endif ()
