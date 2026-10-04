# What -DCMAKE_SYSTEM_NAME=Android configures with: the NDK EACP_ANDROID_NDK_VERSION
# names in the SDK eacp_android_find_sdk finds, else the one $ANDROID_NDK_HOME,
# $ANDROID_NDK_ROOT or $ANDROID_NDK points at, else the newest NDK in that SDK,
# for arm64-v8a at the lowest API level eacp supports unless the configure
# names others.

include("${CMAKE_CURRENT_LIST_DIR}/AndroidVersions.cmake")
eacp_android_find_sdk(eacp_android_sdk)

set(ANDROID_ABI arm64-v8a CACHE STRING "The ABI this build targets")
set(ANDROID_PLATFORM android-${EACP_ANDROID_MIN_SDK} CACHE STRING
        "The API level this build targets")

set(eacp_android_ndk "$ENV{ANDROID_NDK_HOME}")

if (NOT eacp_android_ndk)
    set(eacp_android_ndk "$ENV{ANDROID_NDK_ROOT}")
endif ()

if (NOT eacp_android_ndk)
    set(eacp_android_ndk "$ENV{ANDROID_NDK}")
endif ()

if (EACP_ANDROID_NDK_VERSION)
    set(eacp_android_ndk "${eacp_android_sdk}/ndk/${EACP_ANDROID_NDK_VERSION}")
    set(eacp_ndk_missing "No NDK ${EACP_ANDROID_NDK_VERSION} in ${eacp_android_sdk}")
elseif (eacp_android_ndk)
    set(eacp_ndk_missing "No NDK at ${eacp_android_ndk}")
else ()
    set(eacp_ndk_missing "No NDK in ${eacp_android_sdk}")
    file(GLOB eacp_android_ndks LIST_DIRECTORIES true "${eacp_android_sdk}/ndk/*")
    list(SORT eacp_android_ndks COMPARE NATURAL)

    foreach (ndk IN LISTS eacp_android_ndks)
        if (EXISTS "${ndk}/build/cmake/android.toolchain.cmake")
            set(eacp_android_ndk "${ndk}")
        endif ()
    endforeach ()
endif ()

file(TO_CMAKE_PATH "${eacp_android_ndk}" eacp_android_ndk)
set(eacp_ndk_toolchain "${eacp_android_ndk}/build/cmake/android.toolchain.cmake")

if (NOT EXISTS "${eacp_ndk_toolchain}")
    message(FATAL_ERROR
            "${eacp_ndk_missing}: install one with Android Studio's SDK Manager "
            "(SDK Tools, NDK (Side by side), with Show Package Details), set "
            "ANDROID_HOME to an SDK that has one, or ANDROID_NDK_HOME to an NDK.")
endif ()

include("${eacp_ndk_toolchain}")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES EACP_ANDROID_NDK_VERSION)
