# Usage: cmake -P Scripts/android-setup.cmake
#
# Makes the Android SDK eacp builds with, from nothing, in any shell: the SDK
# at $ANDROID_HOME, else ~/.eacp/android/sdk, which `cmake --preset android`
# finds with no variable set; Java from $JAVA_HOME or the PATH when it is 17 or
# later, else a Temurin 21 JDK in ~/.eacp/android/jdk, which the packaging
# script finds the same way; the SDK command-line tools; and exactly the
# packages CMake/AndroidVersions.cmake names. It writes the license file
# sdkmanager --licenses would, which accepts the Android SDK License
# (https://developer.android.com/studio/terms) on your behalf.
#
# No emulator and no system image: eacp runs on a phone over USB. Run it again
# whenever AndroidVersions.cmake changes; what is present is kept, and a
# complete SDK is checked in a second with nothing downloaded.

cmake_minimum_required(VERSION 3.31)

set(eacp_script android-setup)
include("${CMAKE_CURRENT_LIST_DIR}/android-common.cmake")

set(sdk "${eacp_android_sdk}")
set(platform "android-${EACP_ANDROID_TARGET_SDK}")
set(work "${eacp_android_dir}/download")

function(download url file)
    eacp_say("downloading ${url}")
    file(DOWNLOAD "${url}" "${file}" STATUS status SHOW_PROGRESS ${ARGN})
    list(GET status 0 code)

    if (NOT code EQUAL 0)
        list(GET status 1 error)
        eacp_fail("could not download ${url}: ${error}")
    endif ()
endfunction()

function(native path out)
    file(TO_NATIVE_PATH "${path}" native)
    set(${out} "${native}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${work}")

eacp_find_java(java)

if (java)
    eacp_java_major("${java}" major)
    native("${java}" shown)
    eacp_say("Java ${major} at ${shown}")
else ()
    # The JDK rather than the JRE: packaging needs its jar tool.
    set(api "https://api.adoptium.net/v3/assets/latest/21/hotspot")
    download("${api}?os=${eacp_android_os}&architecture=${eacp_android_arch}&image_type=jdk&vendor=eclipse"
            "${work}/jdk.json")
    file(READ "${work}/jdk.json" json)
    string(JSON url GET "${json}" 0 binary package link)
    string(JSON sha256 GET "${json}" 0 binary package checksum)
    get_filename_component(archive "${url}" NAME)

    download("${url}" "${work}/${archive}" EXPECTED_HASH SHA256=${sha256})
    file(ARCHIVE_EXTRACT INPUT "${work}/${archive}" DESTINATION "${work}/jdk")
    file(GLOB home LIST_DIRECTORIES true "${work}/jdk/*")

    if (EXISTS "${home}/Contents/Home")
        set(home "${home}/Contents/Home")
    endif ()

    file(REMOVE_RECURSE "${eacp_android_dir}/jdk")
    file(RENAME "${home}" "${eacp_android_dir}/jdk")
    eacp_find_java(java)

    if (NOT java)
        eacp_fail("the JDK in ${eacp_android_dir}/jdk does not run")
    endif ()

    native("${eacp_android_dir}/jdk" shown)
    eacp_say("installed Temurin 21 at ${shown}")
endif ()

set(tools "${sdk}/cmdline-tools/latest")

if (NOT EXISTS "${tools}/lib/sdkmanager-classpath.jar")
    if (eacp_android_os STREQUAL "windows")
        set(host win)
    elseif (eacp_android_os STREQUAL "linux")
        set(host linux)
    elseif (eacp_android_arch STREQUAL "aarch64")
        set(host mac_arm64)
    else ()
        set(host mac_x86_64)
    endif ()

    set(zip "commandlinetools-${host}-${EACP_ANDROID_CMDLINE_TOOLS}_latest.zip")
    download("https://dl.google.com/android/repository/${zip}" "${work}/${zip}")
    file(ARCHIVE_EXTRACT INPUT "${work}/${zip}" DESTINATION "${work}")
    file(REMOVE_RECURSE "${tools}")
    file(MAKE_DIRECTORY "${sdk}/cmdline-tools")
    file(RENAME "${work}/cmdline-tools" "${tools}")
    native("${tools}" shown)
    eacp_say("installed the SDK command-line tools at ${shown}")
