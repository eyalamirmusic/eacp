# The Android toolchain eacp builds with: the API levels, the NDK version an app
# locks to, and the Gradle the Android Studio project runs. Cached, so an app
# that fetches eacp reads them from its own directories too, and sets the first
# three to its own values.

set(EACP_ANDROID_NDK_VERSION "" CACHE STRING
        "The NDK version in the SDK to build with (empty: the newest installed)")
set(EACP_ANDROID_MIN_SDK 33 CACHE STRING
        "The oldest API level the app runs on, and the one a configure targets")
set(EACP_ANDROID_TARGET_SDK 35 CACHE STRING "targetSdk and compileSdk")
# The Android Gradle Plugin and the Gradle its wrapper runs
# (CMake/Android/wrapper holds that release's wrapper).
set(EACP_ANDROID_GRADLE_PLUGIN 9.4.1 CACHE INTERNAL "")
set(EACP_ANDROID_GRADLE 9.8.0 CACHE INTERNAL "")

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
