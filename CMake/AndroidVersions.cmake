# The one Android toolchain eacp builds with: the NDK, the API levels, and the
# Gradle the Android Studio project runs. A new NDK is one edit here; the
# Studio project pins the same version, so Gradle and the plain configure
# compile with one toolchain.

set(EACP_ANDROID_NDK_VERSION 30.0.16248370)
# The oldest API level eacp runs on, and the one a configure targets by default.
set(EACP_ANDROID_MIN_SDK 33)
# targetSdk and compileSdk.
set(EACP_ANDROID_TARGET_SDK 35)
# The Android Gradle Plugin and the Gradle its wrapper runs
# (CMake/Android/wrapper holds that release's wrapper).
set(EACP_ANDROID_GRADLE_PLUGIN 9.4.1)
set(EACP_ANDROID_GRADLE 9.8.0)

# The SDK: $ANDROID_HOME, else $ANDROID_SDK_ROOT, else where Android Studio
# installs it on this host.
function(eacp_android_find_sdk out)
    set(sdk "$ENV{ANDROID_HOME}")

    if (NOT sdk)
        set(sdk "$ENV{ANDROID_SDK_ROOT}")
    endif ()

    if (NOT sdk)
        if (CMAKE_HOST_WIN32)
            set(sdk "$ENV{LOCALAPPDATA}/Android/Sdk")
        elseif (CMAKE_HOST_APPLE)
            set(sdk "$ENV{HOME}/Library/Android/sdk")
        else ()
            set(sdk "$ENV{HOME}/Android/Sdk")
        endif ()
    endif ()

    file(TO_CMAKE_PATH "${sdk}" sdk)
    set(${out} "${sdk}" PARENT_SCOPE)
endfunction()
