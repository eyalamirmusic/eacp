# -DCMAKE_TOOLCHAIN_FILE=CMake/AndroidToolchain.cmake: the NDK named in
# AndroidVersions.cmake, from the SDK eacp_android_find_sdk finds.

include("${CMAKE_CURRENT_LIST_DIR}/AndroidVersions.cmake")
eacp_android_find_sdk(eacp_android_sdk)

set(eacp_ndk_toolchain
        "${eacp_android_sdk}/ndk/${EACP_ANDROID_NDK_VERSION}/build/cmake/android.toolchain.cmake")

if (NOT EXISTS "${eacp_ndk_toolchain}")
    message(FATAL_ERROR
            "No NDK ${EACP_ANDROID_NDK_VERSION} in ${eacp_android_sdk}: "
            "run cmake -P Scripts/android-setup.cmake, which installs it there.")
endif ()

include("${eacp_ndk_toolchain}")
