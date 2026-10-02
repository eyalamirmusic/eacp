# The one Android toolchain eacp builds with. The build and
# Scripts/android-setup.cmake both include this file, so a new NDK is one edit
# here.

set(EACP_ANDROID_NDK_VERSION 30.0.16248370)
set(EACP_ANDROID_BUILD_TOOLS 35.0.0)
# The oldest API level eacp runs on, and the one a configure targets by default.
set(EACP_ANDROID_MIN_SDK 33)
# targetSdkVersion, and the android.jar the manifest links against.
set(EACP_ANDROID_TARGET_SDK 35)
# The emulator's system image: Google APIs at the target SDK, for the host's
# own ABI, which on Apple Silicon has Vulkan 1.3.
set(EACP_ANDROID_SYSTEM_IMAGE_TAG google_apis)
# The command-line tools build that bootstraps sdkmanager: 22.0, the last
# whose sdkmanager is the Java one rather than a shim over the native Android
# CLI that 23.0 introduced.
set(EACP_ANDROID_CMDLINE_TOOLS 15859902)
# The Android Studio project (CMake/AndroidStudio.cmake): the Android Gradle
# Plugin, the Gradle its wrapper runs, and the wrapper's own files, pinned by
# hash since they are fetched from Gradle's repository at that release's tag.
set(EACP_ANDROID_GRADLE_PLUGIN 9.4.1)
set(EACP_ANDROID_GRADLE 9.8.0)
set(EACP_ANDROID_GRADLE_WRAPPER_JAR_SHA256
        238e777fcddd7e34f9708186085def2abd6e08e658505b38718d79d74c21abd5)
set(EACP_ANDROID_GRADLEW_SHA256
        e01b5c97892572c82405c02b96a3382379100e7d825ce7c48883d95c26928750)
set(EACP_ANDROID_GRADLEW_BAT_SHA256
        ad2fac6060c5b929bed15d428e09483e52747d0120874346861ad4ec324af64c)
# bundletool, which <target>-aab builds the App Bundle with, from its GitHub
# release, pinned by hash.
set(EACP_ANDROID_BUNDLETOOL 1.18.3)
set(EACP_ANDROID_BUNDLETOOL_SHA256
        a099cfa1543f55593bc2ed16a70a7c67fe54b1747bb7301f37fdfd6d91028e29)

# The SDK that holds this NDK: Android Studio's, then $ANDROID_HOME, then
# ~/.eacp/android/sdk. With none, $ANDROID_HOME, else ~/.eacp/android/sdk, where
# Scripts/android-setup.cmake installs it.
function(eacp_android_find_sdk out)
    set(home "$ENV{HOME}")

    if (NOT home)
        set(home "$ENV{USERPROFILE}")
    endif ()

    set(fallback "${home}/.eacp/android/sdk")

    if (NOT "$ENV{ANDROID_HOME}" STREQUAL "")
        set(fallback "$ENV{ANDROID_HOME}")
    endif ()

    foreach (sdk "${home}/Library/Android/sdk" "$ENV{LOCALAPPDATA}/Android/Sdk"
            "${home}/Android/Sdk" "${fallback}" "${home}/.eacp/android/sdk")
        file(TO_CMAKE_PATH "${sdk}" sdk)

        if (EXISTS "${sdk}/ndk/${EACP_ANDROID_NDK_VERSION}")
            set(fallback "${sdk}")
            break()
        endif ()
    endforeach ()

    file(TO_CMAKE_PATH "${fallback}" fallback)
    set(${out} "${fallback}" PARENT_SCOPE)
endfunction()
