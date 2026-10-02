# Usage: cmake -P Scripts/android-setup.cmake
#
# Makes the Android SDK eacp builds with, from nothing, in any shell: the SDK
# at $ANDROID_HOME, else ~/.eacp/android/sdk, which CMake/AndroidToolchain.cmake
# finds with no variable set; Java from $JAVA_HOME, Android Studio or the PATH
# when it is 17 or later, else a Temurin 21 JDK in ~/.eacp/android/jdk, which
# the packaging script finds the same way; the SDK command-line tools; and
# exactly the packages CMake/AndroidVersions.cmake names. It writes the license file
# sdkmanager --licenses would, which accepts the Android SDK License
# (https://developer.android.com/studio/terms) on your behalf, and for the
# arm64-v8a system image the android-sdk-arm-dbt-license too.
#
# Where Google ships an Android Emulator for the host (all but Windows and
# Linux on ARM), also the emulator, the system image AndroidVersions.cmake
# names for the host's ABI, and an AVD called eacp, which HelloGPU-run boots
# when no phone is attached. -DEACP_ANDROID_EMULATOR=OFF leaves all three out,
# for a phone over USB only. Run it again whenever AndroidVersions.cmake
# changes; what is present is kept, and a complete SDK is checked in a second
# with nothing downloaded.

cmake_minimum_required(VERSION 3.31)

set(eacp_script android-setup)
include("${CMAKE_CURRENT_LIST_DIR}/android-common.cmake")

set(sdk "${eacp_android_sdk}")
set(platform "android-${EACP_ANDROID_TARGET_SDK}")
set(work "${eacp_android_dir}/download")

if (NOT DEFINED EACP_ANDROID_EMULATOR)
    set(EACP_ANDROID_EMULATOR ON)
endif ()

set(emulator_wanted FALSE)

if (EACP_ANDROID_EMULATOR AND eacp_android_has_emulator)
    set(emulator_wanted TRUE)
endif ()

# Package paths are written with "/" here, since ";" splits a CMake list,
# and go to sdkmanager with their own ";" in a package file.
set(dirs platform-tools "platforms/${platform}"
        "build-tools/${EACP_ANDROID_BUILD_TOOLS}" "ndk/${EACP_ANDROID_NDK_VERSION}")
string(REPLACE ";" "/" image_dir "${eacp_android_image}")

if (emulator_wanted)
    list(APPEND dirs emulator "${image_dir}")
endif ()

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

# The hashes sdkmanager --licenses writes once a license is accepted. Every
# package is under the Android SDK License but the ARM system image, which
# Google puts under its own ARM DBT license.
function(accept_license name hash)
    set(file "${sdk}/licenses/${name}")
    set(accepted "")

    if (EXISTS "${file}")
        file(READ "${file}" accepted)
    endif ()

    if (NOT accepted MATCHES "${hash}")
        file(APPEND "${file}" "\n${hash}\n")
        native("${sdk}/licenses" shown)
        eacp_say("accepted ${name} in ${shown}")
    endif ()
endfunction()

accept_license(android-sdk-license 24333f8a63b6825ea9c5514f83c2829b004d1fee)

if (emulator_wanted AND eacp_android_image_abi STREQUAL "arm64-v8a")
    accept_license(android-sdk-arm-dbt-license
            859f317696f67ef3d7f30a50a5560e7834b43903)
endif ()

set(missing "")
set(packages "")

foreach (dir IN LISTS dirs)
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

foreach (dir IN LISTS dirs)
    if (NOT EXISTS "${sdk}/${dir}/source.properties")
        eacp_fail("${sdk}/${dir} is missing")
    endif ()
endforeach ()

native("${sdk}" shown)
eacp_say("SDK at ${shown}: platform-tools, ${platform}, build-tools "
        "${EACP_ANDROID_BUILD_TOOLS}, NDK ${EACP_ANDROID_NDK_VERSION}")

set(avd eacp)

