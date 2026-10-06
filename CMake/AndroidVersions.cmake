# The Android toolchain eacp builds with: the API levels, the NDK version an app
# locks to, and the Gradle the Android Studio project runs. Cached, so an app
# that fetches eacp reads them from its own directories too, and sets any of
# them to its own values.

set(EACP_ANDROID_NDK_VERSION "" CACHE STRING
        "The NDK version in the SDK to build with (empty: the newest installed)")
set(EACP_ANDROID_MIN_SDK 33 CACHE STRING
        "The oldest API level the app runs on, and the one a configure targets")
set(EACP_ANDROID_TARGET_SDK 35 CACHE STRING "targetSdk")
set(EACP_ANDROID_COMPILE_SDK "" CACHE STRING
        "compileSdk (empty: the newest platform installed in the SDK)")
# CMake/Android/wrapper holds the wrapper of EACP_ANDROID_GRADLE's release.
set(EACP_ANDROID_GRADLE_PLUGIN 9.4.1 CACHE STRING
        "The Android Gradle Plugin the Studio project applies")
set(EACP_ANDROID_GRADLE 9.8.0 CACHE STRING
        "The Gradle the Studio project's wrapper runs")

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

# The API level of the newest stable platform in <sdk>/platforms, as its
# source.properties names it (35, or 37.0 for an android-37.0), or empty.
function(eacp_android_newest_platform sdk out)
    file(GLOB platforms LIST_DIRECTORIES true "${sdk}/platforms/android-*")
    set(newest "")

    foreach (platform IN LISTS platforms)
        set(properties "${platform}/source.properties")

        if (NOT EXISTS "${properties}")
            continue()
        endif ()

        file(STRINGS "${properties}" level REGEX "^AndroidVersion\\.ApiLevel=")
        file(STRINGS "${properties}" codename REGEX "^AndroidVersion\\.CodeName=.")
        string(REGEX REPLACE "^[^=]*= *" "" level "${level}")

        if (codename OR NOT level MATCHES "^[0-9]+(\\.[0-9]+)?$")
            continue()
        endif ()

        if (NOT newest OR level VERSION_GREATER newest)
            set(newest "${level}")
        endif ()
    endforeach ()

    set(${out} "${newest}" PARENT_SCOPE)
endfunction()
