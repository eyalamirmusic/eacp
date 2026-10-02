# Usage: cmake -DSDK=<sdk> -DAPK=<app.apk> -DPACKAGE=<id> -P Scripts/android-run.cmake
#              [-- --env <name>=<value> ...]
#
# Installs and launches an eacp APK on the device adb sees: a phone over USB,
# woken and unlocked where it has no PIN, or else an emulator it boots on a host
# that has one ($EACP_AVD, else the AVD eacp that android-setup makes, else the
# first AVD the emulator lists). Prints the
# app's first seconds of logcat (tag "eacp", plus any crash). Each --env becomes
# a string extra on the launch intent, which eacp sets as an environment variable
# before main() runs.

cmake_minimum_required(VERSION 3.31)

set(eacp_script android-run)
include("${CMAKE_CURRENT_LIST_DIR}/android-common.cmake")

set(launch_extras "")
set(past_separator FALSE)
set(expect_env FALSE)
math(EXPR last_arg "${CMAKE_ARGC} - 1")

foreach (index RANGE ${last_arg})
    set(arg "${CMAKE_ARGV${index}}")

    if (NOT past_separator)
        if (arg STREQUAL "--")
            set(past_separator TRUE)
        endif ()
    elseif (expect_env)
        if (NOT arg MATCHES "^([^=]+)=(.*)$")
            eacp_fail("--env takes <name>=<value>, not ${arg}")
        endif ()

        # adb hands the command to the device's shell, which would split an
        # unquoted value at its spaces.
        list(APPEND launch_extras --es "${CMAKE_MATCH_1}" "'${CMAKE_MATCH_2}'")
        set(expect_env FALSE)
    elseif (arg STREQUAL "--env")
        set(expect_env TRUE)
    else ()
        eacp_fail("unknown argument ${arg}; android-run takes --env <name>=<value>")
    endif ()
endforeach ()

if (expect_env)
    eacp_fail("--env needs <name>=<value> after it")
endif ()

find_program(adb adb HINTS "${SDK}/platform-tools" NO_DEFAULT_PATH NO_CACHE)

if (NOT adb)
    eacp_fail("no adb in ${SDK}/platform-tools: run cmake -P Scripts/android-setup.cmake")
endif ()

# Runs adb and keeps what it printed, stdout and stderr together, in <out>.
function(adb_output out)
    execute_process(COMMAND "${adb}" ${ARGN} OUTPUT_VARIABLE output
            ERROR_VARIABLE output RESULT_VARIABLE failed)
    set(${out} "${output}" PARENT_SCOPE)
    set(adb_failed "${failed}" PARENT_SCOPE)
endfunction()

function(no_device why)
    eacp_say("no device attached, and ${why}")
    eacp_fail("attach a phone with USB debugging on, check that 'adb devices' "
            "lists it as 'device', and run this again")
endfunction()

adb_output(state get-state)

if (adb_failed)
    adb_output(devices devices)

    if (devices MATCHES "unauthorized")
        eacp_fail("the phone has not allowed USB debugging from this computer; "
                "unlock it, accept the prompt there, and run this again")
    endif ()

    if (NOT eacp_android_has_emulator)
        no_device("Google ships no Android Emulator for ${eacp_android_os} on ARM")
    endif ()

    find_program(emulator emulator HINTS "${SDK}/emulator" NO_DEFAULT_PATH NO_CACHE)

    if (NOT emulator)
        no_device("this SDK has no emulator to boot; run "
                "cmake -P Scripts/android-setup.cmake (without "
                "-DEACP_ANDROID_EMULATOR=OFF) to install one and its AVD")
    endif ()

    set(avd "$ENV{EACP_AVD}")

    if (NOT avd)
        execute_process(COMMAND "${emulator}" -list-avds OUTPUT_VARIABLE avds)

        if (avds MATCHES "(^|[\r\n])eacp([\r\n]|$)")
            set(avd eacp)
        else ()
            string(REGEX MATCH "^[^\r\n]+" avd "${avds}")
        endif ()
    endif ()

    if (NOT avd)
        no_device("the emulator has no AVD to boot")
    endif ()

    eacp_say("booting emulator ${avd}")
    set(args -avd ${avd} -no-snapshot-save -no-audio -gpu host)

    # Detached, so the build does not wait for the emulator to exit: ninja
    # waits on every handle its child holds, so on Windows the emulator starts
    # through Start-Process rather than as a child of this one.
    if (CMAKE_HOST_WIN32)
        file(TO_NATIVE_PATH "${emulator}" emulator_native)
        list(JOIN args " " joined)
        execute_process(COMMAND powershell -NoProfile -Command
                "Start-Process -FilePath '${emulator_native}' -ArgumentList '${joined}'")
    else ()
        list(JOIN args " " joined)
        execute_process(COMMAND sh -c
                "nohup '${emulator}' ${joined} >/dev/null 2>&1 &")
    endif ()

    # A detached emulator that fails to start says nothing, so the wait is
    # bounded rather than adb's wait-for-device, which would hang the build.
    set(boot_timeout 300)

    if (DEFINED ENV{EACP_BOOT_TIMEOUT})
        set(boot_timeout $ENV{EACP_BOOT_TIMEOUT})
    endif ()

    string(TIMESTAMP start %s)
    math(EXPR deadline "${start} + ${boot_timeout}")

    while (TRUE)
        adb_output(booted shell getprop sys.boot_completed)

        if (booted MATCHES "^1")
            break()
        endif ()

        string(TIMESTAMP now %s)

        if (now GREATER_EQUAL deadline)
            eacp_fail("emulator ${avd} did not boot in ${boot_timeout}s; start it by "
                    "hand to see why: ${emulator} -avd ${avd}")
        endif ()

        eacp_sleep(2)
    endwhile ()
