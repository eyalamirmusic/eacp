# What the Android scripts share, included by each: the host, and where
# Scripts/android-setup.cmake puts the SDK and the JDK.
#
#   eacp_android_os    windows, mac or linux
#   eacp_android_arch  x64 or aarch64: the machine's, not this CMake's, so an
#                      x64 CMake on an ARM64 PC still says aarch64
#   eacp_android_dir   ~/.eacp/android
#   eacp_android_sdk   $ANDROID_HOME, else ~/.eacp/android/sdk

include("${CMAKE_CURRENT_LIST_DIR}/../CMake/AndroidVersions.cmake")

if (CMAKE_HOST_WIN32)
    set(eacp_android_os windows)
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}" eacp_home)
elseif (CMAKE_HOST_APPLE)
    set(eacp_android_os mac)
    set(eacp_home "$ENV{HOME}")
else ()
    set(eacp_android_os linux)
    set(eacp_home "$ENV{HOME}")
endif ()

set(eacp_android_arch x64)

if (eacp_android_os STREQUAL "windows")
    if ("$ENV{PROCESSOR_IDENTIFIER}" MATCHES "^ARM"
            OR "$ENV{PROCESSOR_ARCHITECTURE}" STREQUAL "ARM64")
        set(eacp_android_arch aarch64)
    endif ()
elseif (eacp_android_os STREQUAL "mac")
    # A CMake under Rosetta would see x86_64 in uname.
    execute_process(COMMAND sysctl -n hw.optional.arm64
            OUTPUT_VARIABLE arm64 OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

    if (arm64 STREQUAL "1")
        set(eacp_android_arch aarch64)
    endif ()
else ()
    cmake_host_system_information(RESULT machine QUERY OS_PLATFORM)

    if (machine MATCHES "^(aarch64|arm64)$")
        set(eacp_android_arch aarch64)
    endif ()
endif ()

set(eacp_android_dir "${eacp_home}/.eacp/android")

if (DEFINED ENV{ANDROID_HOME} AND NOT "$ENV{ANDROID_HOME}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{ANDROID_HOME}" eacp_android_sdk)
else ()
    set(eacp_android_sdk "${eacp_android_dir}/sdk")
endif ()

# Prints "<script>: <arguments, joined>" without CMake's own prefix. ARGV<n>
# rather than ARGN, which would split an argument at its semicolons.
function(eacp_say)
    set(text "")
    math(EXPR last "${ARGC} - 1")

    foreach (i RANGE ${last})
        string(APPEND text "${ARGV${i}}")
    endforeach ()

    message(NOTICE "${eacp_script}: ${text}")
endfunction()

function(eacp_fail)
    set(text "")
    math(EXPR last "${ARGC} - 1")

    foreach (i RANGE ${last})
        string(APPEND text "${ARGV${i}}")
    endforeach ()

    eacp_say("${text}")
    cmake_language(EXIT 1)
endfunction()

function(eacp_sleep seconds)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep ${seconds})
endfunction()

function(eacp_java_major java out)
    execute_process(COMMAND "${java}" -version
            ERROR_VARIABLE version OUTPUT_QUIET RESULT_VARIABLE failed)
    set(major 0)

    if (NOT failed AND version MATCHES "version \"([0-9]+)(\\.([0-9]+))?")
        set(major ${CMAKE_MATCH_1})

        if (major EQUAL 1)
            set(major ${CMAKE_MATCH_3})
        endif ()
    endif ()

    set(${out} ${major} PARENT_SCOPE)
endfunction()

# A Java 17 or later: $JAVA_HOME, the JDK android-setup installs, `java` on the
# PATH, or a JDK in one of the usual places on a Mac. Sets <out> to the java
# executable, or to nothing.
function(eacp_find_java out)
    set(homes "$ENV{JAVA_HOME}" "${eacp_android_dir}/jdk")

    if (eacp_android_os STREQUAL "mac")
        # /usr/bin/java there is a stub that fails without a JDK installed.
        execute_process(COMMAND /usr/libexec/java_home
                OUTPUT_VARIABLE java_home OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        list(APPEND homes "${java_home}"
                /opt/homebrew/opt/openjdk@21 /opt/homebrew/opt/openjdk
                /usr/local/opt/openjdk@21 /usr/local/opt/openjdk
                "/Applications/Android Studio.app/Contents/jbr/Contents/Home")
    endif ()

    set(candidates "")

    foreach (home IN LISTS homes)
        if (home)
            file(TO_CMAKE_PATH "${home}" home)
            list(APPEND candidates "${home}/bin/java" "${home}/bin/java.exe")
        endif ()
    endforeach ()

    find_program(path_java java NO_CACHE)
    list(APPEND candidates "${path_java}")

    foreach (java IN LISTS candidates)
        if (EXISTS "${java}" AND NOT IS_DIRECTORY "${java}")
            eacp_java_major("${java}" major)

            if (major GREATER_EQUAL 17)
                set(${out} "${java}" PARENT_SCOPE)
                return()
            endif ()
        endif ()
    endforeach ()

    set(${out} "" PARENT_SCOPE)
endfunction()