# Where avdmanager and the emulator both keep AVDs.
function(avd_home out)
    if (DEFINED ENV{ANDROID_AVD_HOME} AND NOT "$ENV{ANDROID_AVD_HOME}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{ANDROID_AVD_HOME}" home)
    elseif (DEFINED ENV{ANDROID_USER_HOME} AND NOT "$ENV{ANDROID_USER_HOME}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{ANDROID_USER_HOME}/avd" home)
    elseif (DEFINED ENV{ANDROID_EMULATOR_HOME}
            AND NOT "$ENV{ANDROID_EMULATOR_HOME}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{ANDROID_EMULATOR_HOME}/avd" home)
    else ()
        set(home "${eacp_home}/.android/avd")
    endif ()

    set(${out} "${home}" PARENT_SCOPE)
endfunction()

# The emulator's own settings an AVD made by avdmanager leaves at a default
# that suits eacp badly: GPU on (HelloGPU-run passes -gpu host), a hardware
# keyboard, and memory for a Vulkan app.
function(configure_avd config)
    file(READ "${config}" text)

    foreach (setting IN ITEMS hw.gpu.enabled=yes hw.gpu.mode=auto hw.keyboard=yes
            hw.ramSize=2048 disk.dataPartition.size=4G)
        string(REGEX REPLACE "=.*" "" key "${setting}")
        string(REPLACE "." "\\." pattern "${key}")
        string(REGEX REPLACE "(^|\n)${pattern}[ ]*=[^\n]*" "" text "${text}")
        string(APPEND text "\n${setting}")
    endforeach ()

    string(REGEX REPLACE "^\n+" "" text "${text}")
    file(WRITE "${config}" "${text}\n")
endfunction()

if (emulator_wanted)
    execute_process(COMMAND "${sdk}/emulator/emulator" -list-avds
            OUTPUT_VARIABLE avds ERROR_QUIET)

    if (NOT avds MATCHES "(^|[\r\n])${avd}([\r\n]|$)")
        eacp_say("creating the AVD ${avd} (${eacp_android_image})")

        # avdmanager run the way its own launcher runs it, stdin empty for the
        # same reason: it asks whether to make a custom hardware profile. Its
        # output is shown only on failure: a good run still prints an "Error"
        # for the devices.xml the system image does not carry.
        file(TOUCH "${work}/empty")
        execute_process(
                COMMAND "${java}" "-Dcom.android.sdkmanager.toolsdir=${tools}"
                        -classpath "${tools}/lib/avdmanager-classpath.jar"
                        com.android.sdklib.tool.AvdManagerCli
                        create avd -n ${avd}
                        -k "${eacp_android_image}" -d pixel_8
                INPUT_FILE "${work}/empty"
                OUTPUT_VARIABLE output ERROR_VARIABLE output
                RESULT_VARIABLE failed)

        if (failed)
            message(NOTICE "${output}")
            eacp_fail("avdmanager could not create the AVD ${avd}")
        endif ()

        avd_home(home)

        if (NOT EXISTS "${home}/${avd}.avd/config.ini")
            eacp_fail("avdmanager made no ${home}/${avd}.avd/config.ini")
        endif ()

        configure_avd("${home}/${avd}.avd/config.ini")
    endif ()

    eacp_say("emulator and AVD ${avd} (${eacp_android_image})")
endif ()

file(REMOVE_RECURSE "${work}")

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
if (emulator_wanted)
    eacp_say("done. From the eacp checkout, run HelloGPU on a phone with USB "
            "debugging on (Android 13+, Vulkan 1.3), or with none attached on "
            "the AVD ${avd}, which the run boots:")
else ()
    eacp_say("done. Turn on USB debugging on a phone (Android 13+, Vulkan 1.3), "
            "plug it in, accept the prompt on it, and from the eacp checkout:")
endif ()

eacp_say("    cmake -G Ninja -B build-android -DCMAKE_BUILD_TYPE=Release "
        "-DCMAKE_SYSTEM_NAME=Android")
eacp_say("    cmake --build build-android --target HelloGPU-run")
eacp_say("adb is ${adb}")
eacp_say("ndk-stack is ${ndk_stack}")