endif ()

# A phone that is asleep would show nothing: not the app, and not the Play
# Protect prompt an install can raise. Wake it and lift the keyguard, which
# only works with no PIN, pattern or password set; with one, the phone shows
# its unlock screen and the app waits behind it.
adb_output(ignored shell input keyevent KEYCODE_WAKEUP)
adb_output(ignored shell wm dismiss-keyguard)
eacp_sleep(1)
adb_output(window shell dumpsys window)

if (window MATCHES "isKeyguardShowing=true")
    eacp_say("the phone is locked with a PIN, pattern or password; unlock it to "
            "see the app")
endif ()

# A first install of a sideloaded app can make the phone ask about it (Google
# Play Protect, or an "install via USB" prompt), and adb install waits for the
# answer saying nothing, so the install is bounded and a first one says where
# to look.
function(install_apk out)
    adb_output(installed shell pm path "${PACKAGE}")

    if (NOT installed MATCHES "package:")
        eacp_say("installing ${PACKAGE} for the first time; if the phone asks "
                "about it (Play Protect, or install via USB), answer it there")
    endif ()

    execute_process(COMMAND "${adb}" install -r "${APK}"
            OUTPUT_VARIABLE output ERROR_VARIABLE output
            RESULT_VARIABLE failed TIMEOUT 120)
    string(STRIP "${output}" output)

    if (failed MATCHES "timeout")
        eacp_fail("install gave no answer in 120s; check the phone for a prompt, "
                "and run this again")
    endif ()

    set(${out} "${output}" PARENT_SCOPE)
    set(install_failed "${failed}" PARENT_SCOPE)
endfunction()

install_apk(output)

# Each machine makes its own debug keystore, so an APK built on another one
# cannot update this one in place: uninstall it (and its data) and try again.
if (install_failed)
    message(NOTICE "${output}")

    if (NOT output MATCHES "INSTALL_FAILED_UPDATE_INCOMPATIBLE")
        cmake_language(EXIT 1)
    endif ()

    eacp_say("${PACKAGE} is signed with another debug key; uninstalling it, and "
            "its data, to install this build")
    execute_process(COMMAND "${adb}" uninstall "${PACKAGE}")
    install_apk(output)

    if (install_failed)
        message(NOTICE "${output}")
        cmake_language(EXIT 1)
    endif ()
endif ()

message(NOTICE "${output}")

execute_process(COMMAND "${adb}" shell am force-stop "${PACKAGE}")
execute_process(COMMAND "${adb}" logcat -c)
execute_process(COMMAND "${adb}" shell am start -n "${PACKAGE}/android.app.NativeActivity"
        ${launch_extras})

set(log_seconds 3)

if (DEFINED ENV{EACP_RUN_LOG_SECONDS})
    set(log_seconds $ENV{EACP_RUN_LOG_SECONDS})
endif ()

eacp_sleep(${log_seconds})
execute_process(COMMAND "${adb}" logcat -d -s eacp AndroidRuntime DEBUG)

file(TO_NATIVE_PATH "${adb}" adb_native)
eacp_say("running ${PACKAGE}; follow its log with: ${adb_native} logcat -s eacp")
