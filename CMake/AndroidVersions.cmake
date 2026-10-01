# The one Android toolchain eacp builds with. The build and
# Scripts/android-setup.cmake both include this file, so a new NDK is one edit
# here.

set(EACP_ANDROID_NDK_VERSION 30.0.16248370)
set(EACP_ANDROID_BUILD_TOOLS 35.0.0)
# targetSdkVersion, and the android.jar the manifest links against.
set(EACP_ANDROID_TARGET_SDK 35)
# The command-line tools build that bootstraps sdkmanager: 22.0, the last
# whose sdkmanager is the Java one rather than a shim over the native Android
# CLI that 23.0 introduced.
set(EACP_ANDROID_CMDLINE_TOOLS 15859902)
