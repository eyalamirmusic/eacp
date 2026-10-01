# The android preset's toolchain file: the NDK named in AndroidVersions.cmake,
# in the SDK at $ANDROID_HOME, else in ~/.eacp/android/sdk, where
# `cmake -P Scripts/android-setup.cmake` puts one.

include("${CMAKE_CURRENT_LIST_DIR}/AndroidVersions.cmake")

if (DEFINED ENV{ANDROID_HOME})
    file(TO_CMAKE_PATH "$ENV{ANDROID_HOME}" eacp_android_sdk)
elseif (DEFINED ENV{HOME})
    file(TO_CMAKE_PATH "$ENV{HOME}/.eacp/android/sdk" eacp_android_sdk)
else ()
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}/.eacp/android/sdk" eacp_android_sdk)
endif ()

set(eacp_ndk_toolchain
        "${eacp_android_sdk}/ndk/${EACP_ANDROID_NDK_VERSION}/build/cmake/android.toolchain.cmake")

if (NOT EXISTS "${eacp_ndk_toolchain}")
    message(FATAL_ERROR
            "No NDK ${EACP_ANDROID_NDK_VERSION} in ${eacp_android_sdk}: "
            "run cmake -P Scripts/android-setup.cmake, which installs it there.")
endif ()

include("${eacp_ndk_toolchain}")
