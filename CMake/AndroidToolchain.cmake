# What -DCMAKE_SYSTEM_NAME=Android configures with: the NDK named in
# AndroidVersions.cmake, from the SDK eacp_android_find_sdk finds, for arm64-v8a
# at the lowest API level eacp supports unless the configure names others.

include("${CMAKE_CURRENT_LIST_DIR}/AndroidVersions.cmake")
eacp_android_find_sdk(eacp_android_sdk)

set(ANDROID_ABI arm64-v8a CACHE STRING "The ABI this build targets")
set(ANDROID_PLATFORM android-${EACP_ANDROID_MIN_SDK} CACHE STRING
        "The API level this build targets")

set(eacp_ndk_toolchain
        "${eacp_android_sdk}/ndk/${EACP_ANDROID_NDK_VERSION}/build/cmake/android.toolchain.cmake")

if (NOT EXISTS "${eacp_ndk_toolchain}")
    message(FATAL_ERROR
            "No NDK ${EACP_ANDROID_NDK_VERSION} in ${eacp_android_sdk}: "
            "run cmake -P Scripts/android-setup.cmake, which installs it there.")
endif ()

include("${eacp_ndk_toolchain}")