endif ()

# The hash sdkmanager --licenses writes once the Android SDK License is
# accepted. Every package below is under that one license.
set(license 24333f8a63b6825ea9c5514f83c2829b004d1fee)
set(license_file "${sdk}/licenses/android-sdk-license")
set(accepted "")

if (EXISTS "${license_file}")
    file(READ "${license_file}" accepted)
endif ()

if (NOT accepted MATCHES "${license}")
    file(APPEND "${license_file}" "\n${license}\n")
    native("${sdk}/licenses" shown)
    eacp_say("accepted the Android SDK License in ${shown}")
endif ()

# Package paths are written with "/" here, since ";" splits a CMake list,
# and go to sdkmanager with their own ";" in a package file.
set(missing "")
set(packages "")

foreach (dir IN ITEMS platform-tools "platforms/${platform}"
        "build-tools/${EACP_ANDROID_BUILD_TOOLS}" "ndk/${EACP_ANDROID_NDK_VERSION}")
    if (NOT EXISTS "${sdk}/${dir}/source.properties")
        list(APPEND missing "${dir}")
        string(REPLACE "/" ";" package "${dir}")
        string(APPEND packages "${package}\n")
    endif ()
endforeach ()

if (missing)
    file(WRITE "${work}/packages.txt" "${packages}")
    list(JOIN missing ", " shown)
    eacp_say("installing ${shown}")

    # sdkmanager run the way its own launcher runs it, the same on every host.
    # Its stdin is an empty file, so a prompt that should never come fails
    # rather than waits.
    file(TOUCH "${work}/empty")
    execute_process(
            COMMAND "${java}" "-Dcom.android.sdklib.toolsdir=${tools}"
                    -classpath "${tools}/lib/sdkmanager-classpath.jar"
                    com.android.sdklib.tool.sdkmanager.SdkManagerCli
                    "--sdk_root=${sdk}" "--package_file=${work}/packages.txt"
            INPUT_FILE "${work}/empty"
            RESULT_VARIABLE failed)

    if (failed)
        eacp_fail("sdkmanager could not install ${shown}")
    endif ()
endif ()

file(REMOVE_RECURSE "${work}")

foreach (dir IN ITEMS platform-tools "platforms/${platform}"
        "build-tools/${EACP_ANDROID_BUILD_TOOLS}" "ndk/${EACP_ANDROID_NDK_VERSION}")
    if (NOT EXISTS "${sdk}/${dir}/source.properties")
        eacp_fail("${sdk}/${dir} is missing")
    endif ()
endforeach ()

native("${sdk}" shown)
eacp_say("SDK at ${shown}: platform-tools, ${platform}, build-tools "
        "${EACP_ANDROID_BUILD_TOOLS}, NDK ${EACP_ANDROID_NDK_VERSION}")

find_program(ninja ninja NO_CACHE)

if (NOT ninja)
    eacp_say("ninja is not on the PATH; the build needs it")
endif ()

set(exe "")

if (CMAKE_HOST_WIN32)
    set(exe ".exe")
endif ()

native("${sdk}/platform-tools/adb${exe}" adb)

if (CMAKE_HOST_WIN32)
    set(exe ".cmd")
endif ()

native("${sdk}/ndk/${EACP_ANDROID_NDK_VERSION}/ndk-stack${exe}" ndk_stack)
eacp_say("done. Turn on USB debugging on a phone (Android 13+, Vulkan 1.3), "
        "plug it in, accept the prompt on it, and from the eacp checkout:")
eacp_say("    cmake --preset android")
eacp_say("    cmake --build --preset android --target HelloGPU-run")
eacp_say("adb is ${adb}")
eacp_say("ndk-stack is ${ndk_stack}")
